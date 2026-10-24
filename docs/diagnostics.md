# Diagnostics

On a crash the binary prints the faulting N64 address, the MIPS registers,
the last 64 recompiled function entries on that thread and the image slide;
`tools/symbolicate.sh build/host/ConkerRecomp <log>` maps the host backtrace
to generated C lines and N64 instruction addresses.

The game's heap is checked once a frame, just before the game's own walk of it
(`func_80004250`, where a damaged heap crashes). The first damage found, such as a
block's link overwritten by a write past the end of the block before it, is
printed as `[conker] heap damaged ...` with the blocks around it and the bytes
either side, and RDRAM is written to `conker-recomp-data/heap-<time>.rdram`
(big-endian, for `tools/cbfd_dl.py` or offline decoding).
`CONKER_HEAP_CHECK=0` turns the check off.

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
* `CONKER_INSPECT=1` starts with the debug menu's INSPECT on, and
  `CONKER_INSPECT_CURSOR=<x>,<y>[,<layer>][;...]` (fractions of the window,
  from the top left, and how many wheel steps behind the front) holds its
  cursor there and picks what is under it every 60 frames, moving to the next
  spot after each, so a scripted run prints what is at those spots
  (`[conker] inspect: picked ...`).
* `CONKER_SKIP_INTRO=1` skips the boot logos, the intro and the title
  scene's opening by itself, behind a black screen, so the GAME 1 menu is up
  around VI 65 instead of 3,670. VI numbers in input scripts written for a full
  intro (such as the ones below) are then about 3,600 too late.
* `CONKER_PRESS_START=<vi>[,<vi>...]` holds Start on controller 1 for 10 VIs
  from each listed VI count, on top of any real input. `CONKER_PRESS_A` does
  the same for A, which the null build needs to get into a new game:
  `CONKER_SKIP_INTRO=1 CONKER_PRESS_A=300,600,900,1200,1500` with no save
  reaches the opening cutscene, whose first speech bubbles are up about
  165 s in.
* `CONKER_VIRTUAL_PAD=<file>` (RT64 build) attaches an SDL virtual game
  controller driven by a script, so the whole controller path can be tested
  without hardware. Each line is `<vi> <length in VIs> <control>...`, where a
  control is an SDL button name (`a`, `b`, `x`, `start`, `leftshoulder`,
  `dpup`, ...) or `<axis>=<value>` (`leftx`, `lefty`, `rightx`, `righty`
  from -1 to 1, with +y down as in SDL; `lefttrigger`, `righttrigger` from 0,
  released, to 1). Before this, the triggers rested half pressed, which held Z. `#`
  starts a comment. For example, `5400 10 start` then `7500 10 a` goes from the
  title scene to a new game. Real controllers are ignored while it is set:
  SDL passes pad input to every process, so a pad in use in another window
  would otherwise steer the scripted run. The virtual pad can rumble, so it
  counts as a Rumble Pak, and it logs each rumble change. It logs each haptic
  effect too, or with `CONKER_HAPTICS_MOTORS=1` the motor strengths it plays
  as (`rumble low <x> high <y>`, `trigger rumble left <x> right <y>`), as on
  a pad without Core Haptics.
* **Rumble.** The log shows the pak probe at boot (`rumble pak inserted` or
  `not inserted`) and every change of the motor the game asks for
  (`VI <n>: rumble on/off, port 1`); a rumble is a run of on/off lines, since
  the game pulses the motor to set its strength. `CONKER_RUMBLE_TEST=1` (RT64
  build) buzzes the pad for a second when it connects, to check the pad
  without the game.
* **Skipping the opening.** Saves persist in
  `conker-recomp-data/saves/conker_us.bin` under the working directory, so
  keep a copy of that file from a run that got past the first save (about
  7 minutes in) and put it back before later runs. With a save present,
  `5400 10 start` opens the GAME 1 page and `6600 10 a` picks PLAY, which
  loads the first area about 2 minutes after boot. Leave the stick alone on
  that page: pushing it moves the lever to ERASE, and a second A erases the
  save. An emulator-style save state is not possible here, because each N64
  thread runs as a native host thread with its own C stack.
