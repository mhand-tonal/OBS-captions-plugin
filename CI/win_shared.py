import os
import shutil
import sys
import subprocess

from pathlib import Path


def spa(line: str):
	args = line.split(" ")
	return [i for i in args if i]


def download(url: str, target_path: Path):
	check_call([*spa("curl -L -f --retry 10 -o"), str(target_path), url])


def unzip(zipfile: Path, target_path: Path):
	check_call(["7z", "x", "-y", str(zipfile), f"-o{str(target_path)}"])


def check_call(args: list, cwd = None, shell = False):
	cwd = str(cwd) if cwd else None
	print(f"calling {args !r} in cwd: {cwd !r}, shell={shell}")
	sys.stdout.flush()
	return subprocess.check_call(args, cwd = cwd, shell = shell)


def eprint(*args, **kwargs):
	print(*args, **kwargs, file = sys.stderr)


def package_zip(release: Path, installed_dir: Path, version: str):
	release.mkdir(parents = True, exist_ok = True)

	pkg_dir = release.joinpath(f"Closed_Captions_Plugin__v{version}_Windows")
	obs_plugin_64bit = pkg_dir.joinpath("obs-plugins", "64bit")
	obs_plugin_64bit.mkdir(parents = True, exist_ok = True)

	plugin_dll = installed_dir.joinpath(r"lib\obs_google_caption_plugin.dll")
	print(f"copying  {plugin_dll!r} -> {obs_plugin_64bit!r}")
	shutil.copy(plugin_dll, obs_plugin_64bit)

	readme = pkg_dir.joinpath("INSTALL.txt")
	readme.write_text(
		"Cloud Closed Captions Plugin for OBS Studio\n"
		"=============================================\n\n"
		"To install:\n"
		"  1. Copy the obs-plugins folder into your OBS Studio installation directory.\n"
		"     Typically: C:\\Program Files\\obs-studio\\\n\n"
		"  2. Restart OBS Studio.\n\n"
		"  3. Go to Tools > Cloud Closed Captions to configure.\n"
	)

	check_call(["7z", "a", "-r", release.joinpath(f"Closed_Captions_Plugin__v{version}_Windows.zip"), pkg_dir])

	# Build NSIS installer
	nsi_script = Path(__file__).parent.joinpath("installer.nsi")
	if nsi_script.exists():
		installer_exe = release.joinpath(f"Closed_Captions_Plugin__v{version}_Windows_Setup.exe")
		try:
			check_call([
				"makensis",
				f"/DVERSION={version}",
				f"/DPLUGIN_DLL={str(plugin_dll)}",
				f"/DOUTFILE={str(installer_exe)}",
				str(nsi_script),
			])
			print(f"installer built: {installer_exe}")
		except Exception as e:
			print(f"WARNING: NSIS installer build failed (makensis may not be installed): {e}")
			print("ZIP package was still created successfully.")


