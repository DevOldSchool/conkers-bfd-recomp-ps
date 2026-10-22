#!/usr/bin/env python3
"""Build an RT64 texture pack template for the game's font glyphs, and check their hashes.

The game makes its Super-xBR pack itself from the ROM at start
(src/enhancements/sharp_font/sharp_font.cpp; CONKER_DUMP_SHARP_FONT writes it out). This writes
the glyphs enlarged with each pixel repeated, as a starting point for new art, and checks the
glyph hashes against a texture dump.

The font is 95 grayscale RLE records at ROM 0x40F10..0x42450. At boot func_15015920 walks them and
func_15015A38 decodes each into its own I8 buffer: a zero row above and below the glyph, a zero
column at its left, rows padded to a multiple of 8 bytes and odd rows word-swapped for TMEM. A
glyph is drawn by loading its buffer to TMEM 0 as it is and sampling row_stride x (height + 1)
texels (the zero row below is not sampled), so each glyph is one texture whose RT64 hash (version
5, TMEMHasher::hash) is XXH3-64 of those bytes followed by the draw's width, height, TLUT, line, siz
and fmt. This script decodes the glyphs from the ROM and computes the hashes
itself, so the pack covers every glyph, including ones a run never showed:

    tools/font_pack.py --out conker-recomp-data/texture_packs/font

Each replacement is the glyph --scale times larger, white with its intensity as alpha as RT64
decodes I8 (R = G = B = A), with each pixel repeated. RT64 loads PNG replacements
without mipmaps, so the default of 8 suits pictures drawn at about 5x to 10x.
font/glyphs.json in the pack lists each glyph's hash and metrics, so new art can be drawn for the
same glyphs.

--dump <dir> checks the computed hashes against a run's CONKER_DUMP_TEXTURES directory: every
dumped texture that holds a glyph must have the hash computed for that glyph.
"""

import argparse
import json
import os
import pathlib
import struct
import sys
import zlib


FONT_START = 0x40F10
FONT_END = 0x42450

MASK64 = (1 << 64) - 1
PRIME64_1 = 0x9E3779B185EBCA87
PRIME_MX1 = 0x165667919E3779F9
XXH3_SECRET = bytes.fromhex(
    "b8fe6c3923a44bbe7c01812cf721ad1cded46de9839097db7240a4a4b7b3671f"
    "cb79e64eccc0e578825ad07dccff7221b8084674f743248ee03590e6813a264c"
    "3c2852bb91c300cb88d0658b1b532ea371644897a20df94e3819ef46a9deacd8"
    "a8fa763fe39c343ff9dcbbc7c70b4f1d8a51e04bcdb45931c89f7ec9d9787364"
    "eac5ac8334d3ebc3c581a0fffa1363eb170ddd51b7f0da49d316552629d4689e"
    "2b16be587d47a1fc8ff8b8d17ad031ce45cb3a8f95160428afd7fbcabb4b407e")


def xxh3_64(data):
    """XXH3-64 with seed 0 for inputs of 17 to 240 bytes (every glyph hashes 44 to 220)."""
    def read64(buffer, offset):
        return struct.unpack_from("<Q", buffer, offset)[0]

    def mix16(offset, secret_offset):
        product = ((read64(data, offset) ^ read64(XXH3_SECRET, secret_offset)) *
                   (read64(data, offset + 8) ^ read64(XXH3_SECRET, secret_offset + 8)))
        return (product & MASK64) ^ (product >> 64)

    def avalanche(h):
        h ^= h >> 37
        h = (h * PRIME_MX1) & MASK64
        return h ^ (h >> 32)

    length = len(data)
    if not 16 < length <= 240:
        raise ValueError(f"xxh3_64 here handles 17 to 240 bytes, not {length}")
    acc = (length * PRIME64_1) & MASK64
    if length <= 128:
        if length > 32:
            if length > 64:
                if length > 96:
                    acc += mix16(48, 96) + mix16(length - 64, 112)
                acc += mix16(32, 64) + mix16(length - 48, 80)
            acc += mix16(16, 32) + mix16(length - 32, 48)
        acc += mix16(0, 0) + mix16(length - 16, 16)
        return avalanche(acc & MASK64)
    for i in range(8):
        acc += mix16(16 * i, 16 * i)
    acc_end = mix16(length - 16, 136 - 17)
    acc = avalanche(acc & MASK64)
    for i in range(8, length // 16):
        acc_end += mix16(16 * i, 16 * (i - 8) + 3)
    return avalanche((acc + acc_end) & MASK64)


def read_png(path):
    """Reads an 8-bit PNG and returns rows of RGBA tuples."""
    data = pathlib.Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos = 8
    idat = b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    if depth != 8 or interlace != 0:
        raise ValueError(f"{path}: only 8-bit, non-interlaced PNGs are read")
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[color]
    raw = zlib.decompress(idat)
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    pos = 0
    for _ in range(height):
        kind = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + up) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                predictor = left if pa <= pb and pa <= pc else (up if pb <= pc else corner)
                line[i] = (line[i] + predictor) & 0xFF
        previous = line
        expand = {1: lambda p: (p[0], p[0], p[0], 255), 2: lambda p: (p[0], p[0], p[0], p[1]),
                  3: lambda p: (p[0], p[1], p[2], 255), 4: tuple}[channels]
        rows.append([expand(line[x * channels:(x + 1) * channels]) for x in range(width)])
    return rows


