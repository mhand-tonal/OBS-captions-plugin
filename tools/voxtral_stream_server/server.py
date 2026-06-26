#!/usr/bin/env python3
"""
Voxtral-Mini-4B-Realtime WebSocket streaming server for the OBS Captions plugin.

Speaks the same subset of sherpa-onnx's online-websocket-server protocol as
the Moonshine server — the plugin works unchanged by just swapping the URL.

Runs the model locally on Apple Silicon via `mlx-audio` (Metal-accelerated).
Voxtral-Mini-4B-Realtime has a native streaming architecture — causal audio
encoder + time-delayed decoder — so its inference yields text incrementally
as audio is processed. We re-decode the current utterance on a fixed cadence;
each decode streams its deltas, so the user sees stable, monotonically
growing interim captions without heavy re-decode compute.

Install (Python 3.10+, Apple Silicon):
    pip install mlx-audio websockets numpy

Run:
    python server.py --port 6007 --model mlx-community/Voxtral-Mini-4B-Realtime-2602-4bit

The 4-bit quant runs well on ≥16 GB unified memory. Use the -fp16 variant
for higher quality if you have ≥24 GB.
"""

import argparse
import asyncio
import json
import logging
import signal
import sys
import time
from concurrent.futures import ThreadPoolExecutor

import numpy as np
import websockets

try:
    import mlx.core as mx
    from mlx_audio.stt import load as load_stt_model
except ImportError:
    print(
        "ERROR: mlx-audio not installed. Install with:\n"
        "    pip install mlx-audio",
        file=sys.stderr,
    )
    sys.exit(1)


SAMPLE_RATE = 16000

# VAD / streaming parameters — tune here if captions feel too eager or too lazy.
SILENCE_RMS_THRESHOLD = 0.008
SILENCE_MS_FOR_ENDPOINT = 600
MIN_UTTERANCE_MS = 250
MAX_UTTERANCE_SEC = 20
DECODE_INTERVAL_MS = 500        # Voxtral's decode is heavier than Moonshine's
MIN_DECODE_AUDIO_MS = 400
TRANSCRIPTION_DELAY_MS = 480    # sweet spot per Mistral's model card


class StreamingSession:
    def __init__(self, model, executor):
        self.model = model
        self.executor = executor

        self.buffer = np.zeros(0, dtype=np.float32)
        self.segment = 0
        self.last_emitted_text = ""
        self.silence_samples = 0
        self.speech_samples = 0
        self.last_decode_at = 0.0
        self.decode_in_flight = False

    def append(self, samples: np.ndarray) -> None:
        self.buffer = np.concatenate([self.buffer, samples])
        rms = float(np.sqrt(np.mean(samples ** 2))) if samples.size else 0.0
        if rms < SILENCE_RMS_THRESHOLD:
            self.silence_samples += samples.size
        else:
            self.silence_samples = 0
            self.speech_samples += samples.size

    def silence_ms(self) -> float:
        return self.silence_samples * 1000.0 / SAMPLE_RATE

    def speech_ms(self) -> float:
        return self.speech_samples * 1000.0 / SAMPLE_RATE

    def buffer_ms(self) -> float:
        return self.buffer.size * 1000.0 / SAMPLE_RATE

    def should_endpoint(self) -> bool:
        return (
            self.silence_ms() >= SILENCE_MS_FOR_ENDPOINT
            and self.speech_ms() >= MIN_UTTERANCE_MS
        )

    def should_decode_interim(self) -> bool:
        if self.decode_in_flight:
            return False
        if self.speech_samples == 0:
            return False
        if self.buffer_ms() < MIN_DECODE_AUDIO_MS:
            return False
        if (time.monotonic() - self.last_decode_at) * 1000 < DECODE_INTERVAL_MS:
            return False
        return True

    async def decode(self) -> str:
        if self.buffer.size == 0:
            return ""
        audio = self.buffer.copy()
        self.last_decode_at = time.monotonic()
        self.decode_in_flight = True
        loop = asyncio.get_running_loop()
        try:
            text = await loop.run_in_executor(self.executor, self._infer, audio)
        finally:
            self.decode_in_flight = False
        return (text or "").strip()

    def _infer(self, audio: np.ndarray) -> str:
        """Run one full decode of `audio` and return the final transcript.

        Voxtral's generate(stream=True) yields text deltas; we concatenate
        them here. We don't emit per-delta because each decode operates on
        the *whole* utterance buffer, so intermediate deltas aren't interim
        in a useful sense — only the final concatenation for this cycle is.
        """
        audio_mx = mx.array(audio)
        try:
            gen = self.model.generate(
                audio_mx,
                stream=True,
                transcription_delay_ms=TRANSCRIPTION_DELAY_MS,
            )
            parts = []
            for delta in gen:
                if delta:
                    parts.append(delta)
            return "".join(parts)
        except Exception:
            logging.exception("voxtral generate failed")
            return ""

    def reset_utterance(self) -> None:
        self.buffer = np.zeros(0, dtype=np.float32)
        self.silence_samples = 0
        self.speech_samples = 0
        self.last_emitted_text = ""
        self.segment += 1


