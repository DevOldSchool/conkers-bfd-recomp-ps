# Conker memory layout and how the recomp describes it

Conker's Bad Fur Day (US) does not run its code from the usual KSEG0 addresses.
Everything below comes from the ROM itself and from the decomp's evidence
documents; the ROM offsets are for the US release
(SHA-1 `4cbadd3c4e0729dec46af64ad018050eada4f47a`).

## Boot sequence

| Step | Address | What happens |
| --- | --- | --- |
| IPL3 | ROM `0x1000` to RAM `0x80001000` | 1 MiB copied, jump to header entry `0x80001000` |
| Entry stub | `0x80001000:0x80001050` | Clears bss `0x8002D4B0` (`0x16690` bytes), sets `$sp = 0x800314B0`, jumps to `0x80005AB0` |
| TLB setup | `0x80005AB0:0x80005B04` | Writes TLB index 0: EntryHi `0x10000000`, 16 MiB page pair over physical `0x00000000`, then jumps to `0x10001050` |
| First C function | `0x10001050` | `func_80001050` in the decomp |

From then on the main executable runs through the alias at `0x10000000`,
which maps every RAM address `0x8000xxxx` to `0x1000xxxx`. A second wired
entry (index 1) installs an alias at `0xB8000000` (see
`main_tlb_alias_boundary.md` in the decomp).

## Why the alias

`jal` encodes only the low 28 bits of its target. Code at `0x1000xxxx` and
code at `0x15xxxxxx`/`0x16xxxxxx` share the same 256 MiB region, so main,
game and debugger can call each other with plain `jal` instructions. Pointer
scans of the binaries confirm they were **linked** for these addresses:

| Image | Code pointers found | `lui` immediates for code |
| --- | --- | --- |
| main (ROM `0x1000:0x40F10`) | 349 at `0x1000xxxx`, 49 at `0x15xxxxxx` | `0x1000`-`0x1003`, `0x1500`, `0x1600` |
| game code image | 281 at `0x15xxxxxx`, 64 at `0x1000xxxx` | `0x1500`-`0x151F`, `0x1000`-`0x1001` |
| game data image | 8836 at `0x15xxxxxx` | n/a |

Main's data lives in KSEG0 (`0x8002xxxx`-`0x8004xxxx`) and main code reaches
it with `lui 0x8003/0x8004`. IDO put main's jump tables in rodata at
`0x8002Cxxx`; their entries are `0x1000xxxx` code addresses.

## Overlays

| Overlay | Runtime vram | ROM | Notes |
| --- | --- | --- | --- |
| main | `0x10001000` | `0x1000` | 0x282F0 bytes of code to `0x292F0`; RSP audio ucode and data follow; gfx ucodes near `0x3DA48`; fonts from `0x40F10` |
| game | `0x15000000` | `0x42450` (compressed) | 2,072,880 bytes of code+rodata, stored as 4 KiB RZIP chunks behind an XOR-encoded offset table; demand-paged by a TLB-miss handler in main (`0x5C2C`) that inflates one chunk per fault |
| game data | `0x80082B20` | after the code chunks | one RZIP chunk, 189,088 bytes, inflated at startup |
| debugger | `0x16000000` | `0x19EA88` | uncompressed, 0x4960 bytes with 0x36F0 of code |

The decomp names main functions `func_8000xxxx` (its link base), game
functions `func_15xxxxxx` and debugger functions `func_16xxxxxx`.

## What the recomp does with this

* `tools/gen_syms.py` describes the sections at their **runtime** addresses
  (`0x10001000`, `0x15000000`, `0x16000000`) so that every `jal`, every
  `lui/addiu` code address and every jump-table entry resolves consistently.
  The decomp's function names are kept.
* The game overlay is compressed in the ROM, so the generator appends the
  decompressed game code and data images to a copy of the ROM (a "virtual
  ROM" at `build/us/conker.us.recomp.bin`, game code at `0x4000000`). Only
  N64Recomp reads this file; the runtime reads the real ROM and inflates the
  images itself.
* Main's rodata is described as a separate data-only section at its KSEG0
  address. N64Recomp (patched, see `tools/patches/`) resolves jump tables
  through whichever section contains them.
* Recompiled code accesses memory as `rdram + (addr + 0x80000000)`.
  N64ModernRuntime reserves 4 GiB of address space for `rdram`, so
  `0x10000000`, `0x15000000` and `0x16000000` land at offsets `0x90000000`,
  `0x95000000` and `0x96000000` inside the reservation. The runtime must make
  those windows readable: the `0x10000000` window must alias the low RAM
  (shared mapping), and the game and debugger windows receive the inflated
  images. Executing code never needs them, but rodata reads (float literals,
  jump tables, string tables) in the game image do.
* Function-pointer calls (`jalr`) go through the runtime's address lookup.
  It must accept both `0x1000xxxx` and `0x8000xxxx` for main functions, and
  `0x15xxxxxx`/`0x16xxxxxx` for the overlays. Register all three sections
  with the overlay loader at their runtime addresses in the init callback.
* The uncached KSEG1 view (`0xA0000000`) is a third mapping of the same
  shared memory; the RCP register block (`0xA4000000`), the ROM (`0xB0000000`),
  cart domain 2 (`0xBC000000`) and PIF RAM (`0xBFC00000`) get plain windows so
  Rare's direct hardware pokes do not fault. Register-driven PI and SP DMA is
  intercepted in `src/ultra_extras.cpp` instead.
* The handwritten CP0/TLB routines (entry stub, `0x5AB0` TLB setup, the
  demand-paging fault handler and bitmap setup, `0x8120` alias install,
  exception handling) are not recompiled. The native entry function clears bss, sets `$sp` and
  calls `func_80001050`; the demand pager is unnecessary because the whole
  game image is resident.
