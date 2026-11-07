#!/usr/bin/env bash
# Cross-compiles the Windows build on macOS or Linux. The D3D12 shaders must be signed, and must come
# from the same DXC as the dxcompiler.dll the game ships (RT64 links them with shaders it compiles at
# run time), so with Docker they are compiled by that DXC release's Windows dxc.exe under Wine in a
# container (tools/windows/dxc_signed.sh). Without Docker (or with CONKER_UNSIGNED_SHADERS=1) this
# machine's DXC compiles them: the game builds but crashes on its first frame on Windows.
#
#   tools/windows/cross_build.sh [null|rt64] [x64|arm64]
#
# Default rt64 and x64; the build folder is build/windows-<renderer>, or build/windows-<renderer>-arm64.
#
# Needs: clang, clang-cl and lld (macOS: brew install llvm lld xwin), and the MSVC CRT and Windows SDK
# in XWIN_DIR (default build/xwin). The first run downloads them with xwin only when
# XWIN_ACCEPT_LICENSE=1 is set, which accepts Microsoft's license for them. Run tools/apply_patches.sh
# and tools/regenerate.sh first, as for any build.
set -euo pipefail

here="$(cd "$(dirname "$0")/../.." && pwd)"
renderer="${1:-rt64}"
arch="${2:-x64}"
case "$arch" in
    x64) xwin_arch=x86_64; build="$here/build/windows-$renderer" ;;
    arm64) xwin_arch=aarch64; build="$here/build/windows-$renderer-arm64" ;;
    *) echo "unknown arch $arch: x64 or arm64" >&2; exit 1 ;;
esac
export XWIN_ARCH="$xwin_arch"
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

if [ ! -d "$XWIN_DIR/crt/lib/$xwin_arch" ]; then
    if [ "${XWIN_ACCEPT_LICENSE:-0}" != 1 ]; then
        echo "error: no $xwin_arch MSVC CRT and Windows SDK in $XWIN_DIR. Set XWIN_ACCEPT_LICENSE=1 to download them" \
            "with xwin, which accepts Microsoft's license for them." >&2
        exit 1
    fi
    xwin --accept-license --cache-dir "$here/build/xwin-cache" --arch "$xwin_arch" splat --output "$XWIN_DIR"
fi
# On a case-sensitive disk, the SDK headers the source names in another case (Shlobj.h for ShlObj.h).
python3 "$here/tools/windows/case_links.py" "$XWIN_DIR" "$here/src" "$here/lib"

# RT64's file_to_c runs during the build, so it is built for this machine as well.
export HOST_TOOLS_DIR="$here/build/windows-host-tools"
mkdir -p "$HOST_TOOLS_DIR"
if [ ! -x "$HOST_TOOLS_DIR/file_to_c" ]; then
    c++ -std=c++17 -O2 -o "$HOST_TOOLS_DIR/file_to_c" "$here/lib/rt64/src/tools/file_to_c/file_to_c.cpp"
fi

"$here/tools/build_mods.sh"

# Shaders from the Windows DXC under Wine in Docker (tools/windows/dxc_signed.sh), when Docker is there.
signing=()
if [ "${CONKER_UNSIGNED_SHADERS:-0}" != 1 ] && command -v docker > /dev/null && docker info > /dev/null 2>&1; then
    docker build -q --platform linux/amd64 -t conker-dxc-wine -f "$here/tools/windows/dxc-wine.Dockerfile" "$here/tools/windows" > /dev/null
    signing=(-DCONKER_SIGNED_SHADERS=ON)
    echo "==> shaders compiled by Windows DXC under Wine in Docker"
else
    signing=(-DCONKER_SIGNED_SHADERS=OFF)
    echo "warning: no Docker: the shaders come from this machine's DXC, which Windows will not run (the game crashes on its first frame)" >&2
fi

# The compiler checks build in Release: xwin brings no debug CRT (msvcrtd.lib) for CMake's default Debug.
cmake -S "$here" -B "$build" -G Ninja -Wno-dev \
    -DCMAKE_TOOLCHAIN_FILE="$here/tools/windows/clang-cl-xwin.cmake" \
    -DCMAKE_CROSSCOMPILING_EMULATOR="$here/tools/windows/host_tool.sh" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCONKER_RENDERER="$renderer" \
    -DCMAKE_TRY_COMPILE_CONFIGURATION=Release "${signing[@]}"
cmake --build "$build"
echo "built $build/ConkersBadFurDayRecompiled.exe"
