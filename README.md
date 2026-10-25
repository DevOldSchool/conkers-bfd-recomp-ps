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
tools/apply_patches.sh
tools/regenerate.sh
cmake -S . -B build/rt64 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCONKER_RENDERER=rt64
cmake --build build/rt64
build/rt64/ConkerRecomp /path/to/conker-us.z64
```

Leave out `-DCONKER_RENDERER=rt64` for the null build: no window, sound or
input, useful for scripted runs. From a git worktree, point `regenerate.sh` at
the decomp with `CONKER_DECOMP=/path/to/conkers-bfd-decomp`.

## Controls

Up to four SDL game controllers are read, one per controller port in the
order they connect, so local multiplayer works: each player presses Start on
the MULTI join screen. Pads are hot-pluggable: one that disconnects frees its
port and the others keep theirs, and the next pad to connect takes the lowest
free port. The keyboard plays controller 1 alongside its pad. There is no
remapping yet. A pad that can rumble (DualSense, DualShock 4, Xbox, Switch
Pro) counts as a Rumble Pak in its port. Connect controller 1's pad before
starting the game, which looks for the pak at boot. Haptics, adaptive
triggers, the button pictures and the debug menu belong to controller 1. `CONKER_RUMBLE_TEST=1` buzzes the pad for a second when it connects.
On macOS, DualSense and DualShock 4 pads are read through Apple's
GameController framework, so they keep working in other apps (OpenEmu, for
one) after the game quits; `SDL_JOYSTICK_HIDAPI_PS5=1` (or `_PS4=1`) uses
SDL's own driver instead, which leaves a Bluetooth pad unreadable to those
apps until it reconnects.

| N64 | Xbox | PlayStation | Keyboard and mouse |
| --- | --- | --- | --- |
| Analog stick | Left stick | Left stick | W A S D (Shift held walks) |
| A | A | Cross | Space |
| B | B or X | Square or Circle | E |
| Z | LT or RT | L2 or R2 | Left click (fire), Left Ctrl or C (crouch) |
| L | LB | L1 | Q or Tab |
| R | RB | R1 | Right click (aim), F |
| Start | Start | Options | Esc or Enter |
| C buttons | Right stick | Right stick | Mouse (the camera), I J K L |
| Camera zoom (C-up) | Right stick up | Right stick up | Mouse wheel, or R (in) and T (out) |
| D-pad | D-pad | D-pad | Arrow keys |
| Debug menu | View | Create (Share) | ` |

Keys held with Cmd or Alt are not passed to the game, so the full-screen
shortcuts (Cmd+Enter, Alt+Enter) do not press Start.

The mouse is captured when you click in the window while playing (that click
is not passed on): the cursor hides, moving the mouse turns the camera, and
while aiming (R) it aims, as the right stick does. Pausing (Esc), a menu or
another window lets it go; it is captured again when play carries on after a
pause, or with a click. The wheel, or R and T, move the camera through its
three distances, in and out (on a pad, C-up only steps in).
`CONKER_MOUSE_SENSITIVITY` scales the mouse (1 by default, 2 twice as fast).

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
| [Everything seen from farther](docs/enhancements.md#everything-seen-from-farther) | Characters, props, bees, grass and flowers drawn two and a half times as far before they are left out | `CONKER_DRAW_DISTANCE=0` |
| [No Rumble Pak banner](docs/enhancements.md#no-rumble-pak-banner-with-a-rumbling-pad) | No "INSERT RUMBLE PAK" banner when the pad can rumble | `CONKER_RUMBLE_BANNER=1` shows it |
| [Pad haptics](docs/enhancements.md#pad-haptics) | Taps and buzzes in the pad's grips (and an Xbox pad's triggers) for some of Conker's sounds (slingshot, landing) | `CONKER_HAPTICS=0` |
| [Peeing and the machine gun in the triggers](docs/enhancements.md#peeing-and-the-machine-gun-in-the-triggers) | A DualSense's triggers push back harder the further Z sends Conker's pee; at the mounted gun they pull heavy and every round thumps in both grips | `CONKER_ADAPTIVE_TRIGGERS=0` |
| [Consistent controls](docs/enhancements.md#consistent-controls) | The same controls across the game, as modern pads have them: the left stick moves, the right stick turns the view (left turns it left) or aims (the shotgun, the rocket launcher's scope), and up looks up | `CONKER_CONSISTENT_CONTROLS=0` |
| [Your pad's buttons](docs/enhancements.md#your-pads-buttons) | The game's N64 button pictures (menus, tutorial bubbles, B pads) drawn as an Xbox or PlayStation pad's, as keys with the keyboard and mouse alone, or redrawn sharper | `CONKER_PAD_ICONS=0`; the debug menu can switch it |
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
