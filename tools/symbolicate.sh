#!/usr/bin/env bash
# Map a ConkersBadFurDayRecompiled crash log back to generated C and N64 addresses (macOS, needs atos).
#   tools/symbolicate.sh build/host/ConkersBadFurDayRecompiled build/run.log
set -euo pipefail
bin="$1"; log="$2"
slide=$(grep -oE 'image slide 0x[0-9a-f]+' "$log" | head -1 | awk '{print $3}')
[ -n "$slide" ] || { echo "no slide in log"; exit 1; }
grep -E '^[0-9]+ +ConkersBadFurDayRecompiled +0x[0-9a-f]+ ' "$log" | head -8 | while read -r _ _ addr rest; do
    loc=$(atos -o "$bin" -s "$slide" "$addr" 2>/dev/null | head -1)
    echo "== $addr $loc"
    if [[ "$loc" =~ \((funcs_[0-9]+\.c):([0-9]+)\) ]]; then
        file="${BASH_REMATCH[1]}"; line="${BASH_REMATCH[2]}"
        if [ "$line" != "0" ]; then
            sed -n "$((line-4)),$((line+1))p" "$(dirname "$0")/../build/us/RecompiledFuncs/$file" | sed 's/^/    /'
        fi
    fi
done
