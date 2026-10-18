# RSP microcode in Conker and what the recomp needs

Conker ships three kinds of RSP code. Two of them are standard or already
reconstructed; the graphics microcode is the real blocker for a playable
build, because RT64 (the renderer N64ModernRuntime expects) does not know it.

## Summary

| Microcode | Identity | Where in ROM (US) | Status for the recomp |
| --- | --- | --- | --- |
| Boot | standard SGI/Rare `rspboot` | `0x290D0:0x291A0` | nothing to do |
| Audio | Rare `n_aspMain`, Perfect Dark lineage, two overlays plus MP3 | text `0x291A0:0x2AAD0`, data `0x2C960:0x2D4B0` | recompile with RSPRecomp (overlay slots) |
| Graphics | Rare `F3DEX2CBFD`, eight variants | text `0x31A10:0x3C110`, data `0x3D910:0x40F10` | RT64 support in `tools/patches/rt64-f3dex2cbfd.patch` |

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

The strings sit `0x6C0` bytes apart at offset `0x138` of each data segment,
so the data segments are `0x6C0` bytes (standard F3DEX2 data is `0x420`). The
text segments are contiguous in the same order. Their sizes come from the
overlay table at data `+0x6A0` (last overlay offset + size + 1) and match the
spacing between the blobs, so they are exact. No static pointer to these blobs
exists in main, the game code or the game data: the game copies the chosen
variant to RDRAM at runtime (text at `0x000BEBB0`, data at `0x000C0050` in the
first graphics task). Every task in the intro uses `F3DEXBG.NoN`; the RDRAM
copy matches the ROM byte for byte.

| Variant | Text (ROM) | Text size | Text XXH3 | Data (ROM) | Data XXH3 (`0x6C0`) |
| --- | --- | --- | --- | --- | --- |
| F3DEX.NoN | `0x31A10` | `0x1510` | `0x5C1B21290CECB813` | `0x3D910` | `0xD7843E48A0715C70` |
| F3DEX | `0x32F20` | `0x1510` | `0x03264D6F0EC69D2C` | `0x3DFD0` | `0xE66E0527A610422F` |
| F3DEXBS.NoN | `0x34430` | `0x1528` | `0x4D6DA143A67D0816` | `0x3E690` | `0x950AAC1BC68963DC` |
| F3DEXBS | `0x35960` | `0x1528` | `0x03A207C092908241` | `0x3ED50` | `0xF8D85E56C313A4FD` |
| F3DEXBG.NoN | `0x36E90` | `0x14A0` | `0x54C7842FDDBB6324` | `0x3F410` | `0x45CACD155D8ADF4E` |
| F3DEXBG | `0x38330` | `0x14A0` | `0x8EC722205C465CFC` | `0x3FAD0` | `0xFE11F9B90D58B251` |
| F3DEXBGBS.NoN | `0x397D0` | `0x14A0` | `0x3873EBBE4229648B` | `0x40190` | `0x020848863D5A2C5B` |
| F3DEXBGBS | `0x3AC70` | `0x14A0` | `0xF9D3730B1BF8508E` | `0x40850` | `0x185B27B2D4CF4013` |

Hashes are XXH3-64 over the blob as RT64 sees it, in RDRAM byte order (each
32-bit word byte-swapped from the ROM), which is what `getGBIForUCode`
hashes. The BS variants are `0x18` bytes longer than the plain ones and the BG
variants `0x70` shorter, all in the second overlay (`0x1188` onwards: sizes
`0x387`, `0x39F` and `0x317`). The text of every variant differs from the
others, so what BS and BG change in command handling is still unknown; the
RT64 patch treats all eight as the same command set.

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
The RT64 patch (`tools/patches/rt64-f3dex2cbfd.patch`, applied in
`lib/rt64`) adds it like this:

1. **Database entries.** Eight `GBIInstance`s (the four `.NoN` builds with the
   NoN flag) with the hashes above, mapped to a new `GBIUCode::F3DEX2CBFD`.
   RT64 also prints `GBI: <name>` when it matches a microcode.
2. **GBI table.** `GBI_F3DEX2CBFD::setup` (`src/gbi/rt64_gbi_f3dex2cbfd.cpp`)
   starts from `GBI_F3DEX2::setup` and overrides VTX, MOVEWORD (NUMLIGHT
   `/48`, LIGHTCOL `/48`, COORD_MOD `0x10`; the rest goes to F3DEX2), MOVEMEM
   (LIGHT offsets in 48-byte units from w0 bits 5..18, `0x0E` sets the normal
   base) and LOAD_UCODE (advanced lighting on), and installs TRI4 at
   `0x10..0x1F` as up to four `drawIndexedTri` calls (`0,0,0` slots skipped).
   A task reset clears the lights and the advanced-lighting switch, as
   GLideN64 does.
3. **RSP state.** `RSP::cbfd` holds sixteen decoded light records (colour,
   normalised direction, 16-bit position, attenuation `byte12 / 16`), the
   sixteen coordinate modifiers, the normal base and the light count. Lit
   vertices are shaded on the CPU in `RSP::lightVerticesCBFD`, following
   GLideN64's `gSPLightVertexCBFD_basic`/`_advanced`: the ambient light, point
   lights against the clip-space position run through the coordinate
   modifiers, and in advanced mode the directional light against the normal
   (X/Y from `normalBase + index * 2`, Z from the low byte of the flag).
   Vertices with flag bit 15 set keep their colour. The lit colour multiplies
   the vertex colour. It goes into the vertex colour, so the GPU does no
   lighting, except with texgen: there the vertex bytes must hold the normal
   for RT64's texgen shader, and the colour is passed as a per-vertex ambient
   light.
4. **RDP side.** Unchanged: CBFD uses the standard RDP command set.

Still open: the lighting follows GLideN64 and has not been checked against
the real microcode or a hardware capture; the wooden floor in the intro looks
streaky (texture filtering, LOD or texture coordinates, not yet investigated);
and nothing is known about what the BS/BG variants change. The large grey
shapes on the weasels in the title scene are their tails and match a
cycle-accurate emulator render; they are not a CBFD decoding error (see the
README).

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
