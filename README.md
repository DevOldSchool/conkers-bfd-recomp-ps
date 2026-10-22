# Conker's Bad Fur Day: Recompiled

This is my attempt at a faithful PC port of *Conker's Bad Fur Day*
(Nintendo 64, US) for Windows, macOS and Linux. The aim is the game as Rare
made it: the same look, sound and feel, running natively. On top of that
come a few small quality-of-life changes, such as widescreen, higher
resolutions, sharper text and menus and skippable cutscenes, listed under
[Enhancements](#enhancements). They stay out of the way of the original
style, and most can be switched off.

It is a static recompilation built with
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
microcode through RSPRecomp) and EEPROM saves all work that far.

Platforms:

* macOS (the only one tested so far)
* Windows
* Linux

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
(cd lib/rt64 && git apply ../../tools/patches/rt64-edge-rects.patch)
(cd lib/rt64 && git apply ../../tools/patches/rt64-memory-packs.patch)
(cd lib/rt64 && git apply ../../tools/patches/rt64-wireframe.patch)
(cd lib/rt64 && git apply ../../tools/patches/rt64-wide-draw-rect.patch)
(cd lib/rt64/src/contrib/plume && git apply ../../../../../tools/patches/plume-wireframe.patch)
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
On macOS, DualSense and DualShock 4 pads are read through Apple's
GameController framework, so they keep working in other apps (OpenEmu, for
one) after the game quits; `SDL_JOYSTICK_HIDAPI_PS5=1` (or `_PS4=1`) uses
SDL's own driver instead, which leaves a Bluetooth pad unreadable to those
apps until it reconnects.

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
| Debug menu | View | Create (Share) | ` |

Keys held with Cmd or Alt are not passed to the game, so the full-screen
shortcuts (Cmd+Enter, Alt+Enter) do not press Start.

## Enhancements

Each one is on by default unless the last column says otherwise; that column
says how to turn it off. The details, and every setting, are in
[docs/enhancements.md](docs/enhancements.md).

| Enhancement | What it does | Off |
| --- | --- | --- |
| [Display options](docs/enhancements.md#display-options) | Window, borderless or full screen, widescreen, resolution, fill or centered, and sharp or original text, in the OPTIONS menu | `CONKER_DISPLAY_OPTIONS=0` hides the rows |
| [Skip to the title menu](docs/enhancements.md#skip-to-the-title-menu) | L skips the boot logos, the intro and the title scene's opening | `CONKER_SKIP_INTRO=0` |
| [Skip any cutscene](docs/enhancements.md#skip-any-cutscene) | L skips every cutscene, the first time too | `CONKER_SKIP_CUTSCENES=0` |
| [SAVE and LOAD in the pause menu](docs/enhancements.md#save-and-load-in-the-pause-menu) | Save where Conker stands and load back to that spot, with a snapshot of every save | `CONKER_PAUSE_SAVE=0` |
| [Debug menu](docs/enhancements.md#debug-menu) | Wireframe, frame rate, position, enhancements on and off, teleport, snapshots and debug reports | `CONKER_DEBUG_MENU=0` |
| [Surround by default](docs/enhancements.md#surround-by-default) | A new save file's sound setting is surround instead of stereo | `CONKER_SURROUND_DEFAULT=0` |
| [Characters at full detail](docs/enhancements.md#characters-at-full-detail) | Conker and others keep their most detailed model at any distance | `CONKER_MAX_DETAIL=0` |
| [Characters seen from farther](docs/enhancements.md#characters-seen-from-farther) | Characters drawn as far as the scene, not faded out past 3000 units | `CONKER_CHARACTER_DISTANCE=0` |
| [No Rumble Pak banner](docs/enhancements.md#no-rumble-pak-banner-with-a-rumbling-pad) | No "INSERT RUMBLE PAK" banner when the pad can rumble | `CONKER_RUMBLE_BANNER=1` shows it |
| [Camera on the right stick](docs/enhancements.md#camera-on-the-right-stick) | C-left and C-right swapped, so left turns the view left | Always on; the debug menu can switch it |
| [Your pad's buttons](docs/enhancements.md#your-pads-buttons) | The game's N64 button pictures (menus, tutorial bubbles, B pads) drawn as an Xbox or PlayStation pad's, or redrawn sharper | `CONKER_PAD_ICONS=0`; the debug menu can switch it |
| [Sharper text](docs/enhancements.md#sharper-text) | The font enlarged with Super-xBR from your ROM | `CONKER_SHARP_FONT=0` |
| [Sharper menus](docs/enhancements.md#sharper-menus) | The menus' signs and headings, and the speech bubbles, enlarged the same way | `CONKER_SHARP_MENUS=0` |
| [Texture packs](docs/enhancements.md#texture-packs) | RT64 replacement packs from `conker-recomp-data/texture_packs` | `CONKER_TEXTURE_PACKS=0` |
| [60 fps (experimental)](docs/enhancements.md#60-fps-experimental) | Starts a frame on every retrace instead of every second one | Off by default; `CONKER_60FPS=1` turns it on |

## Documentation

* [docs/enhancements.md](docs/enhancements.md): each enhancement in full
  and its settings
* [docs/diagnostics.md](docs/diagnostics.md): crash reports, display-list and
  audio dumps, scripted input, skipping the opening with a save
* [docs/recompilation.md](docs/recompilation.md): how the recompilation is set
  up and what the submodule patches change
* [docs/memory-layout.md](docs/memory-layout.md): the TLB layout
* [docs/rsp-microcode.md](docs/rsp-microcode.md): graphics and audio microcode
* [docs/investigations.md](docs/investigations.md): open problems and what
  has been ruled out

## AI disclosure

This project is developed with the help of AI. Much of the code, the tools
and the documentation is written by an AI coding assistant. The direction
and the quality assurance are mine: I decide what is worked on and how it
should behave, test the changes in the game, and review them before they
are merged.

## License

The code written for this project (`src`, `tools`, `config` and the docs) is
under the MIT license; see [LICENSE](LICENSE). Everything else keeps its own
license:

* [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime):
  GPL-3.0. A built `ConkerRecomp` links it, so a binary that is passed on
  falls under the GPL-3.0 as a whole and has to come with its source.
* [N64Recomp](https://github.com/N64Recomp/N64Recomp): MIT.
* [RT64](https://github.com/rt64/rt64): MIT, with the libraries it bundles
  under their own licenses (`lib/rt64/src/contrib`).
* [SDL2](https://www.libsdl.org/): zlib.
* The patches in `tools/patches` change those projects, and each is under
  the license of the project it patches.
* The sharper text and menus use Hyllian's Super-xBR (MIT).

*Conker's Bad Fur Day* and its code, art and sound belong to Rare and
Microsoft. This project is not affiliated with or endorsed by them, contains
none of the game's data, and needs your own ROM.
