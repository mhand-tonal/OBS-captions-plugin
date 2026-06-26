# Building from source

This fork builds three speech-recognition backends into a single plugin and
selects between them at runtime (see the *Speech Recognition Providers* section
of the [README](./README.md)). The build compiles the plugin against a matching
OBS build and its dependencies.

The build scripts live under `CI/` and come in two flavors:

* **`CI/http/`** — builds the Google **HTTP** backend (via plibsys) plus the
  Deepgram and Local WebSocket backends (which only need system `libcurl`). This
  is the recommended path for this fork — it does **not** require vcpkg/gRPC.
* **`CI/grpc/`** — builds the older Google **gRPC** backend instead. Pulls in
  vcpkg + gRPC and takes much longer. Not needed for Deepgram or Local WebSocket.

Each script clones and builds OBS itself (currently **OBS 32.1.1** with the
`2025-08-23` obs-deps) into a local `CI_build/` directory, then builds and
packages the plugin. The OBS build is cached in `CI_build/`, so only the first
run is slow.

## macOS

Verified path. Requires Xcode command-line tools, CMake, and Ninja
(`brew install cmake ninja`).

```bash
cd CI/http
bash osx_install_script.sh
```

This produces a universal (`x86_64;arm64`) plugin bundle and a release zip:

```
CI/http/CI_build/release/Closed_Captions_Plugin__v<version>_MacOS.zip
CI/http/CI_build/release/Closed_Captions_Plugin__v<version>_MacOS/cloud-closed-captions.plugin
```

To build for a single architecture (faster), set `TARGET_ARCH`:

```bash
TARGET_ARCH=arm64 bash osx_install_script.sh
```

### Installing the built plugin

Copy the bundle into your OBS plugins folder and restart OBS:

```bash
cp -R CI/http/CI_build/release/Closed_Captions_Plugin__v*_MacOS/cloud-closed-captions.plugin \
  "$HOME/Library/Application Support/obs-studio/plugins/"
```

The bundle is ad-hoc signed (not notarized). If Gatekeeper blocks it on first
load, clear the quarantine attribute:

```bash
xattr -dr com.apple.quarantine \
  "$HOME/Library/Application Support/obs-studio/plugins/cloud-closed-captions.plugin"
```

## Linux

```bash
cd CI/http
bash linux_install_script.sh
```

## Windows

```bat
cd CI\http
python win_install_script.py
```

## Notes

* To force a clean OBS rebuild, set `CLEAN_OBS=1` (and `CLEAN_VCPKG=1` for the
  gRPC path). Otherwise delete `CI/<flavor>/CI_build/` to start fully fresh.
* The plugin links system `libcurl` for the Deepgram and Local WebSocket
  backends — no extra dependencies are required for those.
</content>
</invoke>
