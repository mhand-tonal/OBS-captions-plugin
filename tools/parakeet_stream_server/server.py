#!/usr/bin/env python3
"""
Parakeet-TDT WebSocket streaming server for the OBS Captions plugin.

Speaks the same subset of sherpa-onnx's online-websocket-server protocol as
the Moonshine and Voxtral sibling servers — the plugin works unchanged by
swapping the URL.

This one actually streams end-to-end: NVIDIA's Parakeet-TDT has a native
streaming decoder (via `parakeet-mlx`'s `transcribe_stream()`), so new audio
chunks extend the encoder state incrementally — no re-decoding a growing
buffer. Should feel much more responsive than the Moonshine/Voxtral servers.

Install (Python 3.10+, Apple Silicon):
    pip install parakeet-mlx websockets numpy

Run:
    python server.py --port 6008 --model mlx-community/parakeet-tdt-0.6b-v2

Model: ~600M params, ~2 GB unified memory, English. For multilingual,
try parakeet-tdt-0.6b-v3 (when an MLX port lands).
"""

import argparse
import asyncio
import json
import logging
import signal
import sys
import time

import numpy as np
import websockets

try:
    import mlx.core as mx
    from parakeet_mlx import from_pretrained
except ImportError:
    print(
        "ERROR: parakeet-mlx not installed. Install with:\n"
        "    pip install parakeet-mlx",
        file=sys.stderr,
    )
    sys.exit(1)


SAMPLE_RATE = 16000

# VAD / streaming parameters.
SILENCE_RMS_THRESHOLD = 0.008
SILENCE_MS_FOR_ENDPOINT = 600
MIN_UTTERANCE_MS = 250
MAX_UTTERANCE_SEC = 30
EMIT_INTERVAL_MS = 150   # how often to poll streamer.result.text for changes
CONTEXT_LEFT = 256       # left-context frames kept in attention cache
CONTEXT_RIGHT = 256      # right-context window
BATCH_AUDIO_MS = 500     # accumulate this many ms before feeding add_audio()


