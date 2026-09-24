# Local WebSocket ASR — Wire Protocol

This document specifies the protocol the OBS captions plugin speaks to any
local ASR server selected via the **Local WebSocket** provider. A server is
compatible if it follows this spec.

The protocol is a minimal subset of sherpa-onnx's `online-websocket-server`
protocol, extended with `is_endpoint` and `words[]` fields.

---

## Transport

- Plain WebSocket (RFC 6455) over TCP. No TLS (`ws://`). No authentication.
- Default port: `6006` (configurable via the plugin's Server URL field).
- Upgrade path accepts any URL path (defaults to `/`). No required query params or headers beyond the standard WebSocket handshake (`Upgrade: websocket`, `Sec-WebSocket-Key`, `Sec-WebSocket-Version: 13`, `Host`).

---

## Client → Server (audio)

**Binary WebSocket frames** containing raw **32-bit float PCM** audio:

| Property | Value |
|---|---|
| Sample format | `float32` little-endian (IEEE-754) |
| Sample rate | 16 000 Hz |
| Channels | 1 (mono) |
| Frame payload | arbitrary length; servers must accept any chunk size, including small (~20 ms / ~320 samples) and large (seconds-worth) frames |
| Range | nominally `[-1.0, +1.0]`; servers should tolerate minor clipping |

The plugin resamples OBS audio to the above format before sending. Chunks
are sent as they arrive from OBS's audio callback (typically every
10–50 ms). Servers should stream-process, not buffer-until-endpoint.

The client does not send a termination sentinel; it just closes the
WebSocket when the user stops captioning.

---

## Server → Client (transcripts)

**Text WebSocket frames.** Each frame is either:

1. **A JSON object** describing the current transcript state — the recommended format.
2. **Bare transcript text** (legacy sherpa-onnx behavior). Accepted but no interim/final metadata is available.
3. **The literal string `"Done!"`** — signals the server is closing the connection. Client responds with a WebSocket close frame.

### JSON schema

```json
{
  "text":        "the current transcript",
  "segment":     0,
  "is_endpoint": false,
  "words": [
    {"word": "hello", "start": 0.10, "end": 0.42},
    {"word": "world", "start": 0.45, "end": 0.80}
  ]
}
```

| Field | Type | Required | Semantics |
|---|---|---|---|
| `text` | string | **yes** | Cumulative transcript for the current utterance. Servers may emit repeatedly with growing text as decoding progresses. |
| `segment` | integer | no | Monotonically increases with each new utterance. If omitted, the plugin falls back to "text length shrank" as the utterance-boundary heuristic. |
| `is_endpoint` | boolean | no | `true` marks this as the **final** transcript for the current utterance; text will not change after this. The plugin emits the caption with `final=true` and advances to the next segment. If omitted, finalization is inferred from `segment` changes or text shrinkage (best-effort, may be delayed by one utterance). |
| `words` | array of `{word,start,end}` | no | Optional word-level alignment. `start`/`end` are seconds from the start of the current utterance (not the stream). Populated when the model supports it; typically on final results only for performance. |

### Finalization precedence (client-side)

When deciding whether a result is `final=true`, the plugin applies, in order:

1. **Explicit `is_endpoint: true`** — authoritative. Emitted as `final` immediately.
2. **Segment advanced** — if the new frame's `segment` differs from the previous one, the previous interim is promoted to `final` before processing the new frame.
3. **Text shrunk** — when no `segment` field is present, a shorter `text` than the previous frame is taken to mean a new utterance started; previous interim is promoted to `final`.

Servers should always emit `is_endpoint` to avoid the heuristic ambiguity.

### Termination

- Server sends text frame `"Done!"` to signal end of session; plugin responds with a close frame and stops the stream.
- Either side may send a WebSocket close (opcode `0x08`) at any time.
- Server should process any in-flight audio before closing.

---

## Minimal conforming server (Python)

```python
import asyncio, json, numpy as np, websockets

async def handler(ws):
    segment = 0
    async for msg in ws:
        if not isinstance(msg, bytes):
            continue
        samples = np.frombuffer(msg, dtype=np.float32)
        # ... run ASR on samples ...
        text = "hello world"  # placeholder
        await ws.send(json.dumps({
            "text": text,
            "segment": segment,
            "is_endpoint": False,
        }))
    await ws.send("Done!")

async def main():
    async with websockets.serve(handler, "localhost", 6006):
        await asyncio.Future()

asyncio.run(main())
```

See the sibling directories for full reference implementations:

| Sidecar | Model | Streaming | Word timestamps |
|---|---|---|---|
| `moonshine_stream_server/` | Moonshine (tiny/base) | pseudo (re-decode) | ✗ |
| `parakeet_stream_server/` | NVIDIA Parakeet-TDT | true (incremental encoder) | ✓ on finals |
| `whisper_stream_server/` | Whisper via MLX | pseudo (re-decode) | ✓ on finals |
| `voxtral_stream_server/` | Voxtral-Mini-4B | true (streaming output) | ✗ |
| `canary_stream_server/` | NVIDIA Canary | (blocked: upstream bug) | — |

---

## Plugin-side behavior reference

How the plugin translates protocol fields into its internal `CaptionResult`:

| Protocol field | `CaptionResult` field | Notes |
|---|---|---|
| `text` | `caption_text` | Trimmed of leading/trailing whitespace. |
| `is_endpoint` | `final` and `speech_final` | Both set to the same boolean. |
| `segment` | drives `index` increments | Each final increments `current_result_index`. |
| `words[i].word` | `words[i].word` | — |
| `words[i].start` | `words[i].start` | Seconds, from utterance start. |
| `words[i].end` | `words[i].end` | — |
| entire payload | `raw_message` | Preserved for debugging / custom downstream use. |

`stability` is set to `1.0` on finals, `0.5` on interims.

---

## Compatibility

- **sherpa-onnx-online-websocket-server** works out of the box (the protocol was derived from its wire format). Interim/final inference falls back to the segment/shrink heuristics since sherpa-onnx emits bare text.
- **sherpa-onnx-offline-websocket-server** is **not** compatible — it uses a length-prefixed binary protocol. Use the online variant.
- Any server matching this spec works; you can trivially wrap a new ASR model by emitting the JSON above in response to audio frames.
