#!/usr/bin/env bash
# DXC for the Windows cross build (RT64_CROSS_DXC, CMakeLists.txt): dxc_signed.sh <dxc.exe> <DXC arguments>.
#
# RT64 links shader libraries compiled at build time with entry points it compiles at run time, and
# DXC links only pieces from the very same DXC version; D3D12 also refuses unsigned shaders. So DXIL is
# compiled by <dxc.exe>, the Windows DXC of the release whose dxcompiler.dll and dxil.dll the game
# ships, under Wine in an x86-64 Linux container (tools/windows/dxc-wine.Dockerfile; emulated on Apple
# silicon), with the repository at the same path. SPIR-V (-spirv) needs neither, and is compiled by
# RT64's DXC for this machine. Paths reach Wine as Z:\ paths, since dxc.exe reads /x as an option.
set -euo pipefail
here="$(cd "$(dirname "$0")/../.." && pwd)"
exe="$1"
shift
dxc="$here/lib/rt64/src/contrib/dxc"

for arg in "$@"; do
    if [ "$arg" = "-spirv" ]; then
        arch="$(uname -m | sed 's/aarch64/arm64/; s/x86_64/x64/')"
        if [ "$(uname -s)" = Darwin ]; then
            DYLD_LIBRARY_PATH="$dxc/lib/$arch" exec "$dxc/bin/$arch/dxc-macos" "$@"
        fi
        LD_LIBRARY_PATH="$dxc/lib/$arch" exec "$dxc/bin/$arch/dxc-linux" "$@"
    fi
done

args=()
for arg in "$@"; do
    # An absolute path has a second slash; options such as /Fo do not. Include paths come joined to -I.
    if [[ "$arg" == /*/* ]]; then
        args+=("Z:${arg//\//\\}")
    elif [[ "$arg" == -I/* ]]; then
        path="${arg#-I}"
        args+=("-IZ:${path//\//\\}")
    else
        args+=("$arg")
    fi
done
exec docker run --rm --platform linux/amd64 -v "$here:$here" -w "$PWD" conker-dxc-wine \
    wine64 "$exe" "${args[@]}" 2> >(grep -v '^rosetta error: invalid gdt selector' >&2)
