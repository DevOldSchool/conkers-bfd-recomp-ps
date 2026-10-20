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
(cd lib/rt64 && git apply ../../tools/patches/rt64-edge-rects.patch)
(cd lib/rt64 && git apply ../../tools/patches/rt64-memory-packs.patch)
(cd lib/rt64 && git apply ../../tools/patches/rt64-wireframe.patch)
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

* **Display options:** the OPTIONS page of the title menu has
  four rows under the sound setting.
  * WINDOWED, BORDERLESS or FULL SCREEN. Full screen (the default) is macOS
    full screen in a Space of its own; borderless covers the screen in the
    current Space, with the menu bar and Dock hidden while the game is in
    front.
  * 4:3 or WIDESCREEN. Widescreen widens the view to the shape of the
    window or screen instead of stretching the picture: the window becomes
    16:9 (384x216 for each multiple), and covering the screen the picture
    takes the display's own shape (16:9, 16:10 or ultrawide). The HUD and
    menus stay in the middle, and the circle wipe that opens and closes a
    scene covers the whole width. Covering the screen is widescreen by
    default; a window is 4:3 by default, as on an N64. The game
    still decides what to draw for its own 4:3 view, so objects at the far
    sides can appear late. In the intro, Conker is not drawn while he
    waits for his cue just past the edge of the 4:3 view. Behind the pause menu the paused scene stays
    widescreen, but sharp: the game blurs only the 4:3 middle of the frame,
    so that blur is skipped in widescreen. Switching between the window and
    full screen while paused keeps the paused picture as it was, fitted to
    the new size; the new resolution and shape apply once you carry on.
  * The resolution: how large the game's 292x216 picture is rendered, in
    whole multiples from 292x216 (1x). In a window it is the window's size,
    up to the largest that fits the display (1460x1080, 5x, on a 2560x1440
    display with the Dock showing, which is also the default). Covering the
    screen it goes up to the first multiple that covers the screen's height
    (2044x1512, 7x, on 1440 lines, the default). Widescreen widths are
    shown in widescreen. Sizes are in pixels, also on a Retina screen,
    where the game draws at the screen's full pixel density: on a 3024x1964
    screen a window goes up to 2044x1512 (7x, half that in points),
    and covering the screen up to 2920x2160 (10x).
  * FILL SCREEN or CENTERED: the picture is scaled to fill the window or
    screen, or drawn at its own size in the middle with black bars, so that
    every game pixel is an even block. Covering the screen it fills by
    default; a lower resolution then fills it with larger pixels. A window
    is centered by default, which only shows once it is resized by hand.

  The window and the screen-covering modes each keep their own resolution,
  shape and fill. F11, Cmd+Enter, Alt+Enter or
  Ctrl+Cmd+F switch between the window and the last screen-covering mode.
  The choices are kept in `conker-recomp-data/display.cfg`, not in the
  game's save, so the OPTIONS saved to EEPROM are unchanged.
  `CONKER_DISPLAY=windowed`, `borderless` or `fullscreen` overrides the mode
  for one run, `CONKER_DISPLAY_OPTIONS=0` hides the rows, and
  `CONKER_ORIGINAL_SIZE=1` opens the original 960x720 window (in points)
  instead.
* **Skip to the title menu:** Start at any point during the boot logos or the
  intro fades to black and then into the GAME 1 menu, skipping the title
  scene's opening (the clock and the Cock and Plucker) too. On an N64, Start only skips the intro after about five seconds and after
  the reset button, and the title scene needs another press.
  `CONKER_SKIP_INTRO=1` skips without a press; `CONKER_SKIP_INTRO=0` keeps the
  original behaviour, where the intro cannot be skipped after a cold boot.
* **Skip any cutscene:** on an N64, L only skips a cutscene already
  watched, and some, such as the whole opening of a new game, not at all.
  Here L or Start skips every cutscene, the first time too. Start skips
  instead of pausing while a cutscene can be skipped, and pauses as usual
  everywhere else. Skipping any part of the opening goes on to where
  Conker wakes up, its own skip target in the game's data (about 45
  seconds into a new game instead of 7 minutes).
  `CONKER_SKIP_CUTSCENES=0` keeps the original rules.
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
  Every save made this way is also kept as its own snapshot in
  `saves/snapshots/`, named after the scene number and when it was made
  (`scene12-20261020-143205.bin` with a matching `.resume`). LOAD still
  starts the latest save; to go back to an earlier one, copy its two files
  over `conker_us.bin` and `conker_us.resume` while the game is closed, or
  load it from the debug menu.
  `CONKER_SAVE_SNAPSHOTS=0` stops keeping them.
