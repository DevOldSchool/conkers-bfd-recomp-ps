#!/usr/bin/env bash
# Regenerate the N64Recomp inputs and outputs for the US ROM.
#
# Needs: the decomp checkout (default: ../conkers-bfd-decomp) with your US ROM recorded as
# roms/baserom.us.z64, python3, and a built N64Recomp (cmake -S lib/N64Recomp -B build/N64Recomp).
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
decomp="${CONKER_DECOMP:-$here/../conkers-bfd-decomp}"
rom="${CONKER_ROM:-$decomp/roms/baserom.us.z64}"
extract="$here/build/us/extract"

mkdir -p "$here/build/us"

if [ ! -f "$extract/game/code.bin" ]; then
    echo "==> extracting game code/data with the decomp's RZIP tool"
    (cd "$decomp" && python3 scripts/rzip_extract.py --profile us --rom "$rom" --output "$extract" --force)
fi

echo "==> generating config/us.syms.toml and the virtual ROM"
python3 "$here/tools/gen_syms.py" --decomp "$decomp" --rom "$rom" \
    --game-code "$extract/game/code.bin" --game-data "$extract/game/data.bin" \
    --out-syms "$here/config/us.syms.toml" --out-rom "$here/build/us/conker.us.recomp.bin" \
    --report "$here/build/us/syms-report.json"

if [ ! -x "$here/build/N64Recomp/N64Recomp" ] || [ ! -x "$here/build/N64Recomp/RSPRecomp" ]; then
    echo "==> building N64Recomp and RSPRecomp"
    cmake -S "$here/lib/N64Recomp" -B "$here/build/N64Recomp" -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build "$here/build/N64Recomp" --target N64Recomp RSPRecomp
fi

echo "==> recompiling"
rm -rf "$here/build/us/RecompiledFuncs"
mkdir -p "$here/build/us/RecompiledFuncs"
(cd "$here" && build/N64Recomp/N64Recomp config/us.toml 2> build/us/recomp-run.log)

echo "==> recompiling the audio microcode (RSPRecomp)"
mkdir -p "$here/build/us/rsp"
(cd "$here" && build/N64Recomp/RSPRecomp config/aspMain.toml)

echo "==> declaring runtime-provided functions"
python3 "$here/tools/gen_extras_decl.py" "$here/build/us/RecompiledFuncs"
echo "done: $(ls "$here/build/us/RecompiledFuncs" | wc -l | tr -d ' ') files in build/us/RecompiledFuncs (log: build/us/recomp-run.log)"
