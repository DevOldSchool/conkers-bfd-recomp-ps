# RSP microcode in Conker and what the recomp needs

Conker ships three kinds of RSP code: the standard boot code, Rare's audio
microcode (recompiled with RSPRecomp) and Rare's graphics microcode, which RT64
(the renderer N64ModernRuntime expects) needed a patch to understand.

## Summary

| Microcode | Identity | Where in ROM (US) | Status for the recomp |
| --- | --- | --- | --- |
| Boot | standard SGI/Rare `rspboot` | `0x290D0:0x291A0` | nothing to do |
| Audio | Rare `n_aspMain`, Perfect Dark lineage, two overlays plus MP3 | text `0x291A0:0x2AAD0`, data `0x2C960:0x2D4B0` | recompiled by RSPRecomp (`config/aspMain.toml`), plays through the intro and title scene |
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

### Identity

The decomp has a matching assembly source for the audio microcode
(`src/rsp/asp.s`, derived from the Perfect Dark decomp; `./conker rsp`
rebuilds it byte for byte). It is not stock libultra `n_aspMain`: it is Rare's
variant with sixteen commands, among them `MP3` (`0x07`) and `MP3ADDY`
(`0x08`), and a second overlay that does the MP3 transform and synthesis. The
decomp's `src/rsp/README.md` lists what Conker changed relative to Perfect
Dark (central command-count decrement, the resampler lane selection, the
envelope mixer's gain handling).

| Range (US ROM) | Size | MD5 | SHA-1 |
| --- | --- | --- | --- |
| text, overlay 0: `0x291A0:0x2A110` | `0xF70` | `6864e8310f25c15951984eb993d41aca` | `af0878beaa789ae5db8bd0c5c3469df6be9bb68b` |
| text, overlay 1 (MP3): `0x2A110:0x2AAD0` | `0x9C0` | `303b874b553871857297f5ba830841e7` | `dd170eff8ea2269aac3c92d2b87fb3364a1599a3` |
| text, whole: `0x291A0:0x2AAD0` | `0x1930` | `1b74a04911787a80851a9d04881fc1df` | `9eca55c0116a8bb0d52461a2524a79da619c7ab7` |
| data: `0x2C960:0x2D4B0` | `0xB50` | `442fc9d7c4428c46e3ad08757c1524b3` | `31191e64346ea29a4c2ff7d7a8f7bdc781d4ca9c` |
| boot (`rspboot`): `0x290D0:0x291A0` | `0xD0` | `9414dd746eddee59ce6bf97eca16853e` | `9a9289d6ccdbe1ba0577ad77c0704ccf3565d31c` |

The MD5s are the ones the decomp's `config/rsp/us.json` verifies. The audio
task (`func_800095A0`) passes `ucode = 0x100291A0` (main's TLB alias of the
text), `ucode_data = 0x0002C960` and `ucode_data_size = 0x800`. The data's
first 16 bytes are the overlay descriptors: overlay 0 at text offset `0x000`,
`0xF70` bytes, IMEM `0x1080`; overlay 1 at text offset `0xF70`, `0x9C0` bytes,
IMEM `0x1238`. The dispatch table at data `+0x10` gives the handler
addresses listed in `config/aspMain.toml`.

**Where MP3 decoding runs.** It is split. The CPU side in main
(`__conker_mp3_*`, `mp3dec_init` and friends, with Huffman and lookup tables
from RZIP bank `0x17`) parses the bitstream and does the Huffman decoding.
The audio command list then carries `MP3ADDY` (the address of the decoded
frame data) and `MP3`; `cmd_MP3` DMAs overlay 1 into IMEM `0x1238` and the
MP3 data tables over DMEM `0x10`, runs the IMDCT/polyphase synthesis on the
RSP, writes the PCM back to RDRAM and reloads overlay 0 and its data.

### Recompilation

`config/aspMain.toml` describes the text as overlay 0 (`0xF70` bytes at
`0x04001080`) plus one overlay slot at `0x04001238` holding either the rest of
overlay 0 (text offset `0x1B8`, `0xDB8` bytes, which is exactly what
`reload_overlay_0` DMAs back) or overlay 1 (offset `0xF70`, `0x9C0` bytes).
IMEM `0x1BF8:0x1FF0` keeps overlay 0's code while the MP3 overlay is in;
overlay 1 only calls `dma_read`, `dma_wait` and `reload_overlay_0`, all below
the slot. RSPRecomp emits `aspMain_initial` plus one function per slot
permutation (`aspMain0`, `aspMain1`); an IMEM DMA returns
`RspExitReason::SwapOverlay`, and `aspMain` resumes in the permutation that
matches `dma_dram_address - ucode_addr`. The sixteen command handlers are
`extra_indirect_branch_targets`, because they are reached through `jr $at`
from the dispatch table. `tools/regenerate.sh` runs RSPRecomp into
`build/us/rsp/aspMain.cpp`, which CMake compiles at `-O2` into the runtime.
No RSPRecomp change was needed.

### Getting the samples out

Conker does not call the libultra AI functions the runtime replaces by name:

* `func_80002DB0` is libultra's `osAiSetNextBuffer` (8 KiB hardware-bug flag,
  direct `AI_DRAM_ADDR`/`AI_LEN` writes). `tools/gen_syms.py` names it
  `osAiSetNextBuffer`, so the runtime's version queues the buffer.
