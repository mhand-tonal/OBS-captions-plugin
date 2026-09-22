# Windows troubleshooting

How to collect the information needed to diagnose a problem with the Cloud
Closed Captions plugin on Windows, and how to read it yourself.

## 1. Reproduce, then collect

Do this in order — the collector reads the log OBS just wrote, so reproducing
first matters.

1. **Close OBS completely.** Check the system tray; OBS may still be running
   there.
2. **Start OBS and reproduce the problem.** If the problem involves captions
   specifically: enable captioning, speak for about 30 seconds, then stop
   captioning.
3. **Turn captioning off.** Session totals (bytes of audio sent, number of
   caption results) are written when the caption stream shuts down, which
   happens as soon as captioning stops — you do not need to quit OBS.
4. **Run the collector**, in PowerShell:

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\windows_diagnostics.ps1
   ```

   If you do not have the repository checked out, download just
   [`tools/windows_diagnostics.ps1`](tools/windows_diagnostics.ps1), then right-click
   the file → **Properties** → **Unblock** before running it.

5. **Send the zip** it prints at the end (it lands next to a folder on your
   Desktop).

The script needs no administrator rights and changes nothing. It collects:

| Section | Why it matters |
| --- | --- |
| OBS installs, `obs64.exe` version | The plugin is built per OBS version and OBS refuses a mismatch |
| Every copy of `obs_google_caption_plugin.dll`, with SHA-256 | Two copies means OBS may be loading a stale build |
| DLL load test against your OBS runtime | Reproduces OBS's own load failure outside OBS, with the Win32 error |
| Last 3 OBS logs and any crash logs | The actual failure |
| Plugin lines extracted from the newest log | The short version |
| Plugin settings from each scene collection | Provider, model, output modes |
| Audio devices | Capture problems |

**The Deepgram API key is replaced with `<redacted, N characters>`.** The OBS
logs still contain machine names, device names, and file paths — open
`report.txt` and skim it before sharing.

## 1b. Without the script: the OBS UI alone

The OBS log already contains most of what the collector gathers — the OBS
version, whether the module loaded, the provider in use, the audio source, and
the Deepgram session totals. Same steps 1–3 above, then:

**Help → Log Files →**

| Menu item | Use |
| --- | --- |
| **View Current Log** | Read it now, in OBS |
| **Show Log Files** | Opens `%APPDATA%\obs-studio\logs` — attach the newest `.txt` |
| **Upload Current Log File** | Copies a shareable URL — note this publishes the log publicly |

Attaching the file is the safer of the two ways to share it.

Send with it:

* OBS version from **Help → About**.
* Which plugin package you installed (`...OBS-30.2.3`, `...OBS-32.1.1` or `...OBS-32.2.2`).
* Whether `C:\Program Files\obs-studio\obs-plugins\64bit\obs_google_caption_plugin.dll`
  exists, plus its size and date from Explorer.

To trim the log to the relevant lines without running a script, one command:

```bat
findstr /C:"captions_plugin" /C:"Deepgram" /C:"Loading module" "%APPDATA%\obs-studio\logs\<logfile>.txt"
```

What the UI route cannot tell you, and the collector can: whether a second,
stale copy of the DLL exists elsewhere, and the exact Win32 error when the DLL
fails to load. Reach for the script only if the log shows the module failing to
load and the reason is not obvious.

### Verbose logging

Most useful plugin lines are info-level and appear in the normal log. A few
(WebSocket message types, event-loop entry, queue overflow) are debug-level and
OBS drops those unless it is started with `--verbose`. Only needed if the normal
log comes up empty:

```bat
cd /d "C:\Program Files\obs-studio\bin\64bit"
obs64.exe --verbose --unfiltered_log
```

## 2. What to tell me

Paste this filled in — it decides which half of the log I read:

```
OBS version         :
Plugin package      :  (OBS-30.2.3 / OBS-32.1.1 / OBS-32.2.2 / built from source)
Provider            :  (Google / Deepgram / Local WebSocket)
What I did          :
What I expected     :
What happened       :
Worked before?      :  (never worked / broke after <change>)
Captions in the dock:  (yes / no)  <- Tools > Cloud Closed Captions
```

"Captions in the dock" splits the problem in half: text in the dock but not on
the stream is an output-routing problem, and no text in the dock is a capture or
speech-API problem.

## 3. Reading the log yourself

Every plugin line is prefixed `[captions_plugin]`. Search the log for that, and
for `Deepgram`.

### Did OBS load the module?

| Line | Meaning |
| --- | --- |
| `Loading module: obs_google_caption_plugin.dll` | OBS found the DLL |
| `LoadLibrary failed for ...: ... code 126` | A DLL the plugin imports is missing |
| `Module '...' not loaded` | Load failed; the reason is on the line above |
| `[captions_plugin] ... google_s2t_caption_plugin 0.33 obs_module_load` | Plugin initialized — this is the one that matters |

No `Loading module` line at all means the DLL is not in
`<OBS install>\obs-plugins\64bit\`. A `Loading module` line with no
`obs_module_load` line means the DLL was found but rejected — almost always the
wrong OBS version. The collector's load test reports the exact API versions.

### Did captioning start?

| Line | Meaning |
| --- | --- |
| `OBS_FRONTEND_EVENT_FINISHED_LOADING, plugin_manager loaded: 1` | UI wired up |
| `enabled: 1, is_streaming ...` | Why the plugin decided to caption (or not) |
| `captioning start ok` / `captioning start failed` | Session start result |
| `provider=1 model='nova-3' keywords=''` | Provider actually in use (0 Google, 1 Deepgram, 2 Local WebSocket) |
| `source <name> active: 1` | The audio source it is capturing from |
| `no caption source with name: '...'` | The configured source is gone — reselect it in settings |
| `couldn't create AudioCaptureSession` | Audio capture failed to start |

