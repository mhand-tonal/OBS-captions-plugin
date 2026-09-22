#!/usr/bin/env python3
"""
NVIDIA Canary WebSocket streaming server for the OBS Captions plugin.

Speaks the same subset of sherpa-onnx's online-websocket-server protocol as
the Moonshine / Voxtral / Parakeet sibling servers — the plugin works
unchanged by swapping the URL.

Canary (180M-flash by default) is an NVIDIA transducer-family ASR model,
multilingual (EN/DE/ES/FR) with punctuation/capitalization. mlx-audio's
Canary support is currently offline-only (no native streaming decoder),
so we do the same re-decode-on-cadence pseudo-streaming pattern as the
Moonshine server.

Install (Python 3.10+, Apple Silicon):
    pip install mlx-audio websockets numpy

Run:
    python server.py --port 6009 --model mlx-community/canary-180m-flash
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
    print("ERROR: mlx-audio not installed. pip install mlx-audio", file=sys.stderr)
    sys.exit(1)


SAMPLE_RATE = 16000

SILENCE_RMS_THRESHOLD = 0.008
SILENCE_MS_FOR_ENDPOINT = 600
MIN_UTTERANCE_MS = 250
MAX_UTTERANCE_SEC = 20
DECODE_INTERVAL_MS = 400
MIN_DECODE_AUDIO_MS = 400


class StreamingSession:
    def __init__(self, model, executor, source_lang: str):
        self.model = model
        self.executor = executor
        self.source_lang = source_lang
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

    def silence_ms(self): return self.silence_samples * 1000.0 / SAMPLE_RATE
    def speech_ms(self): return self.speech_samples * 1000.0 / SAMPLE_RATE
    def buffer_ms(self): return self.buffer.size * 1000.0 / SAMPLE_RATE

    def should_endpoint(self) -> bool:
        return self.silence_ms() >= SILENCE_MS_FOR_ENDPOINT and self.speech_ms() >= MIN_UTTERANCE_MS

    def should_decode_interim(self) -> bool:
        if self.decode_in_flight: return False
        if self.speech_samples == 0: return False
        if self.buffer_ms() < MIN_DECODE_AUDIO_MS: return False
        if (time.monotonic() - self.last_decode_at) * 1000 < DECODE_INTERVAL_MS: return False
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
        t0 = time.monotonic()
        try:
            out = self.model.generate(
                mx.array(audio),
                source_lang=self.source_lang,
                target_lang=self.source_lang,
                use_pnc=True,
                stream=False,
            )
            text = out.text if hasattr(out, "text") else str(out)
            infer_ms = (time.monotonic() - t0) * 1000
            audio_ms = audio.size * 1000.0 / SAMPLE_RATE
            if infer_ms > audio_ms * 0.8:
                logging.warning(
                    "slow canary inference: %.0fms for %.0fms audio — may lag",
                    infer_ms, audio_ms)
            return text
        except Exception:
            logging.exception("canary generate failed")
            return ""

    def reset_utterance(self) -> None:
        self.buffer = np.zeros(0, dtype=np.float32)
        self.silence_samples = 0
        self.speech_samples = 0
        self.last_emitted_text = ""
        self.segment += 1


async def handle_client(ws, model, executor, source_lang):
    peer = getattr(ws, "remote_address", None) or ("?", "?")
    prefix = f"[{peer[0]}:{peer[1]}]"
    logging.info("%s connected", prefix)

    session = StreamingSession(model, executor, source_lang)

    async def emit(text, is_endpoint):
        payload = {"text": text, "segment": session.segment, "is_endpoint": is_endpoint}
        try:
            await ws.send(json.dumps(payload))
        except websockets.exceptions.ConnectionClosed:
            pass

    try:
        async for msg in ws:
            if isinstance(msg, str):
                if msg.strip() in ("Done!", "Done"): break
                continue
            if not isinstance(msg, (bytes, bytearray, memoryview)) or len(msg) < 4:
                continue
            samples = np.frombuffer(msg, dtype=np.float32).copy()
            if samples.size == 0 or not np.isfinite(samples).any():
                continue

            session.append(samples)

            if (session.buffer_ms() > MAX_UTTERANCE_SEC * 1000
                    and session.speech_ms() > MIN_UTTERANCE_MS):
                text = await session.decode()
                if text: await emit(text, True)
                logging.info("%s final(cap) seg=%d: %s", prefix, session.segment, text[:80])
                session.reset_utterance()
                continue

            if session.should_endpoint():
                text = await session.decode()
                if text: await emit(text, True)
                logging.info("%s final seg=%d: %s", prefix, session.segment, text[:80])
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
            if text: await emit(text, True)
    except Exception:
        logging.exception("%s flush failed", prefix)

    try:
        await ws.send("Done!")
    except Exception:
        pass
    logging.info("%s disconnected", prefix)


async def main_async(args):
    logging.info("Loading Canary model: %s", args.model)
    model = load_stt_model(args.model)
    logging.info("Model loaded, warming up...")
    _ = model.generate(
        mx.array(np.zeros(SAMPLE_RATE, dtype=np.float32)),
        source_lang=args.language, target_lang=args.language, use_pnc=True,
    )
    logging.info("Warm. Starting WebSocket server.")

    executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="canary-infer")

    async def handler(ws):
        await handle_client(ws, model, executor, args.language)

    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop_event.set)
        except NotImplementedError:
            pass

    async with websockets.serve(handler, args.host, args.port, max_size=8 * 1024 * 1024):
        logging.info("Listening on ws://%s:%d", args.host, args.port)
        await stop_event.wait()
    executor.shutdown(wait=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=6009)
    ap.add_argument(
        "--model",
        default="Mediform/canary-1b-v2-mlx-q8",
        help="HF repo id. Options today: Mediform/canary-1b-v2-mlx-q8 (8-bit, ~1B params), qfuxa/canary-mlx",
    )
    ap.add_argument(
        "--language", default="en",
        help="Source/target language: en|de|es|fr (default: en)",
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
