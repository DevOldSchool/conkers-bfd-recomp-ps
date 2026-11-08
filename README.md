# Conker's Bad Fur Day: Recompiled

This is my attempt at a faithful PC port of *Conker's Bad Fur Day*
(Nintendo 64, US) for Windows, macOS and Linux. The aim is the game as Rare
made it: the same look, sound and feel, running natively. On top of that
come a few small quality-of-life changes, such as widescreen, higher
resolutions, sharper text and menus and skippable cutscenes, listed under
[Enhancements](#enhancements). They stay out of the way of the original
style, and most can be switched off. MULTI can also be played online, across
macOS, Windows and Linux ([docs/online-play.md](docs/online-play.md)), and code mods can be added
([docs/modding.md](docs/modding.md)).

It is a static recompilation built with
[N64Recomp](https://github.com/N64Recomp/N64Recomp) and
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime), using the
function inventory from
[conkers-bfd-decomp](https://github.com/DevOldSchool/conkers-bfd-decomp).
Recompiled functions are replaced with decompiled C as the decomp matches them.

This repository contains no ROM data. You need your own US ROM
(SHA-1 `4cbadd3c4e0729dec46af64ad018050eada4f47a`).

## Status

The game is fully functional and has been played from start to finish: the
story, its chapters (replayed from the title's CHAPTERS page), saves, and
MULTI with up to four players on one machine or online. Graphics run Rare's F3DEX2CBFD microcode in RT64, audio runs the
`n_aspMain` microcode through RSPRecomp, and saves are the game's own EEPROM.
Problems found are tracked in the issues; a bug report with a debug report
attached (see the issue form) is the quickest to fix.

Platforms:

* macOS on Apple silicon
* Windows 10 (version 1803 or later) and 11, x64 and ARM64
* Linux: Ubuntu 24.04 or newer with a Vulkan driver

## Playing

Download the build for your system from the releases, or build it yourself
(below). No game data comes with it: the first time it starts, the game asks
for your own US ROM and keeps a copy in its data folder, with the saves,
settings, mods and logs:

* macOS: `~/Library/Application Support/ConkersBadFurDayRecompiled`
* Windows: `%LOCALAPPDATA%\ConkersBadFurDayRecompiled`
* Linux: `~/.local/share/ConkersBadFurDayRecompiled`

The builds are not signed by Apple or Microsoft, so the first start needs a
step more:

* macOS: open the app once, then in System Settings > Privacy & Security
  choose Open Anyway.
* Windows: before extracting, right-click the zip, choose Properties, tick
  Unblock and press OK. If SmartScreen still stops the game, choose More
  info, then Run anyway.
* Linux: run `install.sh` in the folder to add the game to your applications.

### Through Steam

The game can sit in your Steam library as a non-Steam game, with its own
artwork, which suits the Steam Deck and Big Picture. Quit Steam, then run the
script next to the game:

* macOS: `Add to Steam.command` beside the app (the first time, right-click it
  and choose Open, as for the app).
* Windows: `Add to Steam.bat`.
* Linux and the Steam Deck (in Desktop Mode): `./add-to-steam.sh` in the
  game's folder.

The next time Steam starts the game is in the library. Run the script again
after moving the game; `--remove` (`-Remove` on Windows) takes it out. It
changes only its own entry in Steam's list of non-Steam games and keeps a copy
of the list as it was (`shortcuts.vdf.bak`). From a checkout the scripts are
in `tools/steam`, and the artwork in `assets/steam`.

The debug menu (View, Create or Share on a pad, `` ` `` on the keyboard) holds
the settings that are not in the game's own OPTIONS, the MODS page (the
Speedrun timer comes built in, off until you turn it on) and SAVE DEBUG
REPORT for bug reports. To quit with a pad or in full screen,
press Start on the title menu and choose EXIT GAME. If the game crashes, the next start says so and points
at the log in the data folder's `crash-reports`.

## Building

Step-by-step guides, one-time setup included:

* [macOS](docs/building-macos.md)
* [Windows](docs/building-windows.md)
* [Linux](docs/building-linux.md)

Every build needs:

* CMake 3.20 or later, Ninja, Python 3 and SDL2
* a C++20 compiler (clang)
* the [decomp](https://github.com/DevOldSchool/conkers-bfd-decomp) checked
  out next to this repository, as `../conkers-bfd-decomp`. Only its scripts
  and inventory are used; it does not have to be built, so its Docker
  toolchain is not needed.
* your US ROM

The build has three stages: `tools/apply_patches.sh` patches the submodules,
`tools/regenerate.sh` turns the ROM into C with N64Recomp, and CMake compiles
that with the runtime and the renderer.

### Build options

* Cloned without `--recursive`? Run `git submodule update --init --recursive`
  before `tools/apply_patches.sh`.
* Leave out `-DCONKER_RENDERER=rt64` for the null build: no window, sound or
  input, useful for scripted runs.
* With the decomp or the ROM somewhere else, set
  `CONKER_DECOMP=/path/to/conkers-bfd-decomp` or `CONKER_ROM=/path/to/rom.z64`
  for `regenerate.sh` (from a git worktree, `CONKER_DECOMP` is needed).
* Run `tools/regenerate.sh` again after changing `config/us.toml`.
* `regenerate.sh` uses the committed `config/us.syms.toml`, so any decomp
  revision builds the same game. `CONKER_UPDATE_SYMS=1 tools/regenerate.sh`
  rewrites it from your decomp checkout instead.

## Controls

Up to four SDL game controllers are read, one per controller port in the
order they connect, so local multiplayer works: each player presses Start on
the MULTI join screen. Pads are hot-pluggable: one that disconnects frees its
port and the others keep theirs, and the next pad to connect takes the lowest
free port, so players follow the order pads are switched on. (On a PS5, a
signed-in user whose pad is off holds no port.) The keyboard plays controller 1 alongside its pad. The debug menu's
CONTROLS page maps each controller's pad buttons and the keyboard's keys
differently and inverts the aim (see
[docs/enhancements.md](docs/enhancements.md#debug-menu)); below are the
defaults. A pad that can rumble (DualSense, DualShock 4, Xbox, Switch
Pro) counts as a Rumble Pak in its port. Connect controller 1's pad before
starting the game, which looks for the pak at boot. Each pad has its own
haptics and adaptive triggers, felt for its own player in a multiplayer match;
the button pictures and the debug menu belong to controller 1. `CONKER_RUMBLE_TEST=1` buzzes the pad for a second when it connects.
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
| Start | Start | Options | Enter |
| C buttons | Right stick | Right stick | Mouse (the camera), I J K L |
| Camera zoom (C-up) | Right stick up | Right stick up | Mouse wheel, or R (in) and T (out) |
| D-pad | D-pad | D-pad | Arrow keys |
| Debug menu | View | Create (Share) | ` |

Keys held with Cmd or Alt are not passed to the game, so the full-screen
shortcuts (Cmd+Enter, Alt+Enter) do not press Start.

The mouse is captured when you click in the window while playing (that click
is not passed on): the cursor hides, moving the mouse turns the camera (in a
multiplayer match it turns the player with it, A and D stepping sideways), and
while aiming (R, or the slingshot) it aims, as the right stick does. Esc, pausing
(Enter), a menu or another window lets it go; it is captured again when play
carries on after a pause, or with a click. The wheel, or R and T, move the camera through its
three distances, in and out (on a pad, C-up only steps in).
MOUSE AIM on the CONTROLS page (OPTIONS, then CONTROLS) sets how fast the mouse
aims (25% to 400%).

## Enhancements

Each one is on by default unless the last column says otherwise; that column
says how to turn it off. The details, and every setting, are in
[docs/enhancements.md](docs/enhancements.md).

| Enhancement | What it does | Off |
| --- | --- | --- |
| [Display options](docs/enhancements.md#display-options) | Volume in the OPTIONS menu, and a GRAPHICS page there: window, borderless or full screen, widescreen, resolution, fill or centered, sharp or original text, and the frame rate | `CONKER_DISPLAY_OPTIONS=0` hides the rows |
| [Skip to the title menu](docs/enhancements.md#skip-to-the-title-menu) | L skips the boot logos, the intro and the title scene's opening | `CONKER_SKIP_INTRO=0` |
| [Skip any cutscene](docs/enhancements.md#skip-any-cutscene) | L skips every cutscene, the first time too | `CONKER_SKIP_CUTSCENES=0` |
| [SAVE and LOAD in the pause menu](docs/enhancements.md#save-and-load-in-the-pause-menu) | Save where Conker stands and load back to that spot, with a snapshot of every save | `CONKER_PAUSE_SAVE=0` |
| [EXIT GAME](docs/enhancements.md#exit-game) | Start on the title's corners opens a menu with EXIT GAME, to close the game with a pad alone | `CONKER_QUIT_GAME=0` |
| [Debug menu](docs/enhancements.md#debug-menu) | Wireframe, frame rate, position, enhancements on and off, teleport, snapshots and debug reports | `CONKER_DEBUG_MENU=0` |
| [Surround by default](docs/enhancements.md#surround-by-default) | A new save file's sound setting is surround instead of stereo | `CONKER_SURROUND_DEFAULT=0` |
| [Every multiplayer character](docs/enhancements.md#every-multiplayer-character) | MULTI has every character the cheat codes unlock (the Caveman, Neo Conker, Gregg and the rest), without the codes | `CONKER_MP_CHARACTERS=0` |
| [Characters at full detail](docs/enhancements.md#characters-at-full-detail) | Conker and others keep their most detailed model at any distance | `CONKER_MAX_DETAIL=0` |
| [Everything seen from farther](docs/enhancements.md#everything-seen-from-farther) | Characters, props, bees, grass and flowers drawn two and a half times as far before they are left out | `CONKER_DRAW_DISTANCE=0` |
| [Pad haptics](docs/enhancements.md#pad-haptics) | Taps and buzzes in the pad's grips (and an Xbox pad's triggers) for Conker's landings, falls and slingshot, and a feel for each weapon in hand, the mounted gun's rounds included | `CONKER_HAPTICS=0` |
| [Adaptive triggers](docs/enhancements.md#adaptive-triggers) | A DualSense's triggers push back harder the further Z sends Conker's pee, pull heavy at the mounted gun and resist for each gun in hand | `CONKER_ADAPTIVE_TRIGGERS=0` |
| [Consistent controls](docs/enhancements.md#consistent-controls) | The same controls across the game, as modern pads have them: the left stick moves, the right stick turns the view (left turns it left) or aims (the shotgun, the rocket launcher's scope), and up looks up | `CONKER_CONSISTENT_CONTROLS=0` |
| [Your pad's buttons](docs/enhancements.md#your-pads-buttons) | The game's N64 button pictures (menus, tutorial bubbles, B pads) drawn as an Xbox or PlayStation pad's, as keys with the keyboard and mouse alone, or redrawn sharper | `CONKER_PAD_ICONS=0`; the debug menu can switch it |
| [Sharper text](docs/enhancements.md#sharper-text) | The font enlarged with Super-xBR from your ROM | `CONKER_SHARP_FONT=0` |
| [Sharper menus](docs/enhancements.md#sharper-menus) | The menus' signs and headings, and the speech bubbles, enlarged the same way | `CONKER_SHARP_MENUS=0` |
| [Texture packs](docs/enhancements.md#texture-packs) | RT64 replacement packs from `conker-recomp-data/texture_packs` | `CONKER_TEXTURE_PACKS=0` |
| [60 fps](docs/enhancements.md#60-fps) | Starts a frame on every retrace instead of every second one | Off by default; FRAME RATE on the GRAPHICS page, or `CONKER_60FPS=1` |
| [Smooth motion](docs/enhancements.md#smooth-motion) | Pictures drawn between the game's frames, up to the display's refresh rate (120 Hz and more) | Off by default; FRAME RATE on the GRAPHICS page (60 FPS SMOOTH), or `CONKER_SMOOTH=1` |

## Documentation

* [docs/enhancements.md](docs/enhancements.md): each enhancement in full
  and its settings
* [docs/diagnostics.md](docs/diagnostics.md): crash reports, display-list and
  audio dumps, scripted input, skipping the opening with a save
* [docs/recompilation.md](docs/recompilation.md): how the recompilation is set
  up and what the submodule patches change
* [docs/modding.md](docs/modding.md): installing (only from people you trust) and making code mods
* [docs/memory-layout.md](docs/memory-layout.md): the TLB layout
* [docs/rsp-microcode.md](docs/rsp-microcode.md): graphics and audio microcode
* [docs/online-play.md](docs/online-play.md): playing MULTI online: hosting,
  joining, join codes, port forwarding and firewalls

## Contributing

Bug reports, fixes and enhancements are welcome: [CONTRIBUTING.md](CONTRIBUTING.md) says how the
project works and what fits. Report security problems privately ([SECURITY.md](SECURITY.md)).

## AI disclosure

This project is developed with the help of AI. Much of the code, the tools
and the documentation is written by an AI coding assistant. The direction
and the quality assurance are mine: I decide what is worked on and how it
should behave, test the changes in the game, and review them before they
are merged.

The Steam artwork (`assets/steam`) and `assets/background.webp` were made
with AI image generation for this project. They are new pictures, not taken
from the game.

## License

The code written for this project (`src`, `tools`, `config` and the docs) is
under the MIT license; see [LICENSE](LICENSE). Everything else keeps its own
license:

* [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime):
  GPL-3.0. A built `ConkersBadFurDayRecompiled` links it, so a binary that is passed on
  falls under the GPL-3.0 as a whole and has to come with its source.
* [N64Recomp](https://github.com/N64Recomp/N64Recomp): MIT.
* [RT64](https://github.com/rt64/rt64): MIT, with the libraries it bundles
  under their own licenses (`lib/rt64/src/contrib`).
* [SDL2](https://www.libsdl.org/): zlib.
* [miniupnpc](https://github.com/miniupnp/miniupnp) and
  [libnatpmp](https://github.com/miniupnp/libnatpmp), which open an online
  host's port on the router: BSD-3-Clause. The build fetches them at pinned
  versions.
* The patches in `tools/patches` change those projects, and each is under
  the license of the project it patches.
* The sharper text and menus use Hyllian's Super-xBR (MIT).

Every packaged build carries these licenses, with the notices of the libraries
compiled into it and a SOURCE.txt naming the exact commit it was built from
(`tools/package_notices.py`; in the macOS app, under `Contents/Resources/Licenses`).
Each release attaches the complete source, submodules included, made by
`tools/source_archive.sh`.

*Conker's Bad Fur Day* and its code, art and sound belong to Rare and
Microsoft. This project is not affiliated with or endorsed by them, contains
none of the game's data, and needs your own ROM.