* `CONKER_ORIGINAL_SIZE=1` (RT64 build) opens the original 960x720 window
  (in points: 1920x1440 pixels on a Retina screen) instead of the display mode
  chosen in OPTIONS, which gives smaller window captures. Changing the mode in
  OPTIONS during such a run still applies and saves it. The log shows the
  window size chosen and, in `RT64 renders at <n>x for a <w>x<h> window`, the
  window's resolution from OPTIONS, shrunk to fit, and the swap chain's size
  in pixels. The `display` line gives the display's size in pixels and its
  pixels per point (2 on a Retina screen); a full-screen Space on a MacBook
  with a notch is below it (3024x1890 on a 3024x1964 screen), a
  borderless window covers it (3024x1964).
* **Window captures on a Retina screen** (`screencapture -o -l <window id>`)
  are in pixels, with a 56-pixel title bar on top of a window's picture.
* **Reaching OPTIONS by script.** With `CONKER_SKIP_INTRO=1`, the pad lines
  `400 8 leftx=1`, `640 8 leftx=1` and `900 8 a` go from GAME 1 past MULTI to
  OPTIONS and open it; `1300 8 lefty=1` then selects the display mode row
  (each further `lefty=1` 4:3/WIDESCREEN, the resolution row, then FILL
  SCREEN/CENTERED), and
  `leftx=1` or `leftx=-1` steps its value. `CONKER_ORIGINAL_SIZE=1` keeps the window at 960x720, so test
  window sizes without it. `rightx` is the right stick
  (the C buttons), not the left stick pushed right. When several copies of
  the game are open, capture a window by its process ID: the first window
  named "Conker" may belong to another run.
* `CONKER_DISPLAY_SWITCH=<ms>:<mode>[,<ms>:<mode>...]` (RT64 build) asks for
  a display mode (`windowed`, `borderless` or `fullscreen`) that many
  milliseconds after start, as the full-screen shortcuts would, so mode
  changes can be tested by script, for example while the pause menu is open.
