#!/usr/bin/env python3
"""
OpenAI Whisper (via MLX) WebSocket streaming server for OBS Captions.

Speaks the same subset of sherpa-onnx's online-websocket-server protocol as
the Moonshine / Parakeet sibling servers — the plugin works unchanged by
swapping the URL.

Whisper is an offline encoder-decoder (not streaming-native), so we do the
same re-decode-on-cadence pseudo-streaming pattern as the Moonshine server:
accumulate audio, decode the growing utterance on a fixed cadence, emit as
interim; VAD-gated final on silence.

Install (Python 3.10+, Apple Silicon):
    pip install mlx-whisper websockets numpy

Run:
    python server.py --port 6010 --model mlx-community/whisper-base-mlx

Model recommendations (for your M4 Pro):
    mlx-community/whisper-tiny                  ~10x realtime, English-rough
    mlx-community/whisper-base-mlx                  ~5x  realtime, decent quality  (default)
    mlx-community/whisper-small                 ~2-3x realtime, solid multilingual
    mlx-community/whisper-small.en-mlx-q4       quantized small English-only, faster
    mlx-community/whisper-large-v3-turbo        ~1.5x realtime, near-SOTA accuracy
    mlx-community/distil-whisper-large-v3       ~1.5-2x realtime, English-focused
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
    import mlx_whisper
except ImportError:
    print("ERROR: mlx-whisper not installed. pip install mlx-whisper", file=sys.stderr)
    sys.exit(1)


SAMPLE_RATE = 16000

# VAD / streaming parameters — same shape as the Moonshine server.
SILENCE_RMS_THRESHOLD = 0.008
SILENCE_MS_FOR_ENDPOINT = 600
MIN_UTTERANCE_MS = 250
MAX_UTTERANCE_SEC = 20
DECODE_INTERVAL_MS = 400
MIN_DECODE_AUDIO_MS = 400


class StreamingSession:
    def __init__(self, model_repo: str, executor, language):
        self.model_repo = model_repo
        self.executor = executor
        self.language = language
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
        return (
            self.silence_ms() >= SILENCE_MS_FOR_ENDPOINT
            and self.speech_ms() >= MIN_UTTERANCE_MS
        )

    def should_decode_interim(self) -> bool:
        if self.decode_in_flight: return False
        if self.speech_samples == 0: return False
        if self.buffer_ms() < MIN_DECODE_AUDIO_MS: return False
        if (time.monotonic() - self.last_decode_at) * 1000 < DECODE_INTERVAL_MS: return False
        return True

    async def decode(self, with_words: bool = False):
        """Returns (text, words). `words` is a list of {"word","start","end"} dicts
        when with_words=True, else empty."""
        if self.buffer.size == 0:
            return "", []
        audio = self.buffer.copy()
        self.last_decode_at = time.monotonic()
        self.decode_in_flight = True
        loop = asyncio.get_running_loop()
        try:
            text, words = await loop.run_in_executor(
                self.executor, self._infer, audio, with_words)
        finally:
            self.decode_in_flight = False
        return (text or "").strip(), words

    def _infer(self, audio: np.ndarray, with_words: bool):
        t0 = time.monotonic()
        try:
            # Key tweaks for live captioning vs. batch transcription:
            # - condition_on_previous_text=False: each decode is independent,
            #   prevents runaway hallucination cascades between cycles.
            # - temperature=0.0: greedy decode, deterministic.
            # - word_timestamps: True only on finals (costs a bit extra).
            result = mlx_whisper.transcribe(
                audio,
                path_or_hf_repo=self.model_repo,
                language=self.language,
                condition_on_previous_text=False,
                temperature=0.0,
                word_timestamps=with_words,
                verbose=None,
            )
            text = result.get("text", "") if isinstance(result, dict) else str(result)
            words = []
            if with_words and isinstance(result, dict):
                for seg in result.get("segments") or []:
                    for w in seg.get("words") or []:
                        word_text = (w.get("word") or "").strip()
                        if not word_text:
                            continue
                        words.append({
                            "word": word_text,
                            "start": float(w.get("start", 0.0)),
                            "end": float(w.get("end", 0.0)),
                        })
            infer_ms = (time.monotonic() - t0) * 1000
            audio_ms = audio.size * 1000.0 / SAMPLE_RATE
            if infer_ms > audio_ms * 0.8:
                logging.warning(
                    "slow whisper inference: %.0fms for %.0fms audio — may lag",
                    infer_ms, audio_ms)
            return text, words
        except Exception:
            logging.exception("whisper transcribe failed")
            return "", []

    def reset_utterance(self) -> None:
        self.buffer = np.zeros(0, dtype=np.float32)
        self.silence_samples = 0
        self.speech_samples = 0
        self.last_emitted_text = ""
        self.segment += 1


async def handle_client(ws, model_repo, executor, language):
    peer = getattr(ws, "remote_address", None) or ("?", "?")
    prefix = f"[{peer[0]}:{peer[1]}]"
    logging.info("%s connected", prefix)

    session = StreamingSession(model_repo, executor, language)

    async def emit(text, is_endpoint, words=None):
        payload = {"text": text, "segment": session.segment, "is_endpoint": is_endpoint}
        if words:
            payload["words"] = words
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
                text, words = await session.decode(with_words=True)
                if text: await emit(text, True, words)
                logging.info("%s final(cap) seg=%d (%d words): %s",
                             prefix, session.segment, len(words), text[:80])
                session.reset_utterance()
                continue

            if session.should_endpoint():
                text, words = await session.decode(with_words=True)
                if text: await emit(text, True, words)
                logging.info("%s final seg=%d (%d words): %s",
                             prefix, session.segment, len(words), text[:80])
                session.reset_utterance()
                continue

            if session.should_decode_interim():
                text, _ = await session.decode(with_words=False)
                if text and text != session.last_emitted_text:
                    session.last_emitted_text = text
                    await emit(text, False)

    except websockets.exceptions.ConnectionClosed:
        pass
    except Exception:
        logging.exception("%s handler error", prefix)

    try:
        if session.buffer_ms() > MIN_UTTERANCE_MS and session.speech_ms() > 0:
            text, words = await session.decode(with_words=True)
            if text: await emit(text, True, words)
    except Exception:
        logging.exception("%s flush failed", prefix)

    try:
        await ws.send("Done!")
    except Exception:
        pass
    logging.info("%s disconnected", prefix)


async def main_async(args):
    logging.info("Loading Whisper model: %s", args.model)
    # mlx_whisper caches on first use; warm by transcribing 1s of silence.
    logging.info("Warming up (first run downloads weights from HF) ...")
    _ = mlx_whisper.transcribe(
        np.zeros(SAMPLE_RATE, dtype=np.float32),
        path_or_hf_repo=args.model,
        language=args.language,
        condition_on_previous_text=False,
        temperature=0.0,
        word_timestamps=False,
        verbose=None,
    )
    logging.info("Warm. Starting WebSocket server.")

    # Single worker: MLX/Metal serializes on the GPU anyway.
    executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="whisper-infer")

    async def handler(ws):
        await handle_client(ws, args.model, executor, args.language)

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
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=6010)
    ap.add_argument(
        "--model",
        default="mlx-community/whisper-base-mlx",
        help="HF repo id of an MLX Whisper model (default: whisper-base)",
    )
    ap.add_argument(
        "--language", default="en",
        help="Language code (default: en). Use None/auto for auto-detect.",
    )
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    if args.language and args.language.lower() in ("none", "auto", ""):
        args.language = None

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
