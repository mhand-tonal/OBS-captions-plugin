#!/bin/bash

function build_obs() {
  set -e

  echo "setting up src and building OBS in $(pwd)/obs-studio"
  mkdir -p obs-studio && cd obs-studio/

  BUILD_OBS__UNPACKED_DEPS_DIR="$(pwd)/unpacked_deps"
  BUILD_OBS__SRC_DIR="$(pwd)/src"
  BUILD_OBS__BUILD_DIR="$(pwd)/src/build_macos"
  BUILD_OBS__INSTALLED_DIR="$BUILD_OBS__BUILD_DIR"
  echo "BUILD_OBS__SRC_DIR: $BUILD_OBS__SRC_DIR"
  echo "BUILD_OBS__UNPACKED_DEPS_DIR: $BUILD_OBS__UNPACKED_DEPS_DIR"
  echo "BUILD_OBS__BUILD_DIR: $BUILD_OBS__BUILD_DIR"
  echo "BUILD_OBS__INSTALLED_DIR: $BUILD_OBS__INSTALLED_DIR"
  test -n OSX_ARCHITECTURES

  if [ -e "src/done" ]; then
    echo "obs build done,skipping"
    return
  fi

  if [ ! -e "src" ]; then
    echo getting src
    git clone  https://github.com/obsproject/obs-studio.git src
    cd src
    git checkout 32.1.1
    git submodule update --init --recursive
    cd ..
  fi

  if [ ! -e deps.tar.xz ]; then
    curl -L -o deps.tar.xz https://github.com/obsproject/obs-deps/releases/download/2025-08-23/macos-deps-2025-08-23-universal.tar.xz
  fi

  if [ ! -e deps.qt.tar.xz ]; then
    curl -L -o deps.qt.tar.xz https://github.com/obsproject/obs-deps/releases/download/2025-08-23/macos-deps-qt6-2025-08-23-universal.tar.xz
  fi

  if [ ! -d unpacked_deps ]; then
    mkdir unpacked_deps
    tar -k -xvf deps.tar.xz -C unpacked_deps
    tar -k -xvf deps.qt.tar.xz -C unpacked_deps
  fi

  # CMake 4.x doesn't auto-detect Swift via the Xcode generator.
  # OBS's libobs-metal is pure Swift — tell cmake about it.
  sed -i '' '1a\
enable_language(Swift)
' src/libobs-metal/CMakeLists.txt

  echo building OBS && pwd
  mkdir -p build_installed
  cd src

  $CMAKE --preset macos \
    -DCMAKE_OSX_ARCHITECTURES="$OSX_ARCHITECTURES" \
    -DENABLE_BROWSER=OFF \
    -DENABLE_PLUGINS=OFF \
    -DENABLE_UI=OFF \
    -DENABLE_SCRIPTING=OFF

  $CMAKE --build build_macos --config Release -t obs-frontend-api
  $CMAKE --install build_macos --config Release --component obs_libraries

  cd ../
  du -chd1 && pwd
  touch "done"
}

function build_obs_cleanup() {
  #  if [ "$CLEAN_OBS" = "1" ] || [ "$CLEAN_OBS" = "true" ]; then
  #    if [[ -n "$BUILD_OBS__BUILD_DIR" && -d "$BUILD_OBS__BUILD_DIR" ]]; then
  #      echo "cleaning up OBS BUILD dir: $BUILD_OBS__BUILD_DIR"
  #      rm -rf "$BUILD_OBS__BUILD_DIR" || true
  #    else
  #      echo "OBS BUILD dir folder not found: $BUILD_OBS__BUILD_DIR"
  #    fi
  #  else
  #    echo "not cleaning OBS build, CLEAN_OBS: $CLEAN_OBS"
  #  fi
  :
}
