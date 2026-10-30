#!/usr/bin/env bash
# Regenerate the N64Recomp inputs and outputs for the US ROM.
#
# Needs: the decomp checkout (default: ../conkers-bfd-decomp) with your US ROM recorded as
# roms/baserom.us.z64, python3, and a built N64Recomp (cmake -S lib/N64Recomp -B build/N64Recomp).
#
# config/us.syms.toml is committed and kept as it is: symbols generated from another decomp
# revision can name functions differently and break hooks. CONKER_UPDATE_SYMS=1 rewrites it
# from the decomp checkout; otherwise the generated copy goes to build/us/us.syms.generated.toml.
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

if [ "${CONKER_UPDATE_SYMS:-0}" = 1 ]; then
    syms="$here/config/us.syms.toml"
else
    syms="$here/build/us/us.syms.generated.toml"
fi
echo "==> generating the virtual ROM (symbols: ${syms#$here/})"
python3 "$here/tools/gen_syms.py" --decomp "$decomp" --rom "$rom" \
    --game-code "$extract/game/code.bin" --game-data "$extract/game/data.bin" \
    --out-syms "$syms" --out-rom "$here/build/us/conker.us.recomp.bin" \
    --report "$here/build/us/syms-report.json"
if [ "$syms" != "$here/config/us.syms.toml" ] && ! cmp -s "$syms" "$here/config/us.syms.toml"; then
    echo "note: the decomp checkout gives different symbols than config/us.syms.toml, which is used;" \
        "CONKER_UPDATE_SYMS=1 takes the decomp's (see build/us/us.syms.generated.toml)"
fi

if [ ! -x "$here/build/N64Recomp/N64Recomp" ] || [ ! -x "$here/build/N64Recomp/RSPRecomp" ]; then
    echo "==> building N64Recomp and RSPRecomp"
    cmake -S "$here/lib/N64Recomp" -B "$here/build/N64Recomp" -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build "$here/build/N64Recomp" --target N64Recomp RSPRecomp
fi

echo "==> recompiling"
rm -rf "$here/build/us/RecompiledFuncs"
mkdir -p "$here/build/us/RecompiledFuncs"
if ! (cd "$here" && build/N64Recomp/N64Recomp config/us.toml 2> build/us/recomp-run.log); then
    echo "N64Recomp failed; the end of build/us/recomp-run.log:" >&2
    tail -n 20 "$here/build/us/recomp-run.log" >&2
    exit 1
fi

echo "==> recompiling the audio microcode (RSPRecomp)"
mkdir -p "$here/build/us/rsp"
(cd "$here" && build/N64Recomp/RSPRecomp config/aspMain.toml)

echo "==> declaring runtime-provided functions"
python3 "$here/tools/gen_extras_decl.py" "$here/build/us/RecompiledFuncs"
echo "==> recording the config the code was recompiled from"
# The build refuses recompiled code from another config (tools/check_recompiled.cmake): a hook
# added to config/us.toml does nothing until this script runs again.
python3 - "$here" <<'EOF'
import hashlib, pathlib, sys
here = pathlib.Path(sys.argv[1])
lines = [f"{hashlib.sha256((here / name).read_bytes()).hexdigest()}  {name}\n"
         for name in ("config/us.toml", "config/us.syms.toml")]
(here / "build/us/RecompiledFuncs/config.sha256").write_text("".join(lines))
EOF
echo "done: $(ls "$here/build/us/RecompiledFuncs" | wc -l | tr -d ' ') files in build/us/RecompiledFuncs (log: build/us/recomp-run.log)"
