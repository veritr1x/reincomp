#!/usr/bin/env bash
# Run from an activated Emscripten SDK, with host cmake and ninja on PATH.
set -euo pipefail
if [[ $# != 3 ]]; then
  echo "usage: build-tools.sh LLVM_WASM_BUILD PINNED_SOURCE_CHECKOUTS WORK" >&2
  exit 2
fi
llvm=$(realpath "$1")
sources=$(realpath "$2")
mkdir -p "$3"
work=$(realpath "$3")
here=$(cd "$(dirname "$0")" && pwd)
jobs=${JOBS:-4}
deps="$work/deps"
python3 "$here/prepare_lifter.py" "$sources/remill" "$work"
for pair in gflags:v2.2.2 glog:v0.6.0; do
  name=${pair%:*}
  rev=${pair#*:}
  src="$work/$name-$rev"
  if [[ ! -d "$src" ]]; then
    temp=$(mktemp -d "$work/$name.XXXXXX")
    git -c "safe.directory=$sources/$name" -C "$sources/$name" archive "$rev" | tar -xf - -C "$temp"
    mv "$temp" "$src"
  fi
  options=()
  if [[ $name == glog ]]; then
    if ! grep -q GLOG_OS_EMSCRIPTEN "$src/src/glog/platform.h"; then
      patch -d "$src" -p1 --batch < "$here/glog-emscripten.patch"
    fi
    # glog 0.6 probes pthread_rwlock with Threads::Threads even in its
    # explicitly single-threaded configuration.
    options+=(-DWITH_THREADS=OFF -DHAVE_RWLOCK=OFF -DWITH_UNWIND=OFF -DWITH_GTEST=OFF -DWITH_GFLAGS=ON
      -DWITH_SYMBOLIZE=OFF -DHAVE_SYSCALL_H=OFF -DHAVE_SYS_SYSCALL_H=OFF
      -DHAVE__UNWIND_BACKTRACE=OFF -DHAVE_EXECINFO_H=OFF)
  else
    options+=(-DGFLAGS_BUILD_gflags_LIB=OFF -DGFLAGS_BUILD_gflags_nothreads_LIB=ON)
  fi
  emcmake cmake -S "$src" -B "$work/build-$name" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
    -DCMAKE_PREFIX_PATH="$deps" -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
    -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON -DREGISTER_INSTALL_PREFIX=OFF "${options[@]}"
  cmake --build "$work/build-$name" --parallel "$jobs" --target install
done
emcmake cmake -S "$here" -B "$work/tools" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_PREFIX_PATH="$deps" -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
  -DLLVM_DIR="$llvm/lib/cmake/llvm" -DGENERATED="$work/generated"
cmake --build "$work/tools" --parallel "$jobs"