async def handle_client(ws, model, executor):
    peer = getattr(ws, "remote_address", None) or ("?", "?")
    prefix = f"[{peer[0]}:{peer[1]}]"
    logging.info("%s connected", prefix)

    session = StreamingSession(model, executor)

    async def emit(text: str, is_endpoint: bool) -> None:
        payload = {
            "text": text,
            "segment": session.segment,
            "is_endpoint": is_endpoint,
        }
        try:
            await ws.send(json.dumps(payload))
        except websockets.exceptions.ConnectionClosed:
            pass

    try:
        async for msg in ws:
            if isinstance(msg, str):
                if msg.strip() in ("Done!", "Done"):
                    break
                continue
            if not isinstance(msg, (bytes, bytearray, memoryview)):
                continue
            if len(msg) < 4:
                continue

            samples = np.frombuffer(msg, dtype=np.float32).copy()
            if samples.size == 0 or not np.isfinite(samples).any():
                continue

            session.append(samples)

            if (
                session.buffer_ms() > MAX_UTTERANCE_SEC * 1000
                and session.speech_ms() > MIN_UTTERANCE_MS
            ):
                logging.info("%s max-utterance cap, forcing endpoint", prefix)
                text = await session.decode()
                if text:
                    await emit(text, True)
                session.reset_utterance()
                continue

            if session.should_endpoint():
                text = await session.decode()
                if text:
                    await emit(text, True)
                session.reset_utterance()
                continue

            if session.should_decode_interim():
                text = await session.decode()
                if text and text != session.last_emitted_text:
                    session.last_emitted_text = text
                    await emit(text, False)

    except websockets.exceptions.ConnectionClosed:
        pass
    except Exception:
        logging.exception("%s handler error", prefix)

    try:
        if session.buffer_ms() > MIN_UTTERANCE_MS and session.speech_ms() > 0:
            text = await session.decode()
            if text:
                await emit(text, True)
    except Exception:
        logging.exception("%s flush-on-close failed", prefix)

    try:
        await ws.send("Done!")
    except Exception:
        pass

    logging.info("%s disconnected", prefix)


async def main_async(args):
    logging.info("Loading Voxtral model: %s", args.model)
    model = load_stt_model(args.model)
    logging.info("Model loaded, warming up...")

    # Warm up by running inference on 1 s of silence so the first real decode
    # doesn't pay Metal-kernel-compile / graph-trace latency.
    _ = model.generate(
        mx.array(np.zeros(SAMPLE_RATE, dtype=np.float32)),
        stream=False,
        transcription_delay_ms=TRANSCRIPTION_DELAY_MS,
    )
    logging.info("Warm. Starting WebSocket server.")

    # Single worker: MLX/Metal on Apple Silicon serializes GPU work anyway,
    # so running multiple decodes in parallel would just cause contention.
    executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="voxtral-infer")

    async def handler(ws):
        await handle_client(ws, model, executor)

    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop_event.set)
        except NotImplementedError:
            pass

    async with websockets.serve(
        handler, args.host, args.port, max_size=8 * 1024 * 1024
    ):
        logging.info("Listening on ws://%s:%d", args.host, args.port)
        await stop_event.wait()
        logging.info("Shutting down")

    executor.shutdown(wait=False)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--host", default="localhost", help="bind address (default: localhost)")
    ap.add_argument("--port", type=int, default=6007, help="bind port (default: 6007)")
    ap.add_argument(
        "--model",
        default="mlx-community/Voxtral-Mini-4B-Realtime-2602-4bit",
        help="HF repo id of an MLX Voxtral realtime model (default: 4-bit)",
    )
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
