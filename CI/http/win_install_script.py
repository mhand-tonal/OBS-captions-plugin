import argparse
import os
import re
import shutil
import sys
from pathlib import Path

sys.path.append(str(Path(__file__).parent.parent))

from sys import exit
from win_build_obs import check_call, CMAKE_VS_ARGS, DEFAULT_OBS_VERSION, OBS_VERSIONS, spa, setup_obs
from win_shared import package_zip


def main():
	parser = argparse.ArgumentParser(description="Build the Windows x64 plugin for a specific OBS release.")
	parser.add_argument("--obs-version", choices=OBS_VERSIONS, default=DEFAULT_OBS_VERSION)
	args = parser.parse_args()
	obs_version = args.obs_version
	print(f"Target: OBS {obs_version} (Windows x64)")

	root_dir = Path(os.getcwd())
	ci_root_dir = root_dir.joinpath("CI_build")
	build_deps_dir = ci_root_dir.joinpath("build_deps")
	ci_root_dir.mkdir(exist_ok = True)
	build_deps_dir.mkdir(exist_ok = True)
	print("root_dir:", repr(str(root_dir)))
	print("ci_root_dir:", repr(str(ci_root_dir)))
	print("build_deps_dir:", repr(str(build_deps_dir)))

	cmake_text = root_dir.parent.parent.joinpath("CMakeLists.txt").read_text()
	cmake_text = re.sub(r"\s+", "", cmake_text)
	version = re.search(r'set\(VERSION_STRING"(.*?)"\)', cmake_text).group(1)
	if not version:
		raise ValueError("no version found")
	print(f"VERSION_STRING: {version!r}")

	obs_studio = build_deps_dir.joinpath("obs-studio")
	CLEAN_OBS = os.environ.get("CLEAN_OBS")
	clean_afterwards = CLEAN_OBS in ("1", "true")
	print("CLEAN_OBS clean_afterwards", (CLEAN_OBS, clean_afterwards))
	# This build uses OBS's build-tree CMake exports, so keep it until the plugin is built.
	obs_studio_src, obs_deps_dir, build_installed_dir = setup_obs(obs_studio, clean_afterwards = False, obs_version = obs_version)

	check_call(["cmd", "/C", str(root_dir.joinpath("clone_plibsys.cmd"))], cwd = ci_root_dir)
	build_dir = ci_root_dir.joinpath("build", obs_version)
	installed_dir = ci_root_dir.joinpath("installed", obs_version)
	build_dir.mkdir(parents = True, exist_ok = True)
	obs_build_dir = obs_studio_src.joinpath("build")
	check_call([
		"cmake",
		*CMAKE_VS_ARGS,
		r"-DCMAKE_BUILD_TYPE=RelWithDebInfo",
		r"-DCMAKE_GENERATOR_PLATFORM=x64",
		"-DBUILD_SHARED_LIBS=ON",
		f"-DOBS_BUILD_DIR={str(obs_build_dir)}",
		f"-Dw32-pthreads_DIR={(obs_build_dir / 'deps' / 'w32-pthreads').as_posix()}",
		f"-DOBS_DEPS_DIR={str(obs_deps_dir)}",
		f"-DCMAKE_MODULE_PATH={(obs_studio_src / 'cmake' / 'finders').as_posix()}",
		f"-Dobs-frontend-api_DIR={(obs_build_dir / OBS_VERSIONS[obs_version]['frontend_api']).as_posix()}",
		f"-DCMAKE_INSTALL_PREFIX:PATH={str(installed_dir)}",
		str(root_dir.parent.parent),
	], cwd = build_dir)
	check_call(spa("cmake --build . --config RelWithDebInfo"), cwd = build_dir)
	check_call(spa("cmake --install . --config RelWithDebInfo"), cwd = build_dir)

	release = ci_root_dir.joinpath("release")
	package_zip(release, installed_dir, version, obs_version = obs_version)

	if clean_afterwards:
		shutil.rmtree(obs_build_dir)


if __name__ == '__main__':
	main()
