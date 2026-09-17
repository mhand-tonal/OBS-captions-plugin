# Selected upstream stability fixes

Adapted from [ratwithacompiler/OBS-captions-plugin](https://github.com/ratwithacompiler/OBS-captions-plugin)
through `109e028521`. This is a selective backport; the fork's providers, caption
modes, settings UI, and OBS-version build matrix remain in use.

| Area | Upstream commits |
| --- | --- |
| Scene collection cleanup, removed sources, capture state synchronization | `9e9a7c1713`, `c7a3dfd695` |
| Output shutdown and writer failure state | `dfaab194d9`, `80bb7402d5` |
| Drain queued transcripts and retain the newest file caption on shutdown | `dbb4d18323` |
| Format file captions before inserting the current result into history | `c82d4251a5` |
| Native caption routing, delay handling, duplicate tracking, UTF-8 byte limit | `e1b76f63a7`, `d1ae8fee7d`, `1bdd416dae` |
| Windows template instantiations and includes | `ebe9998f90` |
| Audio conversion initialization, capture connection errors, null results | `73d921dcd2`, `f32d481ddf`, `def2e201df` |
| Callback replacement under one lock, thread creation errors | `afb18fa461`, `cbdf203` |
| Thread-safe transcript filename timestamps | `6ed9b8e1d6` |

The shared logging sink comes from upstream's logging cleanup through `109e028521`.
The capture status signal uses a queued Qt connection (also present in upstream's
later audio changes), so stopping capture cannot destroy a session inside its own
callback. Stream/writer stop flags are atomic; the stream changes also cover this
fork's Deepgram and local WebSocket providers.

Additional Deepgram diagnostics report the loaded curl/TLS versions, connection,
first audio write, session audio totals, nonempty caption result count, and server
close code/reason in the OBS log. Received frames are handled before a subsequent
socket EOF can discard them, including frames received with the handshake.

For Windows troubleshooting, reproduce the issue, stop captioning, then open
**Help → Log Files → View Current Log** and search for `[captions_plugin]` and
`Deepgram`. Logs are also under `%APPDATA%\obs-studio\logs`. Audio totals count bytes
successfully written by the client; they do not prove server transcription.

Regression test instructions are in [tests/README.md](tests/README.md).
