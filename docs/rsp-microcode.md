# RSP microcode in Conker and what the recomp needs

Conker ships three kinds of RSP code. Two of them are standard or already
reconstructed; the graphics microcode is the real blocker for a playable
build, because RT64 (the renderer N64ModernRuntime expects) does not know it.

## Summary

| Microcode | Identity | Where in ROM (US) | Status for the recomp |
| --- | --- | --- | --- |
| Boot | standard SGI/Rare `rspboot` | `0x290D0:0x291A0` | nothing to do |
| Audio | Rare `n_aspMain`, Perfect Dark lineage, two overlays plus MP3 | text `0x291A0:0x2AAD0`, data `0x2C960:0x2D4B0` | recompile with RSPRecomp (overlay slots) |
| Graphics | Rare `F3DEX2CBFD`, eight variants | version strings at `0x3DA48` through `0x40988` | **needs an RT64 implementation** |

## Graphics: F3DEX2CBFD

The ROM carries eight builds of one Rare-modified F3DEX2 (2.08) microcode:

```
RSP Gfx ucode F3DEX.NoN   fifo 2.08  Yoshitaka Yasumoto 1999 Nintendo.
RSP Gfx ucode F3DEX       fifo 2.08  ...
RSP Gfx ucode F3DEXBS.NoN fifo 2.08  ...
RSP Gfx ucode F3DEXBS     fifo 2.08  ...
RSP Gfx ucode F3DEXBG.NoN fifo 2.08  ...
RSP Gfx ucode F3DEXBG     fifo 2.08  ...
RSP Gfx ucode F3DEXBGBS.Nofifo 2.08  ...
RSP Gfx ucode F3DEXBGBS   fifo 2.08  ...
```

The strings sit `0x6C0` bytes apart, so the data segments are `0x6C0` bytes
(standard F3DEX2 data is `0x420`). Autocorrelation of the surrounding region
puts the text segments at `0x14A0` bytes each (standard: `0x1390`). No static
pointer to these blobs exists in main, the game code or the game data; the
task setup builds the addresses at runtime. The headless runtime prints the
`OSTask` ucode/data pointers and sizes on the first graphics task, and RT64
prints `HASH:` lines for microcodes it does not recognise, so the exact
boundaries and XXH3 hashes fall out of the first run.

GLideN64 implements this microcode as `F3DEX2CBFD`
(`src/uCodes/F3DEX2CBFD.cpp`, 150 lines, plus `gSPCBFDVertex`,
`gSPLightCBFD` and `gSPLoadCBFDVertexData` in `gSP.cpp`). The decomp's
`docs/runtime-tracing.md` independently documents the same command deltas
from traced display lists. The differences from stock F3DEX2 are:

| Command | Stock F3DEX2 | CBFD |
| --- | --- | --- |
| `01` VTX | `n` in w0 bits 12..19, `v0+n` in bits 1..7 | same fields, but normals are **not** in the vertex: X/Y come from a separate normal stream at `vertexNormalBase + index*2`, Z from the low byte of the vertex flag; vertex colour is used as-is |
| `DB` MOVEWORD `G_MW_NUMLIGHT` | `w1 / 24` lights | `w1 / 48` lights (48-byte light records) |
| `DB` MOVEWORD index `0x10` (`G_MW_COORD_MOD`) | n/a | sixteen coordinate-modifier values; w0 bits select slot and which of three groups is written |
| `DC` MOVEMEM `G_MV_LIGHT` | offset / 24 → lookat 0..1, lights 2.. | offset (w0 bits 5..18) / 48; records hold two RGB triplets, a signed direction, an attenuation byte at `+12`, and a 16-bit position XYZW at `+32` |
| `DC` MOVEMEM index `0x0E` (`G_MV_NORMALES`) | matrix | sets the vertex-normal base address |
| `DD` LOAD_UCODE | load new microcode | repurposed: switches to the "advanced lighting" path |
| `10`..`1F` | unused | `TRI4`: four triangles per command (5-bit indices packed across w0/w1) |

RT64 already has a precedent for Rare microcodes (`F3DPD`, `F3DGOLDEN`, each a
small delta on F3D) and a hash-keyed GBI database (`src/gbi/rt64_gbi.cpp`).
Adding CBFD means:

1. **Database entries.** Eight `GBIInstance`s (four with the NoN flag) with
   text and data hashes taken from the runtime log, mapped to a new
   `GBIUCode::F3DEX2CBFD`.
2. **GBI table.** `GBI_F3DEX2CBFD::setup` that starts from
   `GBI_F3DEX2::setup` and overrides VTX, MOVEWORD, MOVEMEM, LOAD_UCODE and
   installs TRI4 at `0x10..0x1F`. Mechanically the same shape as GLideN64's
   `F3DEX2CBFD_Init`.
3. **RSP state (the real work).** `RT64::RSP` (`src/hle/rt64_rsp.cpp`,
   ~1300 lines) needs: a vertex path that reads normals from the separate
   stream and the flag byte; 48-byte light records with the second colour,
   position and attenuation; the sixteen coordinate modifiers; the
   advanced-lighting switch; and `drawIndexedTri` called four times for TRI4.
   GLideN64's `gSPLoadCBFDVertexData` and `gSPLightCBFD` are the reference
   for the first two. What the "BS"/"BG" variants change beyond the common
   CBFD command set is not yet documented anywhere; diffing the eight text
   blobs against each other will show which command handlers differ.
4. **RDP side.** Unchanged: CBFD uses the standard RDP command set, which RT64
   already handles.

This is a contained renderer feature, not a research project: the command set
is known from two independent sources and the reference implementation is
small. It is the first thing to build once the headless runtime boots.

## Audio: n_aspMain with the MP3 overlay

The decomp has a matching assembly source for the audio microcode
(`src/rsp/asp.s`, derived from the Perfect Dark decomp) and documents the
layout: overlay 0 is `0x0000:0x0F70` of the text and loads at IMEM `0x1080`;
overlay 1 (MP3) is `0x0F70:0x1930` and loads at IMEM `0x1238`, overlapping
the middle of overlay 0. Initialised data is `0xB50` bytes.

N64Recomp's `RSPRecomp` tool recompiles RSP text to C and supports
`overlay_slots` (a `text_address` with a list of `{offset, size}` overlays
swapped in by IMEM DMA), which is exactly this shape. A config to try:

```toml
text_offset = 0x291A0
text_size = 0x1930
text_address = 0x1080
rom_file_path = "../build/us/conker.us.recomp.bin"
output_file_path = "../build/us/rsp/aspMain.cpp"
output_function_name = "aspMain"
[[overlay_slots]]
text_address = 0x1238
overlays = [
    { offset = 0x1B8, size = 0x9C0 },  # the part of overlay 0 that lives at 0x1238
    { offset = 0xF70, size = 0x9C0 },  # overlay 1 (MP3)
]
```

Until that is done the runtime's `get_rsp_microcode` returns a task that
completes without writing samples, so the game runs silently.

The CPU side of MP3 decoding lives in main (`__conker_mp3_*` in the decomp's
ELF) and recompiles normally. The large handwritten block in the game overlay
at `0x150A3A70:0x150AE35C` is not audio at all but Rare's software
geometry/visibility code; it is recompiled as a shared code region (see the
README).
