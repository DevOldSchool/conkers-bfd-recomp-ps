# Conker's Bad Fur Day: Recompiled

A static recompilation of *Conker's Bad Fur Day* (Nintendo 64, US) built with
[N64Recomp](https://github.com/N64Recomp/N64Recomp) and
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime), using the
function inventory from
[conkers-bfd-decomp](https://github.com/DevOldSchool/conkers-bfd-decomp).
Recompiled functions are replaced with decompiled C as the decomp matches them.

This repository contains no ROM data. You need your own US ROM
(SHA-1 `4cbadd3c4e0729dec46af64ad018050eada4f47a`).

## Status

The RT64 build boots through the intro, title scene and menus, plays the new
game's opening, saves, and reaches the first playable area, where Conker can
be walked around. Saved games load from the title menu. There is no known
crash. Graphics (Rare's F3DEX2CBFD microcode in RT64), audio (the `n_aspMain`
microcode through RSPRecomp) and EEPROM saves all work that far. Windows is
not supported yet.

## Building

Requirements: CMake 3.20+, Ninja, clang with C++20, Python 3, SDL2
(`brew install sdl2`), and a checkout of the decomp next to this one
(`../conkers-bfd-decomp`) with the ROM at `roms/baserom.us.z64`. On macOS,
RT64 also needs Xcode's Metal toolchain
(`xcodebuild -downloadComponent MetalToolchain` on Xcode 26 or later).

```sh
git submodule update --init --recursive
(cd lib/N64Recomp && git apply ../../tools/patches/n64recomp.patch)
(cd lib/N64ModernRuntime && git apply ../../tools/patches/n64modernruntime-running-thread.patch)
(cd lib/N64ModernRuntime && git apply ../../tools/patches/n64modernruntime-pi-dma-message.patch)
(cd lib/rt64 && git apply ../../tools/patches/rt64-f3dex2cbfd.patch)
tools/regenerate.sh
cmake -S . -B build/rt64 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCONKER_RENDERER=rt64
cmake --build build/rt64
build/rt64/ConkerRecomp /path/to/conker-us.z64
```

Leave out `-DCONKER_RENDERER=rt64` for the null build: no window, sound or
input, useful for scripted runs. From a git worktree, point `regenerate.sh` at
the decomp with `CONKER_DECOMP=/path/to/conkers-bfd-decomp`.

## Controls

Controller 1 is read from the first SDL game controller (hot-pluggable) and
the keyboard at the same time. There is no remapping yet.

| N64 | Xbox | PlayStation | Keyboard |
| --- | --- | --- | --- |
| Analog stick | Left stick | Left stick | W A S D / arrow keys |
| A | A | Cross | Space |
| B | B or X | Circle or Square | Left Shift |
| Z | LT or RT | L2 or R2 | Q |
| L | LB | L1 | Tab |
| R | RB | R1 | E |
| Start | Start | Options | Enter |
| C buttons | Right stick | Right stick | I J K L |
| D-pad | D-pad | D-pad | T F G H |

## Enhancements

* **Skip to the title menu:** Start at any point during the boot logos or the
  intro fades to black and then into the GAME 1 menu, skipping the title
  scene's opening (the clock and the Cock and Plucker) too. On an N64, Start only skips the intro after about five seconds and after
  the reset button, and the title scene needs another press.
  `CONKER_SKIP_INTRO=1` skips without a press; `CONKER_SKIP_INTRO=0` keeps the
  original behaviour, where the intro cannot be skipped after a cold boot.
* **Camera on the right stick:** C-left and C-right are swapped (also on J/L),
  so pushing left turns the view left.

## Documentation

* [docs/diagnostics.md](docs/diagnostics.md): crash reports, display-list and
  audio dumps, scripted input, skipping the opening with a save
* [docs/recompilation.md](docs/recompilation.md): how the recompilation is set
  up and what the submodule patches change
* [docs/memory-layout.md](docs/memory-layout.md): the TLB layout
* [docs/rsp-microcode.md](docs/rsp-microcode.md): graphics and audio microcode
* [docs/investigations.md](docs/investigations.md): bugs fixed so far