* **Debug menu:** View on an Xbox pad, Create (Share) on a PlayStation pad, or
  the ` key opens it during play. It is the game's pause with its own page in
  place of the lever, PAUSED and the HUD, so the game stops as it does for the
  pause menu, and it closes with B, Start or the same button. Up and down choose a row, A or left and
  right change it:
  * WIREFRAME: the game's 3D triangles drawn as their edges on black; text,
    sprites and the HUD stay solid. Metal only for now.
  * SHOW FPS: the frames the game draws per second, in the top left corner.
  * SHOW POSITION: the scene, Conker's X, Y and Z and his facing, and the
    camera's yaw and pitch, under the frame rate during play. Angles are in
    degrees, and facing and yaw share their zero (+Z) and direction (towards
    +X), so walking away from the camera they agree. The first page shows them
    too, and opening the menu prints them in the terminal, with the camera's
    position, the point it looks at and its forward vector.
  * TELEPORT: X, Y, Z, FACING and CAMERA YAW, set to where Conker is and
    changed with left and right by STEP (1, 10, 100 or 1000; held, they keep
    changing). GO puts him there facing that way, and the camera where it
    looks along CAMERA YAW at him, as far away as it was: the same view as the
    numbers on screen, to the hundredth of a degree, for repeating what someone
    saw. The camera's pitch is the game's own for that spot (it keeps the
    camera at the height it wants), so it is shown and recorded but not set.
    HERE goes back to where he is, REMEMBER keeps the spot in
    `conker-recomp-data/debug_spots.txt` (one line each: `scene x y z angle
    facing camera-yaw camera-pitch`) and shows what it kept, and SPOTS lists
    those of the scene being played with their facing, yaw and pitch; A on one
    puts him there. Teleports stay within the scene.
  * LOAD SNAPSHOT: the snapshots in `saves/snapshots/`, newest first. A makes
    the chosen one the save, as copying its files over `conker_us.bin` and
    `conker_us.resume` would, and starts it where it was saved, as LOAD does.
    Progress since the last snapshot is not kept.

  It opens wherever Start would open the pause menu. The settings last until
  the game is closed. `CONKER_DEBUG_MENU=0` turns it off.
* **Surround by default:** with no saved options (a new save file), the
  OPTIONS menu's sound setting is surround instead of stereo. The
  game keeps its options in EEPROM next to the saved games, so a sound
  setting chosen and saved from that menu stays as it was.
  `CONKER_SURROUND_DEFAULT=0` keeps the stereo default.
* **Characters at full detail:** the game swaps Conker for a lower-detail
  model once the camera is more than 500 units away, which in play is often
  its usual distance (the blockier tail is the easiest place to see it), and
  for cruder ones further out. He, and the few other characters with more
  than one model, keep their most detailed model at any distance.
  `CONKER_MAX_DETAIL=0` keeps the game's choice.
* **No Rumble Pak banner with a rumbling pad:** the title menu's "INSERT
  RUMBLE PAK" banner shows on an N64 even with the pak inserted. It is left
  out when controller 1's pad can rumble; `CONKER_RUMBLE_BANNER=1` shows it.
* **Camera on the right stick:** C-left and C-right are swapped (also on J/L),
  so pushing left turns the view left.
* **Sharper text:** the game's text font is enlarged 8 times with
  Super-xBR, a pixel-art upscaler, from the glyphs in your ROM when the game
  starts: the same letters, smoother and less blurred than the N64's
  filtering. The game draws each glyph a little short of its edges, which its
  blur hides, so those rectangles are widened over each glyph's empty border
  and the sharper letters are not cut off (`src/platform/font_rects.cpp`).
  `CONKER_SHARP_FONT=0` keeps the original font.
* **Texture packs:** RT64 replacement packs (a folder, `.rtz` or `.zip` with
  an `rt64.json`) in `conker-recomp-data/texture_packs` are loaded at start,
  in name order; where two replace the same texture, the later name wins,
  and any of them wins over the sharper text. `CONKER_TEXTURE_PACKS=0` loads
  none. `CONKER_DUMP_SHARP_FONT=<dir>` writes the sharper text out as a pack
  to edit; `tools/font_pack.py` writes the same pack outside the game, or
  with `--style traced` a version redrawn with crisp edges and a thin dark
  outline (`tools/font_trace.py`).

## Documentation

* [docs/diagnostics.md](docs/diagnostics.md): crash reports, display-list and
  audio dumps, scripted input, skipping the opening with a save
* [docs/recompilation.md](docs/recompilation.md): how the recompilation is set
  up and what the submodule patches change
* [docs/memory-layout.md](docs/memory-layout.md): the TLB layout
* [docs/rsp-microcode.md](docs/rsp-microcode.md): graphics and audio microcode
* [docs/investigations.md](docs/investigations.md): open problems and what
  has been ruled out