def write_png(path, rows):
    """Writes rows of RGBA tuples as an 8-bit RGBA PNG."""
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", len(rows[0]), len(rows), 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    pathlib.Path(path).write_bytes(png)


def decode_glyphs(rom):
    """Decodes every font record as func_15015A38 does. Each glyph's image is the part a draw
    samples, row_stride x (height + 1) intensities without the word swap, and its tmem is the same
    texels as they are in its buffer and TMEM."""
    glyphs = []
    cursor = FONT_START
    while cursor + 0xF < FONT_END:
        record = rom[cursor:]
        width = record[0] + 1
        height = record[1] + 1
        size = struct.unpack(">I", record[4:8])[0]
        row_stride = (width + 7) & ~7
        image = [[0] * row_stride for _ in range(height + 1)]
        offset = 8
        for y in range(1, height + 1):
            x = 1
            while x < width:
                packed = record[offset]
                offset += 1
                run = (packed & 0xF) + 1
                for i in range(run):
                    if x + i < row_stride:
                        image[y][x + i] = packed & 0xF0
                x += run
        tmem = bytearray()
        for y, row in enumerate(image):
            swap = 4 if y & 1 else 0
            tmem += bytes(row[x ^ swap] for x in range(row_stride))
        # Width, height, TLUT (none), line in 64-bit words, siz (8-bit) and fmt (I), as RT64 adds them.
        tmem += struct.pack("<HHIHBB", row_stride, height + 1, 0, row_stride // 8, 1, 4)
        glyphs.append({
            "index": len(glyphs),
            "width": width,
            "height": height,
            "x_offset": record[2],
            "y_offset": record[3],
            "image": image,
            "hash": f"{xxh3_64(bytes(tmem)):016x}",
        })
        cursor += size
    return glyphs


def check_dump(glyphs, dump):
    """Checks that every texture in a dump whose texels are a glyph's has that glyph's computed hash."""
    by_texels = {}
    for glyph in glyphs:
        by_texels.setdefault(json.dumps(glyph["image"]), []).append(glyph)
    seen = set()
    failures = 0
    for tile_path in sorted(dump.glob("*.tile.json")):
        tile = json.loads(tile_path.read_text())
        if (tile["tile"]["fmt"], tile["tile"]["siz"]) != (4, 1) or tile["width"] > 16 or tile["height"] > 16:
            continue
        rows = read_png(tile_path.with_name(tile_path.name.replace(".tile.json", ".png")))
        if any(px[0] != px[3] for row in rows for px in row):
            continue
        candidates = by_texels.get(json.dumps([[px[3] for px in row] for row in rows]), [])
        if not candidates:
            continue
        hash_text = tile_path.name.split(".")[0]
        glyph = next((g for g in candidates if g["hash"] == hash_text), None)
        if glyph is None:
            print(f"{hash_text}: glyph {candidates[0]['index']} was hashed as {candidates[0]['hash']}")
            failures += 1
        else:
            seen.add(glyph["index"])
    print(f"dump: {len(seen)} glyphs match their computed hashes, {failures} do not")
    return failures == 0


def main():
    here = pathlib.Path(__file__).resolve().parent.parent
    decomp = pathlib.Path(os.environ.get("CONKER_DECOMP", here.parent / "conkers-bfd-decomp"))
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--rom", default=os.environ.get("CONKER_ROM", decomp / "roms" / "baserom.us.z64"),
                        type=pathlib.Path, help="US ROM (.z64); defaults as in tools/regenerate.sh")
    parser.add_argument("--out", type=pathlib.Path, help="pack directory to write")
    parser.add_argument("--scale", default=8, type=int, help="replacement size multiple")
    parser.add_argument("--dump", type=pathlib.Path, help="CONKER_DUMP_TEXTURES directory to check against")
    args = parser.parse_args()

    glyphs = decode_glyphs(args.rom.read_bytes())
    print(f"{len(glyphs)} glyphs decoded from the ROM")
    if args.dump is not None and not check_dump(glyphs, args.dump):
        return 1
    if args.out is None:
        return 0

    glyph_dir = args.out / "font"
    glyph_dir.mkdir(parents=True, exist_ok=True)
    textures = []
    record = []
    for glyph in glyphs:
        name = f"glyph_{glyph['index']:02d}"
        rows = [[(v, v, v, v) for v in row for _ in range(args.scale)]
                for row in glyph["image"] for _ in range(args.scale)]
        write_png(glyph_dir / f"{name}.png", rows)
        textures.append({"path": f"font/{name}.png", "hashes": {"rt64": glyph["hash"], "rice": ""},
                         "operation": "preload", "shift": "half"})
        record.append({key: glyph[key] for key in ("index", "hash", "width", "height", "x_offset", "y_offset")})
    database = {
        "configuration": {"configurationVersion": 3, "autoPath": "rt64", "defaultOperation": "stream",
                          "defaultShift": "half", "hashVersion": 5},
        "textures": textures,
        "operationFilters": [],
        "shiftFilters": [],
        "extraFiles": [],
    }
    (args.out / "rt64.json").write_text(json.dumps(database, indent=4) + "\n")
    (glyph_dir / "glyphs.json").write_text(json.dumps(record, indent=4) + "\n")
    print(f"{len(textures)} glyph replacements ({args.scale}x) written to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
