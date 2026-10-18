# Conker's Bad Fur Day: Recompiled

A static recompilation of *Conker's Bad Fur Day* (Nintendo 64, US release)
built with [N64Recomp](https://github.com/N64Recomp/N64Recomp) and
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime), fed by the
[conkers-bfd-decomp](https://github.com/DevOldSchool/conkers-bfd-decomp)
project's function inventory.

The plan is the hybrid one: start from a full static recompilation so there
is a running build now, and replace recompiled functions with decompiled C as
the decomp finishes them. The decomp is at roughly 27% matched code; this
project does not wait for it.

This repository contains no ROM data. You need your own US ROM
(SHA-1 `4cbadd3c4e0729dec46af64ad018050eada4f47a`).

## Status

| Piece | State |
| --- | --- |
| Symbol file from the decomp inventory, ELF and evidence docs | done: 5,900+ functions across main, game and debugger overlays, SDK copies in the game overlay named so the runtime replaces them |
| Static recompilation | done: every function recompiles; 13 CP0/TLB/exception routines are stubbed; Rare's handwritten geometry code and one handwritten prefix-code tree builder are recompiled as shared code regions |
| Runtime | boots through the legal and Nintendo screens, intro cutscene, title scene, menus and the new game's opening cutscene with scripted Start presses; no known crash (see "Current state") |
| Crash diagnostics | crash reporter with MIPS registers, per-thread call trace, `tools/symbolicate.sh`; display-list dumper and scripted Start press (see "Diagnostics") |
| Graphics: RT64 support for Rare's F3DEX2CBFD microcode | working through the title scene and menus: all eight variants recognised, the intro cutscene, the title scene and the menus render in an RT64 window (`CONKER_RENDERER=rt64`); the title scene matches a cycle-accurate emulator render, including the weasels' tails that looked like stray polygons; see [docs/rsp-microcode.md](docs/rsp-microcode.md) |
| Audio: RSPRecomp of the `n_aspMain` microcode | working through the intro and title scene: Rare's MP3-capable audio microcode recompiled with its overlays, SDL2 output in the RT64 build, WAV dump with `CONKER_DUMP_AUDIO`; music in tune and MP3 voice at the right pitch and speed (checked against the decomp's extracted assets); title music no longer drops out (a timing race fixed by a hook in `src/hooks.cpp`); see [docs/rsp-microcode.md](docs/rsp-microcode.md#audio-n_aspmain-with-the-mp3-overlay) |
| Input | not started: controller 1 only sees the scripted Start presses from `CONKER_PRESS_START` |
| Saving: EEPROM through a PIF shim | volatile in-memory image only |
| Windows support for the memory aliasing | not started |

### Current state

The headless and RT64 builds play the boot sequence, the intro cutscene
(scene `0x21`) and the title scene `0x1D` in the Cock and Plucker bar, which
then waits for input. With scripted Start presses
(`CONKER_PRESS_START=5400,6000,6600,7200,7800,9000,10200`) the run goes
through the bar doors to the NEW GAME menu, loads scene `0x18` and plays the
new game's opening credits and throne-room cutscene; the later presses pause
and resume it. A 5.5-minute run with those presses ends on the timer, not a
crash, and there is no known crash.

There is no real controller input yet: the only input is the scripted Start
press, so nothing past the opening cutscene has been tried.

In RT64 the title scene and the menus look right and match a cycle-accurate
emulator render. The grey shapes on the weasels are their tails, drawn the
same way by the original game. That analysis, and the fixes that got the boot
this far, are written up in [docs/investigations.md](docs/investigations.md).

![RT64 window: title scene](docs/images/rt64-title.png)

![RT64 window: NEW GAME menu](docs/images/rt64-new-game.png)

## Layout

```
config/us.toml          N64Recomp configuration (stub list lives here)
config/us.syms.toml     generated: sections and functions at runtime addresses
config/aspMain.toml     RSPRecomp configuration for the audio microcode
tools/gen_syms.py       builds us.syms.toml and the virtual ROM from the decomp
tools/regenerate.sh     extraction + generation + recompilation in one go
tools/cbfd_dl.py        offline display-list replay from a CONKER_DUMP_RDRAM image
tools/patches/          patches applied to the N64Recomp, N64ModernRuntime and RT64 submodules
src/                    runtime glue: entry, memory windows, RZIP, overlays, renderers, window, audio
docs/memory-layout.md   why main runs at 0x10001000 and the game at 0x15000000
docs/rsp-microcode.md   the microcode situation and the RT64 work
lib/N64Recomp           recompiler (submodule, patched; see tools/patches)
lib/N64ModernRuntime    libultra reimplementation and recomp glue (submodule)
lib/rt64                renderer (submodule, patched for F3DEX2CBFD; CONKER_RENDERER=rt64)
```

## Building

Requirements: CMake 3.20+, Ninja, clang with C++20, Python 3, and a checkout
of the decomp next to this one (`../conkers-bfd-decomp`) with the ROM at
`roms/baserom.us.z64`. The decomp's `build/us/original-assets/conker.us.elf`
(from `./conker build`) improves symbol names for main's SDK code but is
optional.

```sh
git submodule update --init --recursive
(cd lib/N64Recomp && git apply ../../tools/patches/n64recomp.patch)
(cd lib/N64ModernRuntime && git apply ../../tools/patches/n64modernruntime-running-thread.patch)
(cd lib/N64ModernRuntime && git apply ../../tools/patches/n64modernruntime-pi-dma-message.patch)
tools/regenerate.sh
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/host
build/host/ConkerRecomp /path/to/conker-us.z64
```

The default build uses the null renderer: no window, graphics tasks are only
logged. For a window rendered by RT64, apply the CBFD patch to the RT64
submodule and configure with `CONKER_RENDERER=rt64`. This needs SDL2
(`brew install sdl2`) and, on macOS, Xcode's Metal toolchain for RT64's shaders
(`xcodebuild -downloadComponent MetalToolchain` on Xcode 26 or later):

```sh
(cd lib/rt64 && git apply ../../tools/patches/rt64-f3dex2cbfd.patch)
cmake -S . -B build/rt64 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCONKER_RENDERER=rt64
cmake --build build/rt64
build/rt64/ConkerRecomp /path/to/conker-us.z64
```

RT64 prints `GBI: F3DEXBG.NoN fifo 2.08 (Conker)` when it recognises the
microcode of the first graphics task.

On a crash the binary prints the faulting N64 address, the MIPS registers,
the last 64 recompiled function entries on that thread and the image slide;
`tools/symbolicate.sh build/host/ConkerRecomp <log>` maps the host backtrace
to generated C lines and N64 instruction addresses.

`regenerate.sh` extracts the compressed game overlay with the decomp's RZIP
tool, writes `config/us.syms.toml` plus a virtual ROM (the real ROM with the
decompressed game images appended, read only by N64Recomp), runs the
recompiler into `build/us/RecompiledFuncs` and RSPRecomp into
`build/us/rsp/aspMain.cpp`. From a git worktree, point it at the decomp with
`CONKER_DECOMP=/path/to/conkers-bfd-decomp`.

The RT64 build plays sound through SDL2; the null build renders the audio but
has no output device.

### Diagnostics

These environment variables are read by both builds:

* `CONKER_DUMP_DL=<task>[,<task>...]` prints every command of the listed
  graphics tasks (1-based). It walks the F3DEX2-style display list from the
  task's data pointer, follows `gSPDisplayList` (`DE`), returns at
  `gSPEndDisplayList` (`DF`), applies `gSPSegment` bases and resolves image
  addresses (`FD`/`FE`/`FF`). While the variable is set, a one-line summary is
  also printed every 60 tasks and whenever the command count changes: command,
  list, triangle and rectangle counts, colour image, content hash and texture
  addresses. `CONKER_DUMP_DL=0` gives only the summaries.
* `CONKER_DUMP_RDRAM=1` also writes `conker-task-<n>.rdram` (8 MiB,
  big-endian) for each fully dumped task, so its textures can be decoded
  offline.
* `tools/cbfd_dl.py <rdram> <dl>` replays one of those RDRAM images offline
  without RT64. It tracks segments, matrices, the viewport and the vertex
  buffer, prints each model instance's screen-space bounding box, and with
  `--largest N [--instance <seg3>]` prints the largest triangles and where
  their vertices came from. Use it to tell bad data in RDRAM apart from a
  renderer that misreads good data.
* `CONKER_PRESS_START=<vi>[,<vi>...]` holds Start on controller 1 for 10 VIs
  from each listed VI count.
* `CONKER_DUMP_AUDIO=<file.wav>` records every buffer the game queues
  (16-bit stereo at the game's 22,020 Hz) in either build. The log prints a
  line every 5 s of audio with the peak, RMS, minimum output-queue depth and
  underrun count.
* `CONKER_AUDIO=0` (RT64 build) disables the SDL device; `CONKER_AUDIO_LATENCY_MS`
  (default 60) sets the extra buffering kept on top of what the game asks for;
  `CONKER_SILENT_AUDIO=1` skips the audio microcode entirely.

## How the recompilation is set up

* Conker links its main executable for a TLB alias at `0x10000000` and its
  game overlay for `0x15000000`, demand-paged 4 KiB at a time from RZIP
  chunks. Sections are described at those runtime addresses so every call,
  address constant and jump table resolves. Details in
  [docs/memory-layout.md](docs/memory-layout.md).
* Function boundaries come from the decomp's `progress/functions.json`, its
  ELF for main's SDK code, `config/game/us-sdk.ld` for SDK code in the game
  overlay, and direct-call targets for anything left uncovered.
* The runtime makes the `0x10000000` window a shared-memory alias of RDRAM,
  inflates the game overlay into the `0x15000000` window, copies the debugger
  overlay to `0x16000000`, and registers every function at its runtime
  address so function-pointer calls resolve.
* The 13 stubbed functions are listed with reasons in `config/us.toml`.
* libultra internals Rare calls directly (SI/PIF access, SP task submission by
  raw DMA, VI/timer internals, controller-pak code, libc helpers) are
  implemented natively in `src/ultra_extras.cpp`; `tools/gen_extras_decl.py`
  keeps the list of required ones in sync.
* `[[patches.hook]]` snippets in `config/us.toml` call helpers in
  `src/hooks.cpp`, for example to keep the music manager from dropping a
  sequence the audio thread has not started yet (see
  [docs/rsp-microcode.md](docs/rsp-microcode.md)).

## Patches to the submodules

`tools/patches/n64recomp.patch` (apply in `lib/N64Recomp`):

* jump tables may live in a different section from their function (IDO puts
  main's tables in rodata at a KSEG0 address while the code runs through the
  TLB alias);
* one run reports every failing function instead of stopping at the first;
* **shared code regions**: a section may declare `regions = [{ start, end }]`.
  Every function inside is emitted as a thin wrapper around one body per
  region, which begins with a `switch` over every instruction address. Entries,
  computed jumps (`jr $reg`) and `jr $ra` with an explicitly loaded `$ra`
  resolve to labels; the wrapper seeds `$ra` with a sentinel so routines that
  return through another register (`or $t7, $ra` ... `jr $t7`) still return.
  Calls inside a region to targets in the same region link `$ra` and jump to
  the label (hardware semantics, since the callee may return through a
  continuation register), `jalr` links and dispatches, and the jump register
  of `jr`/`jalr` is read before the delay slot runs. This is how Rare's
  handwritten geometry code (0x150A3A70:0x150AE35C) and the handwritten
  prefix-code tree builder `func_15018500` (0x15018500:0x150186D0, returns
  through `$s4`) are recompiled;
* stack accesses with negative or unaligned offsets, tables without in-function
  entries and undecodable words no longer fail a function; writes to `$zero`
  are discarded.

`tools/patches/n64modernruntime-running-thread.patch` (apply in
`lib/N64ModernRuntime`) adds `ultramodern::set_running_thread_address`, a
libultra-style `__osRunningThread` mirror that Rare's code reads directly.

`tools/patches/n64modernruntime-pi-dma-message.patch` (apply in
`lib/N64ModernRuntime`) makes `osPiStartDma` fill in the `OSIoMesg` (type,
priority, return queue, addresses, size) and makes PI DMA completion send that
`OSIoMesg` pointer to the return queue, as libultra's PI manager does, instead
of a null message.

`tools/patches/rt64-f3dex2cbfd.patch` (apply in `lib/rt64`) adds Rare's
F3DEX2CBFD microcode: GBI database entries for the eight variants, a
`GBI_F3DEX2CBFD` command table on top of F3DEX2 and the RSP state for its
vertex normals, 48-byte lights, coordinate modifiers and TRI4. Details in
[docs/rsp-microcode.md](docs/rsp-microcode.md).

All of these are candidates for upstreaming.
