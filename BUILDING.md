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

The scripts clone and build OBS into a local `CI_build/` directory, then build
and package the plugin. Windows defaults to **OBS 32.1.1** and also supports
**OBS 30.2.3**, with matching dependencies for each version. The OBS build is
cached in `CI_build/`, so only the first run for each version is slow.

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

Requires Visual Studio 2022 with the Desktop development with C++ workload,
CMake, Python 3, Git, curl and 7-Zip on `PATH`. NSIS (`makensis`) is optional
and adds a setup EXE alongside the ZIP.

```bat
cd CI\http
python win_install_script.py --obs-version 30.2.3
python win_install_script.py --obs-version 32.1.1
```

Omitting `--obs-version` selects `32.1.1`. Both builds use the same plugin source.

| OBS target | Windows x64 dependency and Qt bundles |
| --- | --- |
| 30.2.3 | `2024-05-08` (Qt 6.6.3) |
| 32.1.1 | `2025-08-23` |

These pins match OBS's [30.2.3 build specification](https://github.com/obsproject/obs-studio/blob/30.2.3/buildspec.json)
and [32.1.1 CMake presets](https://github.com/obsproject/obs-studio/blob/32.1.1/CMakePresets.json).
OBS dependencies and plugin build/install directories are separated by OBS
version, so switching targets does not reuse the other version's libraries.

Packages are written to `CI/http/CI_build/release/`:

```text
Closed_Captions_Plugin__v<version>_Windows_OBS-30.2.3.zip
Closed_Captions_Plugin__v<version>_Windows_OBS-32.1.1.zip
```

When NSIS is available, the corresponding installers end in `_Setup.exe`.
GitHub Actions builds both Windows variants and uploads them as
`Windows-Plugin-OBS-30.2.3` and `Windows-Plugin-OBS-32.1.1` artifacts. Download
the artifact matching the OBS version shown under **Help > About**, then use
the enclosed installer or follow `INSTALL.txt` in the plugin ZIP. Install
only one variant: both provide the same `obs_google_caption_plugin.dll`.

Before uploading, CI checks that each DLL loads against the matching official
OBS Windows release and reports the same OBS API version. This detects missing
DLLs or imported symbols; captions and UI still need testing inside OBS.

## Notes

* `CLEAN_OBS=1` removes the OBS build directory after packaging; the next run
  rebuilds it. Delete `CI/<flavor>/CI_build/` to start fully fresh.
* The plugin links system `libcurl` for the Deepgram and Local WebSocket
  backends — no extra dependencies are required for those.
