"""Check DLL imports against an official OBS Windows runtime, without starting OBS."""

import argparse
import ctypes
import os
from pathlib import Path
import tempfile
import zipfile

from win_build_obs import OBS_VERSIONS
from win_shared import download


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--obs-version", required=True, choices=OBS_VERSIONS)
    parser.add_argument("--plugin", required=True, type=Path)
    args = parser.parse_args()
    plugin = args.plugin.resolve(strict=True)

    # OBS 30 predates the architecture suffix in Windows release filenames.
    platform = "Windows" if args.obs_version == "30.2.3" else "Windows-x64"
    archive_name = f"OBS-Studio-{args.obs_version}-{platform}.zip"
    runtime = Path(tempfile.mkdtemp(prefix=f"obs-runtime-{args.obs_version}-"))
    archive = runtime / archive_name
    download(
        f"https://github.com/obsproject/obs-studio/releases/download/{args.obs_version}/{archive_name}",
        archive,
    )
    with zipfile.ZipFile(archive) as package:
        for member in package.infolist():
            if member.filename.startswith("bin/64bit/"):
                package.extract(member, runtime)

    # Only the shipped runtime supplies OBS/Qt/curl DLLs. The build dependency
    # directories must not mask imports that are missing on users' computers.
    with os.add_dll_directory(str(runtime / "bin/64bit")):
        host = ctypes.CDLL(str(runtime / "bin/64bit/obs.dll"))
        module = ctypes.CDLL(str(plugin))
        host.obs_get_version.restype = ctypes.c_uint32
        module.obs_module_ver.restype = ctypes.c_uint32
        host_version = host.obs_get_version()
        module_version = module.obs_module_ver()
        if module_version != host_version:
            raise RuntimeError(
                f"Plugin API version {module_version:#x} differs from OBS {host_version:#x}"
            )
    print(f"PASS: plugin DLL loads with the official OBS {args.obs_version} Windows x64 runtime")
    # DLLs remain loaded until process exit; the CI runner cleans up the temp directory.


if __name__ == "__main__":
    main()
