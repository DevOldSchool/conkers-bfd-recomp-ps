#!/usr/bin/env bash
# Builds the mods the game carries built in (CMakeLists.txt, CONKER_EMBEDDED_MODS): each mods/<name>
# folder with a Makefile, into mods/<name>/build/<mod_filename>.nrm. The packaging scripts run it before
# configuring, so every release has them; they stay off until the player turns them on (debug menu > MODS).
#
#   tools/build_mods.sh
#
# Needs clang with the MIPS target and ld.lld (Homebrew: brew install llvm lld); RecompModTool is built
# here when missing (docs/modding.md).
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
tool="$here/build/N64Recomp/RecompModTool"
if [ ! -x "$tool" ]; then
    echo "==> building RecompModTool"
    cmake -S "$here/lib/N64Recomp" -B "$here/build/N64Recomp" -G Ninja > /dev/null
    cmake --build "$here/build/N64Recomp" --target RecompModTool > /dev/null
fi

for makefile in "$here"/mods/*/Makefile; do
    dir="$(dirname "$makefile")"
    echo "==> mod $(basename "$dir")"
    make -s -C "$dir" REPO="$here"
done