async def handle_client(ws, model):
    peer = getattr(ws, "remote_address", None) or ("?", "?")
    prefix = f"[{peer[0]}:{peer[1]}]"
    logging.info("%s connected", prefix)

    segment = 0
    last_emitted_text = ""
    silence_samples = 0
    speech_samples = 0
    last_poll_at = 0.0
    chunks_received = 0
    last_status_at = time.monotonic()

    async def emit(text: str, is_endpoint: bool, seg: int, words=None) -> None:
        payload = {"text": text, "segment": seg, "is_endpoint": is_endpoint}
        if words:
            payload["words"] = words
        try:
            await ws.send(json.dumps(payload))
        except websockets.exceptions.ConnectionClosed:
            pass

    def extract_words():
        """Pull [{"word","start","end"}, ...] from the streamer's current result."""
        try:
            out = []
            for sent in (streamer.result.sentences or []):
                for tok in (sent.tokens or []):
                    txt = (tok.text or "").strip()
                    if not txt:
                        continue
                    out.append({
                        "word": txt,
                        "start": float(tok.start),
                        "end": float(tok.end),
                    })
            return out
        except Exception:
            return []

    def new_streamer():
        return model.transcribe_stream(
            context_size=(CONTEXT_LEFT, CONTEXT_RIGHT), depth=1
        ).__enter__()

    def close_streamer(streamer):
        try:
            streamer.__exit__(None, None, None)
        except Exception:
            logging.exception("streamer close failed")

    streamer = new_streamer()
    utterance_samples = 0
    pending = np.zeros(0, dtype=np.float32)
    batch_target = int(SAMPLE_RATE * BATCH_AUDIO_MS / 1000)

    try:
        async for msg in ws:
            if isinstance(msg, str):
                if msg.strip() in ("Done!", "Done"):
                    break
                continue
            if not isinstance(msg, (bytes, bytearray, memoryview)) or len(msg) < 4:
                continue

            samples = np.frombuffer(msg, dtype=np.float32).copy()
            if samples.size == 0 or not np.isfinite(samples).any():
                continue

            # VAD bookkeeping on the incoming chunk.
            rms = float(np.sqrt(np.mean(samples ** 2)))
            if rms < SILENCE_RMS_THRESHOLD:
                silence_samples += samples.size
            else:
                silence_samples = 0
                speech_samples += samples.size
            utterance_samples += samples.size

            chunks_received += 1
            if chunks_received == 1:
                logging.info("%s first audio chunk: %d samples, rms=%.4f",
                             prefix, samples.size, rms)

            # Batch tiny OBS frames before invoking the model. Each add_audio
            # call does a forward pass, so firing it on every ~20 ms frame
            # falls behind realtime. Accumulate ~200 ms, then feed.
            pending = np.concatenate([pending, samples])
            if pending.size < batch_target:
                continue

            t0 = time.monotonic()
            try:
                streamer.add_audio(mx.array(pending))
            except Exception:
                logging.exception("%s add_audio failed", prefix)
                pending = np.zeros(0, dtype=np.float32)
                continue
            infer_ms = (time.monotonic() - t0) * 1000
            audio_ms = pending.size * 1000.0 / SAMPLE_RATE
            if infer_ms > audio_ms * 0.8:
                logging.warning(
                    "%s slow inference: %.0fms for %.0fms audio — may lag",
                    prefix, infer_ms, audio_ms)
            pending = np.zeros(0, dtype=np.float32)

            # Heartbeat log every ~3 s so we can see whether audio is flowing
            # and what the streamer currently thinks the transcript is.
            if time.monotonic() - last_status_at > 3.0:
                try:
                    cur = (streamer.result.text or "").strip()
                except Exception:
                    cur = "<err>"
                logging.info(
                    "%s hb: chunks=%d speech=%.1fs silence=%.1fs text=%r",
                    prefix, chunks_received,
                    speech_samples * 1000.0 / SAMPLE_RATE / 1000,
                    silence_samples * 1000.0 / SAMPLE_RATE / 1000,
                    cur[:120])
                last_status_at = time.monotonic()

            # Periodically read current result and emit as interim.
            now = time.monotonic()
            if (now - last_poll_at) * 1000 >= EMIT_INTERVAL_MS:
                last_poll_at = now
                try:
                    text = (streamer.result.text or "").strip()
                except Exception:
                    text = ""
                if text and text != last_emitted_text:
                    last_emitted_text = text
                    await emit(text, False, segment)

            # Endpoint detection.
            silence_ms = silence_samples * 1000.0 / SAMPLE_RATE
            speech_ms = speech_samples * 1000.0 / SAMPLE_RATE
            hit_cap = (
                utterance_samples * 1000.0 / SAMPLE_RATE > MAX_UTTERANCE_SEC * 1000
                and speech_ms > MIN_UTTERANCE_MS
            )
            hit_endpoint = (
                silence_ms >= SILENCE_MS_FOR_ENDPOINT
                and speech_ms >= MIN_UTTERANCE_MS
            )
            if hit_cap or hit_endpoint:
                try:
                    text = (streamer.result.text or "").strip()
                    words = extract_words()
                except Exception:
                    text, words = "", []
                if text:
                    await emit(text, True, segment, words)
                    logging.info("%s final seg=%d (%d words): %s",
                                 prefix, segment, len(words), text[:80])
                # Reset for next utterance.
                close_streamer(streamer)
                streamer = new_streamer()
                segment += 1
                last_emitted_text = ""
                silence_samples = 0
                speech_samples = 0
                utterance_samples = 0
                last_poll_at = 0.0

    except websockets.exceptions.ConnectionClosed:
        pass
    except Exception:
        logging.exception("%s handler error", prefix)

    # Flush any pending transcript on disconnect.
    try:
        text = (streamer.result.text or "").strip()
        if text and text != last_emitted_text:
            await emit(text, True, segment, extract_words())
    except Exception:
        pass
    close_streamer(streamer)

    try:
        await ws.send("Done!")
    except Exception:
        pass
    logging.info("%s disconnected", prefix)


async def main_async(args):
    logging.info("Loading Parakeet model: %s", args.model)
    model = from_pretrained(args.model)
    logging.info("Model loaded, warming up...")

    # Warm up with one second of silence so the first real chunk doesn't
    # pay Metal-kernel-compile latency.
    with model.transcribe_stream(
        context_size=(CONTEXT_LEFT, CONTEXT_RIGHT), depth=1
    ) as warm:
        warm.add_audio(mx.array(np.zeros(SAMPLE_RATE, dtype=np.float32)))
        _ = warm.result
    logging.info("Warm. Starting WebSocket server.")

    async def handler(ws):
        await handle_client(ws, model)

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


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=6008)
    ap.add_argument(
        "--model",
        default="mlx-community/parakeet-tdt-0.6b-v2",
        help="HF repo id of an MLX Parakeet model (default: parakeet-tdt-0.6b-v2)",
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
