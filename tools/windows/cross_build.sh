#!/usr/bin/env bash
# Cross-compiles the Windows (x64) build on macOS or Linux, to check that it still builds. It is not
# a release build: the shaders come from the host's DXC, which cannot sign DXIL.
#
#   tools/windows/cross_build.sh [null|rt64]   (default rt64; build folder build/windows-<renderer>)
#
# Needs: clang, clang-cl and lld (macOS: brew install llvm lld xwin), and the MSVC CRT and Windows SDK
# in XWIN_DIR (default build/xwin). The first run downloads them with xwin only when
# XWIN_ACCEPT_LICENSE=1 is set, which accepts Microsoft's license for them. Run tools/apply_patches.sh
# and tools/regenerate.sh first, as for any build.
set -euo pipefail

here="$(cd "$(dirname "$0")/../.." && pwd)"
renderer="${1:-rt64}"
build="$here/build/windows-$renderer"
export XWIN_DIR="${XWIN_DIR:-$here/build/xwin}"

if [ -z "${LLVM_DIR:-}" ]; then
    if command -v brew > /dev/null && [ -x "$(brew --prefix llvm)/bin/clang-cl" ]; then
        LLVM_DIR="$(brew --prefix llvm)/bin"
    else
        LLVM_DIR="$(dirname "$(command -v clang-cl)")"
    fi
fi
if [ ! -x "$LLVM_DIR/lld-link" ] && command -v brew > /dev/null && [ -x "$(brew --prefix lld)/bin/lld-link" ]; then
    PATH="$(brew --prefix lld)/bin:$PATH"  # Homebrew keeps lld apart from llvm
    export PATH
fi
export LLVM_DIR

if [ ! -d "$XWIN_DIR/crt" ]; then
    if [ "${XWIN_ACCEPT_LICENSE:-0}" != 1 ]; then
        echo "error: no MSVC CRT and Windows SDK in $XWIN_DIR. Set XWIN_ACCEPT_LICENSE=1 to download them" \
            "with xwin, which accepts Microsoft's license for them." >&2
        exit 1
    fi
    xwin --accept-license --cache-dir "$here/build/xwin-cache" --arch x86_64 splat --output "$XWIN_DIR"
fi

# RT64's file_to_c runs during the build, so it is built for this machine as well.
export HOST_TOOLS_DIR="$here/build/windows-host-tools"
mkdir -p "$HOST_TOOLS_DIR"
if [ ! -x "$HOST_TOOLS_DIR/file_to_c" ]; then
    c++ -std=c++17 -O2 -o "$HOST_TOOLS_DIR/file_to_c" "$here/lib/rt64/src/tools/file_to_c/file_to_c.cpp"
fi

# The compiler checks build in Release: xwin brings no debug CRT (msvcrtd.lib) for CMake's default Debug.
cmake -S "$here" -B "$build" -G Ninja -Wno-dev \
    -DCMAKE_TOOLCHAIN_FILE="$here/tools/windows/clang-cl-xwin.cmake" \
    -DCMAKE_CROSSCOMPILING_EMULATOR="$here/tools/windows/host_tool.sh" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCONKER_RENDERER="$renderer" \
    -DCMAKE_TRY_COMPILE_CONFIGURATION=Release
cmake --build "$build"
echo "built $build/ConkersBadFurDayRecompiled.exe"
