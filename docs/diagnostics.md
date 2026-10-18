# Diagnostics

On a crash the binary prints the faulting N64 address, the MIPS registers,
the last 64 recompiled function entries on that thread and the image slide;
`tools/symbolicate.sh build/host/ConkerRecomp <log>` maps the host backtrace
to generated C lines and N64 instruction addresses.

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
* `CONKER_SKIP_INTRO=1` skips the boot logos, the intro and the title
  scene's opening by itself, behind a black screen, so the GAME 1 menu is up
  around VI 65 instead of 3,670. VI numbers in input scripts written for a full
  intro (such as the ones below) are then about 3,600 too late.
* `CONKER_PRESS_START=<vi>[,<vi>...]` holds Start on controller 1 for 10 VIs
  from each listed VI count, on top of any real input.
* `CONKER_VIRTUAL_PAD=<file>` (RT64 build) attaches an SDL virtual game
  controller driven by a script, so the whole controller path can be tested
  without hardware. Each line is `<vi> <length in VIs> <control>...`, where a
  control is an SDL button name (`a`, `b`, `x`, `start`, `leftshoulder`,
  `dpup`, ...) or `<axis>=<value>` (`leftx`, `lefty`, `rightx`, `righty`,
  `lefttrigger`, `righttrigger`, from -1 to 1, with +y down as in SDL). `#`
  starts a comment. For example, `5400 10 start` then `7500 10 a` goes from the
  title scene to a new game. Real controllers are ignored while it is set:
  SDL passes pad input to every process, so a pad in use in another window
  would otherwise steer the scripted run.
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
  instead of starting in full screen, which renders faster and gives smaller
  window captures. The log shows the window size chosen and, in
  `RT64 renders at <n>x`, the scale RT64 picked (4x for the 960x720 window,
  then shrunk to fit).
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
* `CONKER_AUDIO=0` (RT64 build) disables the SDL device; `CONKER_AUDIO_LATENCY_MS`
  (default 60) sets the extra buffering kept on top of what the game asks for;
  `CONKER_SILENT_AUDIO=1` skips the audio microcode entirely.
