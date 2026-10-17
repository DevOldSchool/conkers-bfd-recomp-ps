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
| Static recompilation | done: every function recompiles; 13 CP0/TLB/exception routines are stubbed; Rare's handwritten geometry code is recompiled as a shared code region |
| Headless runtime (no graphics, audio or input) | boots: ROM load, game-archive inflation, thread creation, main init, game entry, software-geometry setup, visibility-partition build, main loop, legal and Nintendo screens, intro cutscene; stops entering the title scene (see below) |
| Crash diagnostics | crash reporter with MIPS registers, per-thread call trace, `tools/symbolicate.sh`; display-list dumper and scripted Start press (see "Diagnostics") |
| Graphics: RT64 support for Rare's F3DEX2CBFD microcode | first light: all eight variants recognised, the intro scene renders in an RT64 window (`CONKER_RENDERER=rt64`); see [docs/rsp-microcode.md](docs/rsp-microcode.md) |
| Audio: RSPRecomp of the `n_aspMain` microcode | not started |
| Saving: EEPROM through a PIF shim | volatile in-memory image only |
| Windows support for the memory aliasing | not started |

### Where the headless boot stops

The headless run now plays the boot sequence and the intro cutscene, then
stops around graphics task 1,270 as the game enters scene
`0x1D`, the title scene in the Cock and Plucker bar. Creating that scene's
trigger node `0xC8` (`func_1509B5AC` → `func_150ADAF0(node, 0)`) ends in a call
through a null function pointer ("Failed to find function at 0x00000000"). The
last recompiled calls on that thread are `func_150183D0`, `func_15018500`,
`__conker_mp3_alloc` and `func_80003C6C`. That is the next thing to look at.

With `CONKER_RENDERER=rt64` (PR #3) the scene the run used to sit on renders:
the intro scene, with Conker animated in an armchair under a hanging lamp on a
wooden floor. That picture is the intro's opening state, before its cutscene
script starts (explained below).

![RT64 window at the old steady state](docs/images/rt64-intro.png)

What the display-list dumper (`CONKER_DUMP_DL`) shows, with the screens
identified against emulator screenshots (`./conker mupen-trace
--software-renderer ... -- --testshots`):

| Graphics tasks | Display list | Screen |
| --- | --- | --- |
| 1-110 | 440 commands, 22 texture rectangles | blank or fade |
| 120-260 | 3,283 commands, 380 texture rectangles | legal text (Dolby, Miles, MPEG, Rare) |
| 270-420 | 296 commands, 4 texture rectangles | Nintendo logo |
| 422 on | ~1,900 commands, ~1,100 triangles, alternating buffers `0x10ADC0` / `0x122C20` | intro scene `0x21` (wooden floor, N64 logo, Conker) |

The earlier note that every task from about the 600th on used display list
`0x00122C20` was a sampling artefact: the log printed every 600th task, always
an even one, and the game double-buffers its display lists.

**Why the intro used to hang.** Before the fix in this section, the run never
got past the first shot of the intro scene. It rendered the scene's starting
state forever: Conker's textures are in the display list, but the shot never
changed. The cause was not missing input, EEPROM data, VI messages or audio:

* The intro is cutscene script 1 of scene `0x21`. It is started by a scene
  trigger node: `func_1509BBA0` walks the node list `D_800D2F48` every frame and
  runs each node's action through `func_150ADAF0`; action type 1 is
  `func_1509C440`, which installs the script with `func_1501D348(0x21, 1, ...)`.
  The emulator shows this happening (node at `0x800D5050`).
* In the recomp the node list was empty. Scene loading (`func_15002FB4`)
  reads the scene header's trigger count (`header[0x12]`). It also checks one
  ROM word with `osPiReadIo(0x40F0FAAC ^ 0xF0F0F0F0 = 0xB0000A5C)` and expects
  `0x1740FFF9 ^ 0xF0F0F0F0`. If the check fails, it **halves the trigger
  count**, which looks like Rare's anti-tamper measure. Scene `0x21` has one
  trigger, so halving left none.
* `osPiRawReadIo` in `src/ultra_extras.cpp` treated its argument as a cartridge
  physical address and returned 0 for the KSEG1 address `0xB0000A5C`.
  libultra reads `osRomBase | devAddr`, so the shim now masks the address the
  same way. With that fix the script clock runs from 0 to its end time (1,700
  ticks), the shots play out and the game moves on to scene `0x1D`.

The input shim had nothing to do with it. Controllers are read through
`osContGetReadData` (the runtime's `get_input`), not raw SI DMA. Holding Start
with `CONKER_PRESS_START=1800` at the stuck scene changed nothing, because the
skip check in `func_1501E05C` needs a running cutscene clock of at least 301
ticks. The game made no EEPROM access through the PIF shim before this point.

Other checks of this kind may be present. Any native shim that reads ROM, PI or
PIF state should behave like the hardware for every address form the game uses.

The last crash before the main loop ran was on the audio thread: `func_8000A03C` reads the
message the PI DMA completion puts on the queue at `0x800416F0` as the
`OSIoMesg` it passed to `osPiStartDma` (it compares `dramAddr` against its
sound-bank buffers). The runtime's PI code sent a null message and never
filled in the `OSIoMesg`; `tools/patches/n64modernruntime-pi-dma-message.patch`
makes it behave like libultra's PI manager.

Three translation bugs in the handwritten geometry region were fixed to get
here (the emulator trace of `func_15049260` entries and returns now matches
the recomp call for call):

* the runtime started every thread in the FR=0 FPU register layout, while the
  game runs with FR=1 (IPL3 sets it) and Rare's assembly does arithmetic on
  odd registers; `src/game.cpp` switches each thread to FR=1;
* `jal` inside the region was a nested host call, but the routines return
  through other registers (continuations loaded before the call), so the
  continuation ran twice; calls inside a region now link `$ra` and jump;
* `jr $t0` with a delay slot that writes `$t0`: the target is now read
  before the delay slot.

## Layout

```
config/us.toml          N64Recomp configuration (stub list lives here)
config/us.syms.toml     generated: sections and functions at runtime addresses
tools/gen_syms.py       builds us.syms.toml and the virtual ROM from the decomp
tools/regenerate.sh     extraction + generation + recompilation in one go
tools/patches/          patches applied to the N64Recomp, N64ModernRuntime and RT64 submodules
src/                    runtime glue: entry, memory windows, RZIP, overlays, renderers, window
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
decompressed game images appended, read only by N64Recomp), and runs the
recompiler into `build/us/RecompiledFuncs`.

### Diagnostics

These environment variables are read by the headless binary:

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
* `CONKER_PRESS_START=<vi>[,<vi>...]` holds Start on controller 1 for 10 VIs
  from each listed VI count.

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
  handwritten geometry code (0x150A3A70:0x150AE35C) is recompiled;
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
