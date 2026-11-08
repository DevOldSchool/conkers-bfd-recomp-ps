#!/usr/bin/env bash
# Package a built ConkersBadFurDayRecompiled.exe as dist/Conkers-Bad-Fur-Day-Recompiled-<version>-windows-<x64|arm64>.zip,
# to run on another Windows PC with no checkout, and its debug information as ...-pdb.zip.
#
#   tools/windows/package.sh [build dir]    (default build/rt64 on Windows, else build/windows-rt64)
#
# The zip is named for the CPU the game was built for: windows-x64 or windows-arm64.
#
# Runs in Git Bash on Windows or on macOS or Linux. The zip holds the game, the DLLs the build puts
# next to it (SDL2 and RT64's shader compiler), a README and licenses/ (tools/package_notices.py).
# No ROM is packaged. Attach tools/source_archive.sh's tarball to the release beside it.
# A build cross-compiled by tools/windows/cross_build.sh packages too; it is only one to hand out when its
# shaders were signed, which needs Docker (docs/building-windows.md).
set -euo pipefail

here="$(cd "$(dirname "$0")/../.." && pwd)"
if [ -n "${1:-}" ]; then
    build="$1"
elif [ -f "$here/build/rt64/ConkersBadFurDayRecompiled.exe" ]; then
    build="$here/build/rt64"
else
    build="$here/build/windows-rt64"
fi
exe="$build/ConkersBadFurDayRecompiled.exe"
# The built-in mods (tools/build_mods.sh, CONKER_EMBEDDED_MODS): a release always carries them.
if ! grep -q '^CONKER_EMBEDDED_MOD_IDS:INTERNAL=.*conker_speedrun' "$build/CMakeCache.txt" 2>/dev/null; then
    echo "$build has no built-in speedrun mod: run tools/build_mods.sh, then build again" >&2
    exit 1
fi
if [ ! -f "$exe" ]; then
    echo "no $exe: build the RT64 renderer for Windows first" >&2
    exit 1
fi
dlls=(SDL2.dll dxcompiler.dll dxil.dll)
for dll in "${dlls[@]}"; do
    if [ ! -f "$build/$dll" ]; then
        echo "no $build/$dll: the build copies it next to the game (CMakeLists.txt)" >&2
        exit 1
    fi
done
# The C++ runtime is linked in (CMAKE_MSVC_RUNTIME_LIBRARY), so no PC needs the Visual C++
# Redistributable: refuse a build whose files still ask for its DLLs.
objdump="$(command -v llvm-objdump || true)"
[ -n "$objdump" ] || { [ -x "$(brew --prefix llvm 2>/dev/null)/bin/llvm-objdump" ] && objdump="$(brew --prefix llvm)/bin/llvm-objdump"; }
if [ -n "$objdump" ]; then
    for f in "$exe" "${dlls[@]/#/$build/}"; do
        if "$objdump" -p "$f" | grep -qiE "DLL Name: (vcruntime|msvcp)"; then
            echo "$f needs the Visual C++ Redistributable: rebuild with the static runtime (CMakeLists.txt)" >&2
            exit 1
        fi
    done
else
    echo "warning: no llvm-objdump, so the build is not checked for the Visual C++ runtime" >&2
fi
if ! command -v zip > /dev/null; then
    echo "needs zip" >&2
    exit 1
fi

version="$("$here/tools/version.sh")"
# The PE header's machine field: 0x8664 x64, 0xAA64 arm64.
pe=$((0x$(od -A n -t x4 -j 60 -N 4 "$exe" | tr -d ' ')))
machine="$(od -A n -t x2 -j $((pe + 4)) -N 2 "$exe" | tr -d ' ')"
case "$machine" in
    8664) arch=x64 ;;
    aa64) arch=arm64 ;;
    *) echo "$exe is for machine 0x$machine, neither x64 nor arm64" >&2; exit 1 ;;
esac
name="Conkers-Bad-Fur-Day-Recompiled-$version-windows-$arch"
dist="$here/dist"
stage="$dist/$name"
echo "==> packaging $exe as dist/$name.zip"
rm -rf "$stage" "$stage.zip" "$stage-pdb.zip"
mkdir -p "$stage"
cp "$exe" "$stage/"
for dll in "${dlls[@]}"; do
    cp "$build/$dll" "$stage/"
done
python3 "$here/tools/package_notices.py" "$build" "$stage/licenses" windows
# Adding it to Steam as a non-Steam game, with its artwork (tools/steam).
cp "$here/tools/steam/add-to-steam.ps1" "$here/tools/steam/Add to Steam.bat" "$stage/"
cp -R "$here/assets/steam" "$stage/steam"
# Windows tools open text files most reliably with CRLF line endings.
cat > "$stage/README.txt" <<'EOF'
Conker's Bad Fur Day Recompiled for Windows (@ARCH@)

Needs Windows 10 version 1803 or later, or Windows 11. Nothing else needs installing.

The game is not signed, so Windows blocks a downloaded copy: before extracting the zip,
right-click it, choose Properties and tick Unblock.

Open ConkersBadFurDayRecompiled.exe. No ROM is included: the first time, it asks for your own
US ROM (.z64) and keeps a copy, so it only asks once. Settings, saves, the ROM copy and
last-run.log are kept in %LOCALAPPDATA%\ConkersBadFurDayRecompiled\conker-recomp-data.

To play it through Steam, quit Steam and double-click "Add to Steam.bat": the next time Steam
starts the game is in its library, with its artwork. Run it again after moving this folder.

If the game crashes or will not start, open it again: it says so and shows the log to attach
to a bug report (crash-reports\crash-<date>_<time>.log, and the .dmp file of the same name).

The licenses are in licenses\: the game as a whole is passed on under the GPL-3.0 (COPYING.txt),
and licenses\SOURCE.txt says where its source code is.
EOF
if [ "$arch" = arm64 ]; then
    sed -i.bak -e 's/@ARCH@/ARM64, for PCs with an ARM processor such as Snapdragon/' "$stage/README.txt"
else
    sed -i.bak -e 's/@ARCH@/x64, for Intel and AMD PCs; use the windows-arm64 zip on an ARM PC/' "$stage/README.txt"
fi
rm -f "$stage/README.txt.bak"
for f in "$stage/README.txt" "$stage/licenses/"*.txt "$stage/add-to-steam.ps1" "$stage/Add to Steam.bat"; do
    sed -i.bak $'s/\r*$/\r/' "$f" && rm -f "$f.bak"
done

(cd "$dist" && zip -qr "$name.zip" "$name")
if [ -f "$build/ConkersBadFurDayRecompiled.pdb" ]; then
    (cd "$build" && zip -q "$stage-pdb.zip" ConkersBadFurDayRecompiled.pdb)
    echo "==> dist/$name-pdb.zip ($(du -h "$stage-pdb.zip" | cut -f1))"
fi
rm -rf "$stage"
echo "==> dist/$name.zip ($(du -h "$stage.zip" | cut -f1))"
