#!/usr/bin/env python3
"""
Moonshine WebSocket streaming server for the OBS Captions plugin.

Speaks a subset of sherpa-onnx's online-websocket-server protocol so the
plugin's sherpa-onnx backend can connect unchanged:

  Client -> server : binary WebSocket frames, float32 PCM @ 16 kHz mono.
  Server -> client : text WebSocket frames containing JSON of the form
                     {"text": "...", "segment": N, "is_endpoint": bool}
                     plus the literal string "Done!" at stream end.

Streaming is done via re-decoding the current utterance every ~250 ms
(Moonshine-base on an M-series CPU handles this comfortably). Utterance
boundaries are detected with a simple amplitude-based VAD; at each
boundary a final result is emitted and the buffer resets.

Install (Python 3.10+):
    pip install useful-moonshine-onnx websockets numpy tokenizers

Run:
    python server.py --port 6006 --model moonshine/base

Tweak --model to moonshine/tiny for lower latency at some accuracy cost.
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
    from moonshine_onnx import MoonshineOnnxModel, load_tokenizer
except ImportError:
    try:
        from useful_moonshine_onnx import MoonshineOnnxModel, load_tokenizer
    except ImportError:
        print(
            "ERROR: could not import moonshine_onnx. Install with:\n"
            "    pip install useful-moonshine-onnx",
            file=sys.stderr,
        )
        sys.exit(1)


SAMPLE_RATE = 16000

# VAD / streaming parameters — tune here if captions feel too eager or too lazy.
SILENCE_RMS_THRESHOLD = 0.008       # RMS below this counts as silence
SILENCE_MS_FOR_ENDPOINT = 600       # trailing silence that triggers a final
MIN_UTTERANCE_MS = 250              # ignore sub-250ms blips (e.g. mouse clicks)
MAX_UTTERANCE_SEC = 20              # hard cap — force an endpoint past this
DECODE_INTERVAL_MS = 250            # minimum gap between interim decodes
MIN_DECODE_AUDIO_MS = 300           # don't bother decoding shorter buffers


class StreamingSession:
    def __init__(self, model, tokenizer, executor):
        self.model = model
        self.tokenizer = tokenizer
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
            tokens = await loop.run_in_executor(self.executor, self._infer, audio)
        finally:
            self.decode_in_flight = False

        if tokens is None:
            return ""
        try:
            texts = self.tokenizer.decode_batch(tokens)
        except AttributeError:
            # Some tokenizer variants expose .decode(ids) not .decode_batch
            texts = [self.tokenizer.decode(seq) for seq in tokens]
        return (texts[0] if texts else "").strip()

    def _infer(self, audio: np.ndarray):
        # Moonshine expects shape (batch, samples), float32, 16 kHz.
        return self.model.generate(audio[np.newaxis, :])

    def reset_utterance(self) -> None:
        self.buffer = np.zeros(0, dtype=np.float32)
        self.silence_samples = 0
        self.speech_samples = 0
        self.last_emitted_text = ""
        self.segment += 1


async def handle_client(ws, model, tokenizer, executor):
    peer = getattr(ws, "remote_address", None) or ("?", "?")
    prefix = f"[{peer[0]}:{peer[1]}]"
    logging.info("%s connected", prefix)

    session = StreamingSession(model, tokenizer, executor)

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
                # The sherpa-onnx client has no meaningful text messages,
                # but be tolerant: "Done!" from the client means "we're done".
                if msg.strip() in ("Done!", "Done"):
                    break
                continue
            if not isinstance(msg, (bytes, bytearray, memoryview)):
                continue
            if len(msg) < 4:
                continue

            samples = np.frombuffer(msg, dtype=np.float32).copy()
            if samples.size == 0:
                continue

            # Basic sanity — reject frames where nearly every sample is NaN/inf.
            if not np.isfinite(samples).any():
                continue

            session.append(samples)

            # 1. Hard cap: force an endpoint on runaway utterances.
            if (
                session.buffer_ms() > MAX_UTTERANCE_SEC * 1000
                and session.speech_ms() > MIN_UTTERANCE_MS
            ):
                logging.info("%s max-utterance cap reached, forcing endpoint", prefix)
                text = await session.decode()
                if text:
                    await emit(text, True)
                session.reset_utterance()
                continue

            # 2. End-of-utterance via trailing silence.
            if session.should_endpoint():
                text = await session.decode()
                if text:
                    await emit(text, True)
                session.reset_utterance()
                continue

            # 3. Periodic interim decode.
            if session.should_decode_interim():
                text = await session.decode()
                if text and text != session.last_emitted_text:
                    session.last_emitted_text = text
                    await emit(text, False)

    except websockets.exceptions.ConnectionClosed:
        pass
    except Exception:
        logging.exception("%s handler error", prefix)

    # Flush any trailing audio as a final result on disconnect.
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
    logging.info("Loading Moonshine model: %s", args.model)
    model = MoonshineOnnxModel(model_name=args.model)
    tokenizer = load_tokenizer()
    logging.info("Model loaded, warming up...")

    # Warm the ONNX session with one second of silence so the first real
    # inference doesn't pay graph-compile latency.
    _ = model.generate(np.zeros((1, SAMPLE_RATE), dtype=np.float32))
    logging.info("Warm. Starting WebSocket server.")

    executor = ThreadPoolExecutor(
        max_workers=args.workers, thread_name_prefix="moonshine-infer"
    )

    async def handler(ws):
        await handle_client(ws, model, tokenizer, executor)

    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop_event.set)
        except NotImplementedError:
            pass

    # max_size: generous cap — OBS can send largeish audio chunks when catching
    # up after a stall. 8 MiB is ~130 s of float32 @ 16 kHz, well beyond normal.
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
    ap.add_argument("--port", type=int, default=6006, help="bind port (default: 6006)")
    ap.add_argument(
        "--model",
        default="moonshine/base",
        help="moonshine/base or moonshine/tiny (default: moonshine/base)",
    )
    ap.add_argument(
        "--workers",
        type=int,
        default=2,
        help="inference thread-pool size (default: 2)",
    )
    ap.add_argument("--verbose", "-v", action="store_true", help="debug logging")
    args = ap.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
