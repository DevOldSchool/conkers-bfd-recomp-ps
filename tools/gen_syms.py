#!/usr/bin/env python3
"""Generate the N64Recomp symbol file and virtual ROM for Conker's Bad Fur Day (US).

Inputs come from the sibling decomp checkout (https://github.com/DevOldSchool/conkers-bfd-decomp):

* ``progress/functions.json``  - every reviewed function boundary (main, game, debugger)
* ``config/overlays.json``     - ROM code ranges and overlay base addresses
* ``config/game/us-sdk.ld``    - SDK symbols that live in the game overlay tail
* ``build/us/original-assets/conker.us.elf`` (optional) - main-executable symbols,
  including SDK functions that functions.json does not track
* the US ROM plus the decompressed game code/data images produced by
  ``scripts/rzip_extract.py``

Conker runs its main executable through a TLB alias at 0x10000000 and its game
overlay at 0x15000000 (demand-paged from RZIP chunks) and the debugger overlay at
0x16000000. The binaries were linked for those addresses, so this file describes
the sections at their *runtime* addresses even though the decomp names main
functions ``func_8000xxxx``.

N64Recomp reads function words from a ROM image at ``rom`` offsets. The game
overlay is compressed in the real ROM, so we build a "virtual ROM": the real ROM
with the decompressed game code and data images appended after the 64 MiB mark.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

MAIN_VRAM = 0x10000000  # TLB alias base: vram = rom offset + MAIN_VRAM
DECOMP_MAIN_VRAM = 0x80000000  # the decomp's naming/link base for main
ROM_SIZE = 0x4000000
GAME_CODE_VROM = ROM_SIZE  # virtual ROM offset of the decompressed game code image


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) & ~(boundary - 1)


# --------------------------------------------------------------------------------------
# ELF reading (tiny big-endian ELF32 symbol reader so we need no third-party package)
# --------------------------------------------------------------------------------------

def read_elf_functions(path: Path) -> list[tuple[int, int, str]]:
    data = path.read_bytes()
    if data[:4] != b"\x7fELF":
        raise SystemExit(f"{path} is not an ELF file")
    endian = ">" if data[5] == 2 else "<"
    shoff = struct.unpack_from(endian + "I", data, 0x20)[0]
    shentsize, shnum, _shstrndx = struct.unpack_from(endian + "HHH", data, 0x2E)
    sections = []
    for i in range(shnum):
        sections.append(struct.unpack_from(endian + "IIIIIIIIII", data, shoff + i * shentsize))

    def string_at(strtab, offset):
        start = strtab[4] + offset
        end = data.index(b"\0", start)
        return data[start:end].decode()

    functions = []
    for sec in sections:
        if sec[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sections[sec[6]]
        for j in range(sec[5] // 16):
            name, value, size, info, _other, _shndx = struct.unpack_from(endian + "IIIBBH", data, sec[4] + j * 16)
            if (info & 0xF) == 2:  # STT_FUNC
                functions.append((value, size, string_at(strtab, name)))
    return functions


# --------------------------------------------------------------------------------------
# Symbol collection
# --------------------------------------------------------------------------------------

# Rare's own copies of libultra routines whose semantics match the originals. Naming them with
# the libultra name makes N64Recomp redirect them to the runtime's native implementation.
# Keyed by runtime vram. Evidence: the decomp's reviewed C for each unit.
# Handwritten assembly regions whose routines branch into shared tails. N64Recomp (patched)
# recompiles every function inside with the whole region as its body. 0x150A3A70:0x150AE35C is
# Rare's software geometry/visibility code (decomp: game_D0F20.c and neighbours).
# 0x15018500:0x150186D0 is func_15018500, a handwritten prefix-code tree builder (game_45880.c) that
# keeps its return address in $s4 (`or $s4, $ra, $zero` ... `jr $s4`). Outside a region `jal` does
# not write $ra, so `jr $s4` became a lookup of a stale value; the region wrapper seeds the
# sentinel that turns it into a return.
GAME_REGIONS = [(0x15018500, 0x150186D0), (0x150A3A70, 0x150AE35C)]

CONKER_RENAMES = {
    0x100037F0: "osCreateThread",  # src/done/main/init_37F0.c: identical field setup, sp - 16
}

GENERIC_NAME = re.compile(r"^(func|D)_[0-9A-Fa-f]{8}$")


def name_quality(name: str) -> int:
    if GENERIC_NAME.match(name):
        return 0
    return 1


class Overlay:
    def __init__(self, name: str, rom: int, vram: int, size: int, code_end: int):
        self.name = name
        self.rom = rom  # (virtual) ROM offset of the section start
        self.vram = vram  # runtime vram of the section start
        self.size = size  # size of the whole section (code + trailing rodata/data)
        self.code_end = code_end  # vram where executable code stops (functions must end here)
        self.functions: dict[int, dict] = {}  # vram -> {name, size, source}

    def contains_code(self, vram: int) -> bool:
        return self.vram <= vram < self.code_end

    def add(self, vram: int, size: int | None, name: str, source: str) -> None:
        if not self.contains_code(vram):
            return
        cur = self.functions.get(vram)
        if cur is None:
            self.functions[vram] = {"name": name, "size": size, "source": source}
            return
        if name_quality(name) > name_quality(cur["name"]):
            cur["name"] = name
            cur["source"] = source
        if cur["size"] in (None, 0) and size:
            cur["size"] = size

    def finalize(self, words_lookup) -> list[dict]:
        starts = sorted(self.functions)
        out = []
        for i, vram in enumerate(starts):
            entry = self.functions[vram]
            next_start = starts[i + 1] if i + 1 < len(starts) else self.code_end
            gap = next_start - vram
            size = entry["size"] or 0
            if size == 0 or size > gap:
                size = gap
                entry["size_from_gap"] = True
            # Trim trailing nop padding so that a gap-derived size does not swallow
            # alignment padding between functions (N64Recomp handles it, but keep tidy).
            while size > 4 and words_lookup(vram + size - 4) == 0 and entry.get("size_from_gap"):
                size -= 4
            size = max(size, 4)
            out.append({"name": entry["name"], "vram": vram, "size": size})
        return out



SDK_DOC_GLOBS = ("docs/evidence/libraries/game_sdk_*.md", "docs/evidence/libraries/game_libultrare_eeprom_variant_boundaries.md")
RANGE_RE = re.compile(r"`0x([0-9A-Fa-f]+):0x([0-9A-Fa-f]+)`")
MEMBER_RE = re.compile(r"`(func_15[0-9A-Fa-f]{6}|0x[0-9A-Fa-f]+|0)`")
C_TYPE_TOKENS = {"uint", "int", "void", "u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64", "f32", "f64", "char", "long", "short", "unsigned", "signed", "float", "double", "if", "while", "for", "switch", "else", "do", "sizeof"}
FUNC_DEF_RE = re.compile(r"^[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\([^;{]*\)?\s*\{?\s*$")


def source_function_order(decomp: Path, hint: str) -> list[str]:
    """Function definitions, in order, of the libultra/libultrare source that `hint` names."""
    base = Path(hint).name
    stem = base[:-2] if base.endswith((".o", ".c")) else base
    candidates = list(decomp.glob(f"lib/ultralib/src/*/{stem}.c")) + list(decomp.glob(f"lib/libultrare/src/**/{stem}.c"))
    if "libultrare" in hint:
        candidates = list(decomp.glob(f"lib/libultrare/src/**/{stem}.c")) or candidates
    if not candidates:
        return []
    names: list[str] = []
    text = candidates[0].read_text(errors="replace")
    # Strip comments so prototypes inside them do not count.
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    depth = 0
    for line in text.splitlines():
        if depth == 0:
            m = FUNC_DEF_RE.match(line)
            if m and not line.lstrip().startswith(("#", "typedef", "return", "extern", "static inline")) and ";" not in line:
                name = m.group(1)
                if "(" in line and not line.strip().endswith(";") and name not in C_TYPE_TOKENS:
                    names.append(name)
        depth += line.count("{") - line.count("}")
    return names


def harvest_game_sdk_members(decomp: Path, game_vram: int) -> list[tuple[int, str | None]]:
    """(vram, name-or-None) for SDK functions documented in the game overlay tail."""
    out: list[tuple[int, str | None]] = []
    for pattern in SDK_DOC_GLOBS:
        for doc in decomp.glob(pattern):
            for line in doc.read_text().splitlines():
                if not line.startswith("| `"):
                    continue
                rng = RANGE_RE.search(line)
                cells = [c.strip().strip("`") for c in line.strip("|").split("|")]
                if rng:
                    start = int(rng.group(1), 16)
                    if start >= 0x1000000:  # already a vram, not a game offset
                        continue
                    hint = cells[0] if cells[0].endswith(".c") or "/" in cells[0] else next((c for c in cells if c.endswith(".o")), cells[0])
                    members_cell = line[rng.end():]
                    members = []
                    for m in MEMBER_RE.finditer(members_cell):
                        tok = m.group(1)
                        if tok.startswith("func_"):
                            members.append(int(tok[5:], 16))
                        else:
                            members.append(game_vram + start + int(tok, 16))
                    if not members:
                        members = [game_vram + start]
                    members = sorted(set(members))
                    names = source_function_order(decomp, hint)
                    if len(names) == len(members):
                        out.extend(zip(members, names))
                    else:
                        out.extend((v, None) for v in members)
                else:
                    # mtxutil-style rows: | name | offset | 0x15xxxxxx | bytes |
                    m = re.match(r"^\| `([A-Za-z_]\w*)` \| `(?:0x[0-9A-Fa-f]+|0)` \| `(0x15[0-9A-Fa-f]{6})` \|", line)
                    if m:
                        out.append((int(m.group(2), 16), m.group(1)))
    return out


def n64recomp_special_names(symbol_lists: Path) -> set[str]:
    """Names N64Recomp renames to <name>_recomp (reimplemented, ignored or renamed lists)."""
    if not symbol_lists.exists():
        return set()
    return set(re.findall(r'"([A-Za-z_]\w*)"', symbol_lists.read_text()))

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--decomp", type=Path, default=Path(__file__).resolve().parents[2] / "conkers-bfd-decomp")
    parser.add_argument("--rom", type=Path, help="US ROM (default: <decomp>/roms/baserom.us.z64)")
    parser.add_argument("--game-code", type=Path, help="decompressed game code image (game/code.bin)")
    parser.add_argument("--game-data", type=Path, help="decompressed game data image (game/data.bin)")
    parser.add_argument("--elf", type=Path, help="decomp main ELF (default: <decomp>/build/us/original-assets/conker.us.elf)")
    parser.add_argument("--out-syms", type=Path, required=True)
    parser.add_argument("--out-rom", type=Path, required=True)
    parser.add_argument("--report", type=Path, help="write a JSON report about coverage and discovered functions")
    args = parser.parse_args()

    decomp = args.decomp
    rom_path = args.rom or decomp / "roms" / "baserom.us.z64"
    elf_path = args.elf or decomp / "build" / "us" / "original-assets" / "conker.us.elf"

    rom = bytearray(rom_path.read_bytes())
    if len(rom) != ROM_SIZE:
        raise SystemExit(f"unexpected ROM size {len(rom):#x}")
    game_code = args.game_code.read_bytes()
    game_data = args.game_data.read_bytes()

    overlays_cfg = json.loads((decomp / "config" / "overlays.json").read_text())["overlays"]
    main_code = overlays_cfg["main"]["code_ranges"]["us"]
    game_code_rng = overlays_cfg["game"]["code_ranges"]["us"]
    dbg_code = overlays_cfg["debugger"]["code_ranges"]["us"]
    game_vram = int(overlays_cfg["game"]["vram"], 16)
    dbg_vram = int(overlays_cfg["debugger"]["vram"], 16)

    main_code_start = int(main_code["start"], 16)
    main_code_end = int(main_code["end"], 16)
    game_code_end_off = int(game_code_rng["end"], 16)
    if game_code_end_off != len(game_code):
        print(f"warning: game code image is {len(game_code):#x} bytes but overlays.json says {game_code_end_off:#x}", file=sys.stderr)
    dbg_rom_start = int(dbg_code["start"], 16)
    dbg_rom_end = int(dbg_code["end"], 16)

    # Virtual ROM layout: real ROM, then game code image, then game data image.
    game_data_vrom = align(GAME_CODE_VROM + len(game_code), 16)
    vrom = bytearray(rom)
    vrom += game_code
    vrom += b"\0" * (game_data_vrom - len(vrom))
    vrom += game_data

    def word_at_vrom(off: int) -> int:
        return struct.unpack_from(">I", vrom, off)[0]

    # Main section: ROM 0x1000 upward is loaded to RAM by IPL3. We describe code at its
    # TLB-aliased runtime address. The section runs to the start of the font data so the
    # trailing rodata/data (where IDO put main's jump tables) is in-section as well.
    main_section_end_rom = 0x40F10  # font_start in the US RZIP layout
    # overlays.json keeps the decomp's historical CPU cut at 0x292F0, but the RSP boot and audio
    # microcode occupy 0x290D0:0x2AAD0; CPU code ends at 0x290D0.
    main_cpu_code_end = min(main_code_end, 0x290D0)
    main_ovl = Overlay("main", 0x1000, MAIN_VRAM + 0x1000, main_section_end_rom - 0x1000, MAIN_VRAM + main_cpu_code_end)
    game_ovl = Overlay("game", GAME_CODE_VROM, game_vram, len(game_code), game_vram + len(game_code))
    dbg_ovl = Overlay("debugger", dbg_rom_start, dbg_vram, 0x4960, dbg_vram + (dbg_rom_end - dbg_rom_start))
    overlays = {"main": main_ovl, "game": game_ovl, "debugger": dbg_ovl}

    # The game data image lives in normal KSEG0 memory; include it as a data-only section
    # so jump tables or literal pools placed there resolve.
    game_data_vram = 0x80082B20

    # 1. Decomp function inventory.
    inventory = json.loads((decomp / "progress" / "functions.json").read_text())["functions"]
    for entry in inventory:
        region = entry["regions"].get("us")
        if not region or "vram" not in region:
            continue
        overlay = entry.get("overlay", "main")
        vram = int(region["vram"], 16)
        if overlay == "main":
            vram = vram - DECOMP_MAIN_VRAM + MAIN_VRAM
        size = region.get("size_bytes")
        name = entry.get("symbol") or region.get("symbol")
        overlays[overlay].add(vram, size, name, "functions.json")

    # 2. Main ELF symbols (SDK functions and anything else the link knows about).
    if elf_path.exists():
        for value, size, name in read_elf_functions(elf_path):
            if (value >> 28) == 0x8 and DECOMP_MAIN_VRAM + main_code_start <= value < DECOMP_MAIN_VRAM + main_code_end:
                main_ovl.add(value - DECOMP_MAIN_VRAM + MAIN_VRAM, size, name, "elf")
            elif (value >> 24) == 0x16:
                dbg_ovl.add(value, size, name, "elf")
    else:
        print(f"warning: {elf_path} not found; main SDK functions will be discovered from call targets only", file=sys.stderr)

    # 3. SDK symbols the game overlay carries in its tail (names only; sizes from gaps).
    sdk_ld = decomp / "config" / "game" / "us-sdk.ld"
    for match in re.finditer(r"^\s*([A-Za-z_]\w*)\s*=\s*0x([0-9A-Fa-f]+);", sdk_ld.read_text(), re.M):
        name, addr = match.group(1), int(match.group(2), 16)
        for ovl in overlays.values():
            if ovl.contains_code(addr):
                ovl.add(addr, None, name, "us-sdk.ld")

    # 3b. SDK objects duplicated in the game overlay tail (documented per object by the decomp).
    #     Naming them with their libultra names lets N64Recomp redirect the ones the runtime
    #     reimplements (osSetTimer, osGetTime, osEepromRead, osContInit, ...) to native code.
    sdk_members = harvest_game_sdk_members(decomp, game_vram)
    for vram, name in sdk_members:
        game_ovl.add(vram, None, name or f"func_{vram:08X}", "game-sdk-docs")
    report_sdk = {"members": len(sdk_members), "named": sum(1 for _, n in sdk_members if n)}

    # 4. Discover function starts from direct call targets. Every `jal` target is a function
    #    entry, which fills gaps that no inventory covers (e.g. SDK code not yet named).
    jal_targets: set[int] = set()
    for ovl in overlays.values():
        for off in range(0, ovl.code_end - ovl.vram, 4):
            word = word_at_vrom(ovl.rom + off)
            if (word >> 26) == 0x03:  # jal
                pc = ovl.vram + off
                jal_targets.add((pc & 0xF0000000) | ((word & 0x03FFFFFF) << 2))
    discovered = []
    interior_targets = []
    for target in sorted(jal_targets):
        for ovl in overlays.values():
            if ovl.contains_code(target):
                if target in ovl.functions:
                    break
                # Is the target inside a known function? Then the known boundary is suspect.
                prev = max((v for v in ovl.functions if v < target), default=None)
                if prev is not None:
                    prev_entry = ovl.functions[prev]
                    if prev_entry["size"] and prev + prev_entry["size"] > target:
                        # The inventory's boundary covers this call target. Split there: a
                        # direct call always lands on an entry point, so the recorded size
                        # is either a multi-entry handwritten routine or an inventory bug.
                        interior_targets.append({"target": f"{target:#010x}", "inside": prev_entry["name"], "overlay": ovl.name})
                        prev_entry["size"] = target - prev
                ovl.add(target, None, f"func_{target:08X}", "jal-target")
                discovered.append({"overlay": ovl.name, "vram": f"{target:#010x}"})
                break

    # 4a. Addresses formed in code (lui/addiu pairs) and function pointers stored in data.
    #     A candidate starts a new function when it points into a code range at no known start.
    #     Candidates inside a function with a recorded size are jump-table entries and ignored.
    #     When the preceding function has no recorded size (its extent is only implied by the
    #     next start), only the strong signal (an address formed in code) may split it.
    def try_discover(word: int, strong: bool, source: str, sink: list):
        if word & 3:
            return
        for ovl in overlays.values():
            if ovl.contains_code(word) and word not in ovl.functions:
                prev = max((v for v in ovl.functions if v < word), default=None)
                if prev is not None:
                    prev_size = ovl.functions[prev]["size"]
                    if prev_size:
                        if prev + prev_size > word:
                            return  # interior of a sized function
                    elif not strong:
                        return
                ovl.add(word, None, f"func_{word:08X}", source)
                sink.append({"overlay": ovl.name, "vram": f"{word:#010x}", "source": source})
                return

    def lui_addiu_addresses(buf: bytes):
        last_lui: dict[int, tuple[int, int]] = {}
        for off in range(0, len(buf) - 3, 4):
            word = struct.unpack_from(">I", buf, off)[0]
            op = word >> 26
            if op == 0x0F:
                last_lui[(word >> 16) & 31] = ((word & 0xFFFF) << 16, off)
            elif op == 0x09:
                rs = (word >> 21) & 31
                if rs in last_lui and off - last_lui[rs][1] <= 32:
                    imm = word & 0xFFFF
                    if imm & 0x8000:
                        imm -= 0x10000
                    yield (last_lui[rs][0] + imm) & 0xFFFFFFFF

    discovered_more: list = []
    for ovl in overlays.values():
        code_bytes = vrom[ovl.rom:ovl.rom + (ovl.code_end - ovl.vram)]
        for addr in lui_addiu_addresses(code_bytes):
            try_discover(addr, True, "code-address", discovered_more)
    for off in range(0x292F0, main_section_end_rom, 4):
        try_discover(struct.unpack_from(">I", rom, off)[0], False, "data-pointer", discovered_more)
    for buf in (game_data, game_code):
        for off in range(0, len(buf) - 3, 4):
            try_discover(struct.unpack_from(">I", buf, off)[0], False, "data-pointer", discovered_more)
    report_pointers = discovered_more

    # 4b. Conker-specific renames (see CONKER_RENAMES).
    for vram, name in CONKER_RENAMES.items():
        for ovl in overlays.values():
            if vram in ovl.functions:
                ovl.functions[vram]["name"] = name
                ovl.functions[vram]["source"] = "conker-rename"

    # 5. Emit.
    report = {"sections": [], "discovered_from_jal": discovered, "jal_into_function_interior": interior_targets, "game_sdk_tail": report_sdk, "discovered_from_data_pointers": report_pointers}
    lines = ["# Generated by tools/gen_syms.py from the conkers-bfd-decomp inventory. Do not edit by hand.", ""]

    def emit_section(name: str, rom: int, vram: int, size: int, funcs: list[dict], regions=()):
        lines.append("[[section]]")
        lines.append(f'name = "{name}"')
        lines.append(f"rom = {rom:#x}")
        lines.append(f"vram = {vram:#x}")
        lines.append(f"size = {size:#x}")
        if regions:
            lines.append("regions = [")
            for start, end in regions:
                lines.append(f"    {{ start = {start:#x}, end = {end:#x} }},")
            lines.append("]")
        lines.append("functions = [")
        for f in funcs:
            lines.append(f'    {{ name = "{f["name"]}", vram = {f["vram"]:#x}, size = {f["size"]:#x} }},')
        lines.append("]")
        lines.append("")
        covered = sum(f["size"] for f in funcs)
        report["sections"].append({"name": name, "rom": f"{rom:#x}", "vram": f"{vram:#x}", "size": f"{size:#x}", "functions": len(funcs), "code_bytes_covered": covered})

    special = n64recomp_special_names(Path(__file__).resolve().parents[1] / "lib" / "N64Recomp" / "src" / "symbol_lists.cpp")
    seen: dict[str, str] = {}
    for ovl in (main_ovl, game_ovl, dbg_ovl):
        for vram, entry in ovl.functions.items():
            name = entry["name"]
            if name in seen and seen[name] != ovl.name and name not in special:
                entry["name"] = f"{name}__{ovl.name}"
            seen.setdefault(name, ovl.name)

    for ovl in (main_ovl, game_ovl, dbg_ovl):
        funcs = ovl.finalize(word_at_vrom_vram := (lambda v, o=ovl: word_at_vrom(o.rom + (v - o.vram))))
        names = [f["name"] for f in funcs]
        dupes = {n for n in names if names.count(n) > 1}
        if dupes:
            raise SystemExit(f"duplicate function names in {ovl.name}: {sorted(dupes)[:10]}")
        emit_section(ovl.name, ovl.rom, ovl.vram, ovl.size, funcs, GAME_REGIONS if ovl is game_ovl else ())
    # Main's rodata/data (0x800292F0 upward) holds IDO jump tables for main functions. They are
    # addressed through KSEG0, not the 0x10000000 code alias, so describe that range as a
    # data-only section at its KSEG0 address; N64Recomp resolves jump tables through it.
    emit_section("main_data", main_code_end, DECOMP_MAIN_VRAM + main_code_end, main_section_end_rom - main_code_end, [])
    emit_section("game_data", game_data_vrom, game_data_vram, len(game_data), [])

    args.out_syms.parent.mkdir(parents=True, exist_ok=True)
    args.out_syms.write_text("\n".join(lines))
    args.out_rom.parent.mkdir(parents=True, exist_ok=True)
    args.out_rom.write_bytes(vrom)
    if args.report:
        args.report.write_text(json.dumps(report, indent=2))

    for s in report["sections"]:
        print(f"{s['name']:10} vram {s['vram']:>10} functions {s['functions']:5} code bytes covered {s['code_bytes_covered']:#x}")
    print(f"discovered {len(discovered)} function starts from jal targets; {len(interior_targets)} jal targets land inside known functions")
    print(f"game SDK tail: {report_sdk['members']} members from the decomp docs, {report_sdk['named']} given libultra names")
    print(f"discovered {len(report_pointers)} function starts from code-formed addresses and data pointers")
    print(f"virtual ROM: {args.out_rom} ({len(vrom):#x} bytes; game code @ {GAME_CODE_VROM:#x}, game data @ {game_data_vrom:#x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