### Deepgram session

| Line | Meaning |
| --- | --- |
| `Deepgram transport: libcurl=... TLS=...` | Which curl/TLS the OBS runtime supplies |
| `Deepgram request: model=... path=...` | The request about to be made |
| `Deepgram authentication failed — check your API key` | Bad or expired key |
| `WebSocket upgrade failed: <status>` | Rejected before the stream opened |
| `Deepgram closed the connection during the handshake` | Server hung up before answering — network, proxy, or TLS interception |
| `handshake recv error: ... (curl code N)` | The OBS libcurl refused the read; `socket=closed` means the connection was already gone |
| `Deepgram WebSocket connected, language=...` | Stream is open |
| `Deepgram first audio sent: N bytes` | Audio is actually flowing |
| `Deepgram close: code=1011 reason=...` | The server closed it, with its reason |
| `Deepgram session ended: audio_bytes_sent=... (N seconds), caption_results=N` | The summary line — read this first |

The session-ended line localizes almost every caption problem:

| audio_bytes_sent | caption_results | Where the problem is |
| --- | --- | --- |
| 0 | 0 | Audio capture. Wrong source selected, source muted, or the source produces no audio. Check the `source <name> active:` line. |
| > 0 | 0 | Reached Deepgram, got nothing back. Wrong language setting, silence/noise, or a model your key can't use. Check for a `Deepgram close:` line. |
| > 0 | > 0 | Recognition worked. The problem is downstream: output mode, text source, or the encoder. |

Audio totals count bytes the client wrote successfully. They prove the plugin
sent audio, not that the server transcribed it.

### The handshake fails on every attempt

A log that repeats `Deepgram transport` → `Deepgram request` → a handshake error
→ `audio_bytes_sent=0` every ten seconds never reached Deepgram at all. Work
through it in this order:

1. **Test the key and the network outside OBS.** This separates an account or
   proxy problem from a plugin problem in one command:

   ```bat
   curl -v --http1.1 -H "Authorization: Token YOUR_KEY" https://api.deepgram.com/v1/projects
   ```

   Fails → the key, the network, or TLS interception on the machine. Succeeds →
   continue.

2. **Match the plugin package to your OBS version.** The plugin uses the
   `libcurl` that ships with OBS, so a package built for one OBS release talks to
   a different libcurl on another. OBS may load the plugin anyway; the mismatch
   only shows up as curl errors at connection time.

3. If both check out, send the log — the error line carries the curl code and
   whether the socket was still open, which distinguishes a dropped connection
   from curl rejecting the call.

### Captions recognized but not visible

* Check which outputs are enabled in the caption settings (streaming, recording,
  virtual camera, text source, file output) — each is separate.
* Native stream captions do not work with the **AMD hardware encoder** on
  Windows. NVENC, QuickSync, and x264 are fine. See the README.
* For a text source output, confirm the target text source still exists and has
  the same name.

## 4. Crashes

Crash logs are in `%APPDATA%\obs-studio\crashes`; the collector picks up the two
newest. Send the crash log plus the plugin DLL's SHA-256 from `report.txt` — the
stack trace shows module offsets, and the hash identifies which build to resolve
them against.

## 5. Known-good checks

* The DLL loads against the official OBS runtime and reports a matching API
  version — CI verifies this for both Windows packages on every build
  (`CI/check_windows_load.py`).
* Install **one** package only. Both variants install the same
  `obs_google_caption_plugin.dll` filename, so installing both leaves whichever
  was copied last.
* Build and packaging details are in [BUILDING.md](./BUILDING.md); the
  diagnostics added to the Deepgram backend are listed in
  [UPSTREAM_FIXES.md](./UPSTREAM_FIXES.md).
