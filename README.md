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
(cd lib/rt64 && git apply ../../tools/patches/rt64-native-size.patch)
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
the keyboard at the same time. There is no remapping yet. A pad that can
rumble (DualSense, DualShock 4, Xbox, Switch Pro) counts as a Rumble Pak in
controller 1, so connect it before starting the game, which looks for the pak
at boot. `CONKER_RUMBLE_TEST=1` buzzes the pad for a second when it connects.

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

Keys held with Cmd or Alt are not passed to the game, so the full-screen
shortcuts (Cmd+Enter, Alt+Enter) do not press Start.

## Enhancements

* **Display options:** the OPTIONS page of the title menu has
  three rows under the sound setting.
  * WINDOWED, BORDERLESS or FULL SCREEN. Full screen (the default) is macOS
    full screen in a Space of its own; borderless covers the screen in the
    current Space, with the menu bar and Dock hidden while the game is in
    front.
  * The resolution: how large the game's 292x216 picture is rendered, in
    whole multiples from 292x216 (1x). In a window it is the window's size,
    up to the largest that fits the display (1460x1080, 5x, on a 2560x1440
    display with the Dock showing, which is also the default). Covering the
    screen it goes up to the first multiple that covers the screen's height
    (2044x1512, 7x, on 1440 lines, the default).
  * FILL SCREEN or CENTERED: the picture is scaled to fill the window or
    screen, or drawn at its own size in the middle with black bars, so that
    every game pixel is an even block. Covering the screen it fills by
    default; a lower resolution then fills it with larger pixels. A window
    is centered by default, which only shows once it is resized by hand.

  Each mode keeps its own resolution and fill. F11, Cmd+Enter, Alt+Enter or
  Ctrl+Cmd+F switch between the window and the last screen-covering mode.
  The choices are kept in `conker-recomp-data/display.cfg`, not in the
  game's save, so the OPTIONS saved to EEPROM are unchanged.
  `CONKER_DISPLAY=windowed`, `borderless` or `fullscreen` overrides the mode
  for one run, `CONKER_DISPLAY_OPTIONS=0` hides the rows, and
  `CONKER_ORIGINAL_SIZE=1` opens the original 960x720 window instead.
* **Skip to the title menu:** Start at any point during the boot logos or the
  intro fades to black and then into the GAME 1 menu, skipping the title
  scene's opening (the clock and the Cock and Plucker) too. On an N64, Start only skips the intro after about five seconds and after
  the reset button, and the title scene needs another press.
  `CONKER_SKIP_INTRO=1` skips without a press; `CONKER_SKIP_INTRO=0` keeps the
  original behaviour, where the intro cannot be skipped after a cold boot.
* **SAVE and LOAD in the pause menu:** two signs sit under the lever, LOAD
  below QUIT and SAVE below CONT.... Push down from the sign above, or use
  the stick's down-left and down-right diagonals, and press A. SAVE saves
  the game where Conker stands, and LOAD, or starting that save from the
  title menu, puts him back on that spot. Quitting from the pause menu keeps
  the spot too. The spot is kept in `conker_us.resume` next to the save; a
  checkpoint reached later takes over as usual. Enemies and other things
  the save does not record are reset, as at a checkpoint. Neither sign is
  offered when replaying a chapter, where the game does not save.
  `CONKER_PAUSE_SAVE=0` turns them off.
* **Surround by default:** with no saved options (a new save file), the
  OPTIONS menu's sound setting is surround instead of stereo. The
  game keeps its options in EEPROM next to the saved games, so a sound
  setting chosen and saved from that menu stays as it was.
  `CONKER_SURROUND_DEFAULT=0` keeps the stereo default.
* **No Rumble Pak banner with a rumbling pad:** the title menu's "INSERT
  RUMBLE PAK" banner shows on an N64 even with the pak inserted. It is left
  out when controller 1's pad can rumble; `CONKER_RUMBLE_BANNER=1` shows it.
* **Camera on the right stick:** C-left and C-right are swapped (also on J/L),
  so pushing left turns the view left.

## Documentation

* [docs/diagnostics.md](docs/diagnostics.md): crash reports, display-list and
  audio dumps, scripted input, skipping the opening with a save
* [docs/recompilation.md](docs/recompilation.md): how the recompilation is set
  up and what the submodule patches change
* [docs/memory-layout.md](docs/memory-layout.md): the TLB layout
* [docs/rsp-microcode.md](docs/rsp-microcode.md): graphics and audio microcode
* [docs/investigations.md](docs/investigations.md): open problems and what
  has been ruled out