* `CONKER_DUMP_AUDIO=<file.wav>` records every buffer the game queues
  (16-bit stereo at the game's 22,020 Hz) in either build. The log prints a
  line every 5 s of audio with the peak, RMS, minimum output-queue depth and
  underrun count. Both builds always print those lines. If the output has been
  exactly zero for 10 s after there was sound, they also print one
  `audio: silent for 10 s` line with the music manager's view of the three
  sequence players: player state (0 stopped, 1 playing, 2 stopping, 3
  paused), the sequence, its volume and pause mode, and the music-paused flag
  that the pause menu sets. An open pause menu is silent by design and shows
  flag 1 with the players paused.
* `CONKER_FRAME_LOG=<file>` writes a line for every frame the game queues:
  the VI, the retraces the frame steps by (`D_800BE9E4`), the scene, the logic
  delta (`D_800BE9A4`), Conker's position and the camera's eye. It works in
  either build and at either frame rate (`CONKER_60FPS`), for comparing the
  two over the same pad script.
* `CONKER_SOUND_LOG=1` prints a line for each sound effect as it starts
  (`sndp_play_sound`): the VI, the sound, its volume and Conker's animation,
  and a line for each change of Conker's animation with his health, for
  finding the sounds and moments to give haptic effects
  (src/enhancements/haptics/haptics.cpp).
* `CONKER_DUMP_TEXTURES=<dir>` (RT64 build) writes each texture the game
  loads to `<dir>` once, as RT64's `<hash>.v5.png`, `.tmem` and `.tile.json`
  files, the starting point for a texture pack. `tools/font_pack.py --dump
  <dir>` checks that every dumped font glyph has the hash the script computes
  for it from the ROM.
* `CONKER_DUMP_SHARP_FONT=<dir>` (RT64 build) writes the built-in sharper
  text, made from the ROM at the first graphics task, to `<dir>` as an RT64
  pack (`rt64.json` and `font/glyph_NN.png`), to edit and load from
  `conker-recomp-data/texture_packs`. `CONKER_DUMP_SHARP_MENUS=<dir>` does the
  same for the built-in sharper menus (`menus/<name>_N.png`, one per tile).
* `CONKER_AUDIO=0` (RT64 build) disables the SDL device; `CONKER_AUDIO_LATENCY_MS`
  (default 60) sets the extra buffering kept on top of what the game asks for;
  `CONKER_SILENT_AUDIO=1` skips the audio microcode entirely.

## GPU faults (black screen, game still running)

On macOS a command buffer that faults on the GPU (it read or wrote outside any
resource) is thrown away, so its frame is lost and the picture stutters. After
about six of them macOS refuses all of the process's GPU work for good: the
window goes black while the game, its sound and its menus carry on, and only a
restart brings the picture back. The system log shows it
(`/usr/bin/log show --predicate 'processID == <pid>'`: "Caused GPU Address
Fault Error", then "Ignored (for causing prior/excessive GPU errors)").

The RT64 build logs each failed command buffer itself
(`src/platform/gpu_faults.cpp`):

```
[conker] GPU fault 1 at VI 2982 (56.0 s, 19:10:43), scene 12: Workload Graphics queue, code 3, faulted encoders: Graphics Render Encoder x1: Caused GPU Address Fault Error ...
[conker]   renderer changes before it (since the last fault):
[conker]     VI 38 (1.0 s, 19:09:48): RT64 scale 3x: frame buffers discarded
[conker] GPU: macOS is refusing the game's GPU work after 6 GPU fault(s) ...
[conker] GPU fault: debug report scene12-20261022-191043
```

The queue names the RT64 worker that sent the work (Workload Graphics draws
the game's frames, Present Graphics shows them, Texture Direct, Texture Copy
and RT64 Stream Worker upload textures, Framebuffer Graphics handles frame
buffers the CPU wrote). The renderer changes are the things that free or remake
GPU resources: texture packs swapped, a new scale or shape, frame buffers
discarded, the pause menu's frame copy holding the settings. The "refusing"
line repeats every 30 s while it lasts. The first fault also makes a debug
report, as SAVE DEBUG REPORT does, with these lines in `report.json` under
`gpu_faults`. The code is Metal's `MTLCommandBufferError`: 3 a page fault, 2 a timeout, 4 a refused submission.

To find the shader behind a fault, run with Metal's shader validation:
`MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_RESOURCE_USAGE=0
MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1`. Each bad access is then printed as
"Invalid device load" with the shader function, the buffer's length, the
offset and the draw. Without `RESOURCE_USAGE=0` validation tracks every resident
resource on each submit, and RT64 keeps so many that no frame reaches the
screen. `MTL_SHADER_VALIDATION_DUMP_PIPELINES=1` adds each pipeline's UID to the
system log. The faults found this way so far were RT64's LOD tile choice
(`tools/patches/rt64-metal-float-to-int.patch`, see `docs/recompilation.md`).
A freeze that clears up by itself is the main thread stuck: the window stops
answering, and the game waits on it. After 3 s the log says `main thread stuck`
and, on macOS, `sample` writes every thread's stack to
`conker-recomp-data/debug_reports/hang-<date>-<time>.txt`; `main thread running
again after N s` follows (src/platform/hang_watch.cpp). Before this, such
freezes showed only in the system log, as spindump's "slow hid response (N)":
`/usr/bin/log show --predicate 'process == "spindump" AND eventMessage CONTAINS
"[<pid>]"'`. The ones seen came while switching between a window and full
screen with RT64 changing its multiple at the same moment; RT64 now keeps its
multiple until the window has settled in the new mode.

A debug report that shows the fault can be replayed by script for this: with
`CONKER_SKIP_INTRO=1` and the report alone in the run's `debug_reports`, the pad
lines `600 8 back`, eight `dpdown`s and two `a`s load it from the title.

## Emulator ground truth

`tools/emulator_frames.py` takes screenshots from the decomp's
software-rendered Mupen64Plus (Angrylion RDP, CXD4 RSP, the
`./conker mupen-trace --software-renderer` container), with controller
presses injected through its debugger, so a scene can be compared with RT64.
Its docstring has a new-game script that reaches the opening's speech bubbles.
The emulator runs at about 20 VIs a second, so that run takes about 18
minutes. Compare pictures in N64 pixels: the emulator's 640x480 screenshot
shows the 285x214 picture at x 40-609, y 25-452; an RT64 capture shows it
across the whole window.

The container follows the host CPU, linux/arm64 or linux/amd64, as the
decomp's `./conker` does, and `CONKER_PLATFORM` picks one. Each platform's
screenshots repeat exactly from run to run; the two platforms differ by at
most one level per colour channel, so compare frames from the same one when
counting pixels. On Windows, run the tool from WSL, where the decomp's
`./conker` also has to run to build the image.

Checked this way: speech-bubble shadows. Each bubble is drawn twice from the
same 64x64 IA8 texture, first black at alpha 0x40 about 5 pixels right of and
4 pixels above the bubble, then white on top. RT64 matches the emulator:
the shadow shows only as a thin crescent along the top-right edge, darkening
what is under it to 0.75. The soft fade around the rest of a bubble is
its own edge under bilinear filtering (the texture is stretched about 3x
across), not the shadow.