* The audio frame builder `func_800095A0` reads `AI_LEN` directly and renders
  a shorter frame while at least 249 samples are pending. The RCP registers
  are plain memory in the recomp, so a `[[patches.hook]]` in `config/us.toml`
  replaces the loaded value with `osAiGetLength`.
* `osAiSetFrequency` is already replaced by name. The game asks for 22,020 Hz.

`src/audio.cpp` implements ultramodern's audio callbacks. The RT64 build plays
through SDL2 (an `S16` stereo device opened at the game's rate; SDL converts).
The null build has no device and drains a virtual queue against the wall
clock instead, so the game's `AI_LEN` feedback behaves the same. The game
keeps only about 11 ms buffered, enough for the AI's two-buffer FIFO but not
for a host device that pulls 512-frame periods, so the reported queue is
reduced by a 60 ms cushion (`CONKER_AUDIO_LATENCY_MS`). Without it, SDL
underran about four times a second. With it, there were no underruns and
the queue never dropped below 38 ms.

### Verification (intro and title scene)

Runs of both builds with `CONKER_DUMP_AUDIO` were compared against
independent references from the decomp (`./conker mp3-assets extract`,
`./conker audio-assets soundtrack-preview`):

* **MP3 voice.** Five MP3 streams (`0105`, `0106`, `0107`, `0111`, `0116`;
  0.8 to 2.1 s clips, 22,050 Hz mono, decoded with macOS `afconvert`) were found
  in the intro section of the dump by cross-correlation, with normalised
  correlations of 0.50 to 0.83 despite the music mixed on top. A playback-rate
  sweep peaks sharply at 22,020 / 22,050 = 0.9986 for all five. The decoded
  MP3 samples go out one-to-one at the AI rate, as on hardware, so pitch and
  speed are right. This exercises the RSP MP3 overlay and the overlay swap.
* **Music.** The Nintendo-logo section matches sequence `0054` (IntroMusic,
  "N64 logo chopped up with the chainsaw") with spectral correlation 0.86 at
  0 cents' offset. The title scene matches sequence `0034`
  (CockAndThePluckerBackgroundMusic) at 0.88 to 0.91, also at 0 cents (5-cent
  grid, ±150 cents searched). The ADPCM, resampler and envelope-mixer paths
  are therefore in tune. A clock in the title scene ticks at 1.0001 s
  intervals over 39 ticks.
* **Rate.** Audio is produced in real time: 115.5 s of audio in a 120 s
  headless run, including about 2 s of startup.
* **Clicks.** A second-difference outlier scan finds 3 to 6 isolated
  transients in 118 s, none periodic at the frame size. That rules out
  buffer-boundary discontinuities.

Not verified: tempo against an emulator recording (the decomp's renders are
approximate and the intro section has chainsaw and voice effects on top, so
onset-based tempo comparison was inconclusive), A/V sync against hardware,
reverb and other effects against hardware, and anything past the title
scene. `CONKER_SILENT_AUDIO=1` restores the old silent task.

### Title music lost to a frame-pacing race (fixed)

Before the fix, some headless runs entered the title scene with its clock and
ambience but no sequence `0034`: 1 of 5 sequential runs, and 10 of 33 runs with
three to seven instances in parallel, since host load makes it more likely. Rare's
music manager (`func_8000D758`, called once per game frame) starts a sequence
with `func_80008CE8` + `func_800084D8`. Those only queue SetSeq and Play events
on the player. At its next tick it reads the player state through
`func_8000853C`, and a stopped player (state 0) means "sequence finished": it
frees the record (`func_8000D2F8`, also `func_8000DEC4`). The fade-in and the
per-frame mixing callback of the sequence are then never run, and `0034` stays
at its starting volume of 14, inaudible under the ambience.

The audio thread (`func_80009400`, priority 12, above the game thread's 10)
applies queued events while it renders, and it renders one 736-sample frame
every second retrace. On hardware the frame that starts the title music takes
the CPU long enough, and the audio thread preempts it, so the render always
comes before the manager's next tick. Recompiled, the frame finishes in about
1 ms. After the long loading frame before the title scene, the game's catch-up
pacing starts the next frame on the very next retrace. When the start frame
falls just after a render, that retrace is an odd one with no render, and the
manager sees the new player as stopped. Traced over 12 runs, the music played
whenever the audio thread applied Play within 19.5 ms and was lost whenever it
took 24 ms or more.

A hook on `func_8000853C` (`conker_seqp_state_hook` in `src/hooks.cpp`) reports
a stopped player that still has a Play event queued as playing, which is the
state hardware shows at that point. Without it 10 of 33 runs lost the music.
With it 30 of 30 played `0034` (corr 0.89 to 0.90), and the hook fired in the
runs that would have failed.

The CPU side of MP3 decoding lives in main (`__conker_mp3_*` in the decomp's
ELF) and recompiles normally. The large handwritten block in the game overlay
at `0x150A3A70:0x150AE35C` is not audio at all but Rare's software
geometry/visibility code; it is recompiled as a shared code region (see the
README).
