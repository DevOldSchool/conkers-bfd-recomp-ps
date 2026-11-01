#!/usr/bin/env bash
# Makes assets/app-icon.ico, the Windows icon (assets/app-icon.rc puts it in the executable), from
# assets/app-icon.png. Run it again after changing the PNG, and commit the .ico: Windows builds have
# no sips to make it themselves.
#
#   tools/windows/make_icon.sh   (macOS: needs sips and python3)
#
# The PNG is 1024 x 1024 with the tile in the middle 824 (the margin macOS icons keep); Windows icons
# fill their square, so the tile alone is used. Each size is stored as a PNG, which Windows reads
# since Vista.
set -euo pipefail

here="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

sips --cropToHeightWidth 824 824 "$here/assets/app-icon.png" --out "$work/tile.png" > /dev/null
sizes=(16 24 32 48 64 128 256)
for size in "${sizes[@]}"; do
    sips -z "$size" "$size" "$work/tile.png" --out "$work/$size.png" > /dev/null
done

python3 -I - "$here/assets/app-icon.ico" "${sizes[@]/#/$work/}" <<'EOF'
import struct
import sys

out, sizes = sys.argv[1], sys.argv[2:]
images = []
for prefix in sizes:
    with open(prefix + ".png", "rb") as f:
        images.append((int(prefix.rsplit("/", 1)[1]), f.read()))

# ICONDIR, then one ICONDIRENTRY per image (a size of 256 is written as 0), then the images.
header = struct.pack("<HHH", 0, 1, len(images))
offset = len(header) + 16 * len(images)
entries, data = b"", b""
for size, png in images:
    side = 0 if size == 256 else size
    entries += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(png), offset)
    offset += len(png)
    data += png
with open(out, "wb") as f:
    f.write(header + entries + data)
EOF
echo "made assets/app-icon.ico"
