# Diagnostics

On a crash the binary prints the faulting N64 address, the MIPS registers,
the last 64 recompiled function entries on that thread and the image slide;
`tools/symbolicate.sh build/host/ConkersBadFurDayRecompiled <log>` maps the host backtrace
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
* `CONKER_VIRTUAL_PAD=<file>[,<file>...]` (RT64 build) attaches an SDL virtual
  game controller per file, each driven by its script, so the whole controller
  path can be tested without hardware. They connect in the order given, so the
  first file is controller 1, the second controller 2, and so on. Each line is `<vi> <length in VIs> <control>...`, where a
  control is an SDL button name (`a`, `b`, `x`, `start`, `leftshoulder`,
  `dpup`, ...) or `<axis>=<value>` (`leftx`, `lefty`, `rightx`, `righty`
  from -1 to 1, with +y down as in SDL; `lefttrigger`, `righttrigger` from 0,
  released, to 1). Before this, the triggers rested half pressed, which held Z. `#`
  starts a comment. For example, `5400 10 start` then `7500 10 a` goes from the
  title scene to a new game. `unplug <vi>` and `plug <vi>` lines disconnect
  and reconnect the pad; a pad whose first such line is `plug` starts
  unplugged. The first script can play controller 1's mouse too:
  `mousex=<pixels>` and `mousey=<pixels>` move it that much every VI,
  `wheel=<steps>` turns the wheel once (+ away from you, zooming in), and
  `lmb` and `rmb` hold its buttons; the scripted mouse counts as captured
  whenever the game is being played. `key=<name>` holds a key as the keyboard
  would for controller 1 (SDL's key names: `key=Return`, `key=Escape`). Real controllers are ignored while it is set:
  SDL passes pad input to every process, so a pad in use in another window
  would otherwise steer the scripted run. A virtual pad can rumble, so it
  counts as a Rumble Pak, and it logs each rumble change. It logs each haptic
  effect too, or with `CONKER_HAPTICS_MOTORS=1` the motor strengths it plays
  as (`rumble low <x> high <y>`, `trigger rumble left <x> right <y>`), as on
  a pad without Core Haptics. `CONKER_VIRTUAL_PAD_ID=<vendor>:<product>` (hex,
  `054c:0ce6` for a DualSense) and `CONKER_VIRTUAL_PAD_NAME=<text>` make the
  pads pose as another make with a name of their own, to check that the lobby
  sends only the make and model (`tools/net-test/lrun.sh pn` with them in
  HENV and GENV: each view lists the other machine's pad by model and its own
  by name).
* **Rumble.** The log shows the pak probe for each connected controller
  (`controller <n> rumble pak inserted` or `not inserted`) and every change of
  the motor the game asks for (`VI <n>: rumble on/off, port <n>`); a rumble is a run of on/off lines, since
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
  OPTIONS and open it on the VOLUME row (under the sound row); `1300 8 lefty=1`
  then selects SHARP TEXT (each further `lefty=1` the display mode row,
  4:3/WIDESCREEN, the resolution row, then FILL SCREEN/CENTERED), and
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
* `CONKER_FRAME_INPUT_LOG=<file>` writes the input record of every game frame,
  the one place the game logic reads its input from
  (src/runtime/frame_input.cpp, M0 of docs/online-multiplayer-plan.md): the
  frame number, the VI, then per controller port `flags:buttons,stick x,stick
  y,right x,right y` (flags 1 connected, 2 has a right stick, 4 Rumble Pak;
  sticks as the game reads them, the right stick in steps of 1/127) and
  each port's mouse as `m<captured>:dx,dy,zoom` (four of them; on one machine
  only port 1's moves, online each machine's mouse plays its own port; logs
  from before have port 1's alone, and replay as before). Its lines match
  `CONKER_FRAME_LOG`'s one for one. Online every peer's log holds the same
  records from frame 1, the host's merged ones.
* `CONKER_MY_VIEW_PORT=<1-4>` tries the online "my view"
  (src/enhancements/my_view) outside a session, in a local split-screen
  match, as that port's player: only its view is drawn, over the whole
  window, and the sounds are worked out for its listener.
  `CONKER_MY_VIEW_FULL=0` presents the local player's split-screen view cut
  out and scaled up instead of drawing it again. `CONKER_MY_VIEW_LOG=1` prints the view
  presented and every sound started with the game's and the local volume and
  pan. `CONKER_MY_VIEW=0` switches it off, online too.
* `CONKER_NET_SIM=latency=<ms>,jitter=<ms>,loss=<%>,dup=<%>,reorder=<%>,outage=<from>-<to>`
  simulates a bad link for one online peer (src/net/net_play.cpp): half
  the round-trip latency on each of its sends and receives plus up to
  `jitter`, then loss, duplicates and reordering (by 40 ms) on its sends,
  and no traffic either way from `from` to `to` seconds after the session
  starts. Every 900 frames each peer prints its fps, its waits and stalls of
  50 ms or more and the round trip. `CONKER_NET_TEST_DESYNC=<frame>` changes
  a byte of game memory then (the host drops that peer as out of sync);
  `CONKER_NET_TEST_HANG=<frame>[:<seconds>]` stops a peer's game for a while
  (30 s by default) with its network going on (it is dropped after 3 s of
  holding the host up). The host logs every wait of 100 ms or more ("waited
  N ms at frame F for player P", with how long that player had been quiet).
* `CONKER_NET_STATS=1` counts a session's traffic (src/net/net_stats.cpp) and prints a report
  every 10 s (`=N`: every N seconds) and when the session ends: packets and bytes on the wire (the
  sealed datagram plus IP and UDP headers) sent and received by type, a second, a packet and a game
  frame; the same since the last report (the match after the menus); and what the INPUT and MERGED
  packets sent carry (headers, times, frame numbers, records, hashes, drops) with how many records
  each held. Compare runs by bytes a frame: a run that plays slower sends fewer packets a second.
  tools/net-test/humanpad.py makes pads (hu1.pad, hu2.pad, hu3.pad from f1, f2, f3; seeded, so the
  same every time) that play the MULTI match with sticks that move every frame, as a person's do,
  so the traffic is a fair one to measure: `P1=hu1.pad P2=hu2.pad runsim.sh ...`.
  `CONKER_NET_TEST_FORGED_LEAVE=<frame>` on the host feeds itself a guest's
  LEAVE with a wrong secret, with none and with the right one: only the last
  may drop the guest (it prints PASS). `tools/net-test/rejoinrun.sh` takes
  `FAULT=term` (the guest quits normally) and `FAULT=stop` (frozen for
  `STOP_SECS`, then let go).
  A dropped guest starts again by itself to rejoin, and the forced trouble
  is cleared then unless `CONKER_NET_TEST_KEEP=1` (a second desync turns it
  away for good); `CONKER_NET_NO_REJOIN=1` keeps it stopped instead.
  `CONKER_NET_TEST_GUEST_PAUSE=1` on every peer lets a guest's Start pause a
  match again, to test handing a dropped player's pause to player 1.
  A rejoining guest starts from a snapshot of the host's game;
  `CONKER_NET_NO_SNAPSHOT=1` on the guest (or the host) has it replay from
  frame 1 instead. `CONKER_NET_TEST_SNAPSHOT_REFUSE=1` on the guest makes it
  refuse the snapshot (it replays, saying why), and
  `CONKER_NET_TEST_SNAPSHOT_SKEW=1` changes a byte after the snapshot is in
  place (the host then has it start again and replay, without counting a
  desync).
  Out of sync, both machines write the hashes of the last 1024 frames they
  played to
  `conker-recomp-data/net/desync-<session>-p<port>-f<frame>-<host|guest>.txt`;
  the folder keeps the 16 newest. The last 128 frames also carry one hash per
  64 KiB block of game memory (`b <frame> ...` lines), so
  `tools/net-test/compare_desync.py <host file> <guest file>` prints the first
  frame that differs and which blocks differ, from the two players' files alone.
  Next to each desync file is a `.ram` file (under 1 MB): that machine's hashed
  game memory for its last 16 frames, the first whole and the others as zlib-packed
  XOR against the frame before. `compare_desync.py --ram <host .ram> <guest .ram>`
  prints the 32-bit words that differ in the first frame both kept that differs.
  The folder keeps the 4 newest. `CONKER_NET_TEST_DESYNC=<frame>` changes the byte
  at 0x80200000 (it was 0x80100000, inside the depth buffer, which is now cleared
  every frame online).
  A packaged app writes its log, stdout and stderr together, to
  `last-run.log` in its data folder. A dropped guest starts itself again to rejoin
  at most 4 times in a row (`CONKER_NET_REJOIN_COUNT` counts them).
  The runs behind docs/online-multiplayer-plan.md's M3 table used these.
  A guest's secret for a host (the 64 bits every session datagram carries, so
  that a forged sender cannot pose as it) is kept in
  `conker-recomp-data/net/secrets.txt`, one `address secret` line a host
  (`address#token secret` for a lobby's session, so each session has its own),
  so the same machine gets its own port back; a dropped guest starting itself
  again passes it in `CONKER_NET_REJOIN_SECRET`. To join as another machine
  from the same folder, delete that file.
  `CONKER_NET_TEST_BUILD=<text>` makes a machine pose as another build, to see
  the lobby and the handshake turn it away; `CONKER_NET_TEST_ROM=<hex>` poses
  as another ROM (its hash) the same way. `CONKER_NET_TEST_DEAF=1` makes a
  hosting lobby ignore its game port while the LAN list still finds it, as a
  firewall dropping a joining game's replies looks, for JOIN's message then.
  `CONKER_NET_TEST_HOST_KEY=<64 hex>` gives a host that secret key instead of
  its kept one (`conker-recomp-data/net/host_key`), so that its join codes are
  known beforehand (tools/net-test/lobbystart.sh).
* Sealed packets (src/net/net_seal.hpp): every lobby and session packet but the
  LAN list's PROBE and OFFER is encrypted on a link of its own between a guest
  and the host. A packet that does not open is dropped without a word, as a
  forged one is. A host's lobby and session use the machine's kept host key; a
  guest's lobby hands the host's public key to its session in
  `CONKER_NET_HOST_PUBLIC` (or a join code's 10-hex-digit fingerprint of it).
  `conker-recomp-data/net/known_hosts` holds the key last seen at each address
  joined: delete a line to forget a host's key.
* LAN only (src/net/net_codes.hpp `sender_allowed`): without
  `--net-unsafe-internet`, every session, lobby and LAN-list socket drops a
  datagram from outside the LAN before looking at it. A build configured with
  `-DCONKER_NET_INTERNET_DEFAULT=ON` starts as if the switch were given (the log's
  UNSAFE line says "this build's default"); `--net-lan-only` turns it off, and
  the session a lobby starts is given `--net-unsafe-internet` or `--net-lan-only`
  to keep the lobby's choice.
  `CONKER_NET_TEST_INTERNET_PORT=<n>` makes a sender whose UDP source port is
  n count as one on the internet, so a crafted packet from 127.0.0.1 can stand
  in for a stranger without an address alias: a PROBE or HELLO from it gets no
  answer at all. Port-mapping
  tests (below) need `--net-unsafe-internet`, since nothing is mapped without it.
* The online lobby (src/net/lobby_ui.hpp, src/net/net_lobby.hpp):
  `--net-lobby` opens its page at boot. `CONKER_LOBBY_SCRIPT=<file>` drives
  it, a line each, `<ms> <action> [text]` from the page opening: `name`,
  `host [port]`, `join <address or code>`, `joinlan [host name]` (the LAN
  list's JOIN for that game, waiting until the list shows it), `ready` (sent,
  no longer needed to start),
  `start`, `kick <n>` (the host's REMOVE for its n-th guest), `leave`,
  `leavesession` (LEAVE THE SESSION), `quit` (as the window's close button),
  `key <key>` (a key pressed on the page: `up`, `down`, `left`, `right`, `enter`,
  `esc`, `back` for Backspace, or `text:<characters>` as typed),
  `close`, `open`, `setenv NAME=value` (for the session the lobby starts
  again into, such as a test's `CONKER_VIRTUAL_PAD`) and `shot <file>`, which
  writes what the page shows as text (players, LAN games, join codes, the
  port's state, errors). Opening the host's port on the router
  (src/net/net_portmap.hpp) is never wanted in a test:
  `CONKER_NET_PORTMAP=0` turns it off, and `CONKER_NET_UPNP_URL=<root
  description URL>` and `CONKER_NET_NATPMP_GATEWAY=<IPv4>` send it to a
  stand-in instead (with either set, only those are tried).
  `tools/fake_gateway.py` is that stand-in: UPnP IGD over HTTP and NAT-PMP on
  127.0.0.1, printing every mapping added and removed, and able to refuse
  (`--upnp fail|conflict`, `--natpmp fail`) or report a carrier-grade NAT
  address (`--external 100.64.1.2`).
* `CONKER_FRAME_INPUT_REPLAY=<file>` plays such a log back in place of the
  pads, frame by frame; past its end nothing is pressed. A log starts with `#`
  lines naming the build, the fixed step and every switch; the replay sets the
  switches as recorded (printing each it changes) and warns about a different
  build. Start the replay as
  the recording was started (same save, same switches, the same
  `CONKER_VIRTUAL_PAD` so the game finds the same controllers at boot) and
  with `CONKER_FIXED_STEP=1` on both: a single-player recording of 5,960
  frames replayed twice matched it in every `CONKER_FRAME_LOG` field. Without
  the fixed step a few slow loading frames are enough to send the replay
  elsewhere.
* `CONKER_STATE_HASH_LOG=<file>` writes, once a game frame, the frame number and
  a hash of each 16 KiB block of RDRAM up to 0x80400000 (256 a line);
  `CONKER_STATE_HASH_EVERY=<n>` takes every nth frame only. The OS timer at
  0x800BE6E0 and the game thread's message queue (D_800BEA10) are left out.
  `tools/compare_state_hashes.py a.log b.log --ignore START-END` prints the
  first frame two runs differ in and which blocks differ for how long; with
  the fixed step, `--ignore 0x80000000-0x80082B20` (OS structures, thread
  stacks, audio buffers) leaves nothing that differs between replays but
  the odd frame of a framebuffer (0x803B7600 to 0x803F5000).
  `CONKER_STATE_DUMP_FRAMES=<n>[,<n>...]` with `CONKER_STATE_DUMP_DIR=<dir>`
  writes the whole 4 MiB at those frames (`frame<n>.bin`, words in host
  order) to find the bytes, `CONKER_STATE_WATCH=<start>-<end>[,...]` with
  `CONKER_STATE_WATCH_LOG=<file>` appends those ranges every frame (after a
  32-bit frame number) for a block that differs only now and then, and
  `CONKER_SNAPSHOT_SAVE=<frame>:<file>` writes a snapshot of the game at
  the start of that frame (src/runtime/snapshot.hpp) and
  `CONKER_SNAPSHOT_LOAD=<file>` puts one in place at frame 3, after which
  the run goes on from the snapshot's frame: with the same replay on both,
  the two runs' hashes after that frame should be equal.
  `CONKER_SNAPSHOT_SELFTEST=<frame>` takes a snapshot at that frame and
  loads copies of it with the runtime's queue and thread structures changed
  as a hostile online host could change them; it prints a PASS line for
  each one refused with the game's memory untouched (and for the unchanged
  copy loaded). `CONKER_RUNTIME_SELFTEST=<frame>:queue` (or `:thread`)
  sends a message to a scratch queue with out-of-range indices (or with a
  copied thread record waiting on it): the runtime must stop the game with
  an `[ultramodern]` message instead.
  `CONKER_RUNTIME_SELFTEST=<frame>:wrap` stores the way recompiled code
  does through addresses that once landed outside the 4 GiB; each must wrap
  into game memory, printing a PASS line. `:guard_high` (or `:guard_low`)
  stores natively just past either end of the 4 GiB, which the guard pages
  must stop; `:pi` and `:pi_size` make ROM reads outside the cartridge, and
  `:ai` queues a 1 GiB audio buffer, and the runtime must stop the game.
  `CONKER_RENDER_SELFTEST=<task>:<kind>` does the same for the renderer at
  that graphics task: `ucode` (a task naming microcode that is not the
  game's: the game stops), and `vertex`, `extended`, `dlstack` and
  `loadtile` (a display list RT64 must refuse, printing a PASS line).
  `CONKER_ALLOC_LOG=<file>` logs every heap allocation and free the game
  makes with the frame and the recompiled functions that called it. The
  hashes, dumps and watch are taken from one copy of RDRAM, so they agree
  even where another thread is writing.
  `CONKER_ARG_TRACE=<file>:<first frame>:<last frame>` writes every recompiled
  function the game thread enters in those frames, by name, with `$a0`-`$a3`,
  `$f12`, `$f14` and `$sp`; each other game thread writes its own to
  `<file>.<its OSThread address>`. Names and the game's addresses are the same
  in every build, so two machines' logs of an online session (a Mac's and a
  Linux PC's) agree up to the first call where their games part. Frame numbers
  restart when a session starts in place: compare the last run of the frames.
  A guest that is dropped starts itself again and the restart truncates its
  logs: `CONKER_NET_REJOIN_COUNT=99` keeps it stopped so they survive.
  `CONKER_MY_VIEW_DUMP=<folder>` (a guest in a match with MY VIEW on) writes,
  every 150 frames, game memory with the game's own split-screen list
  (`f<frame>.game`) and with the "my view" pass's list (`f<frame>.mine`),
  big-endian, keeping the last 8; the log gives each list's start. Walking
  both lists shows what the pass draws differently: it found the empty
  scissor that clipped away a guest's weapons and HUD.
  `CONKER_AUDIO_COUNT_LOG=<file>` writes, after each audio frame, how many of
  the synth's update params, its free, allocated and stopping voices and the
  sound player's free events there are. Online the machines may hear sounds at
  different volumes (the "my view"), so the audio memory below 0x80082B20
  differs, but these counts must be equal on every frame of every machine.
  `CONKER_CALL_HASH_LOG=<file>` hashes the game thread's sequence of
  recompiled function entries (a line each 4096 calls and at the end of each
  frame), and `CONKER_CALL_HASH_REF=<log of another run>` compares as it
  goes: at the first line that differs it writes the last 8192 function
  names to `<file>.split`. A matching run with
  `CONKER_CALL_DUMP=<frame>:<calls>` writes the same window to `<file>.dump`
  to diff against, which names the function where the two runs took
  different paths. The hashes are of the names' addresses, so they compare
  only between runs of one build. docs/online-multiplayer-plan.md has what
  these found.
* `CONKER_FIXED_STEP=1` makes every frame step the game logic by the same
  amount, 2 retraces (1 with `CONKER_60FPS=1`), instead of the retraces the
  frame took, so what happens depends on the input and not on how fast the
  frames are made. Loading frames, which the game counts as long ones, then
  count as ordinary ones. The audio frames are then made by the game thread
  at the start of each frame (every second one at 60 fps) instead of by the
  audio thread on the real clock, so sounds end on the same frame in every
  run (src/runtime/frame_audio.cpp). The game thread also waits for RT64 to
  finish the graphics tasks it has sent at the start of each frame and when
  it handles a task's "done" message, since the game reads the depth buffer
  back from RDRAM, and Rare's paced EEPROM write takes one block every frame
  (src/runtime/frame_input.cpp). It is for repeatable runs, replays and
  online play.
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

A freeze where the picture and the frame log stop for good while the VI count
and the audio go on, with the gfx thread busy in RT64's `processDisplayLists`,
is RT64 reading a display list the game has rewritten. RT64 now stops such a
task and prints `RT64: display list at ... stopped after ...` instead
(`tools/patches/rt64-dl-command-limit.patch`, see `docs/recompilation.md`).
The known cause, a frame completed twice by Rare's scheduler under CPU load, is
fixed (`src/fixes/scheduler_yield.cpp`), so a new stop is worth a look. CPU
load makes such races far more likely, so a freeze in a test run is worth
checking for leftover game processes.

A debug report that shows the fault can be replayed by script for this: with
`CONKER_SKIP_INTRO=1` and the report alone in the run's `debug_reports`, the pad
lines `600 8 back`, eight `dpdown`s and two `a`s load it from the title.

## Save/load check

`tools/save_check.py` checks the pause-menu save/load restore (`docs/enhancements.md`,
SAVE and LOAD) over the debug reports in `conker-recomp-data/debug_reports/`, a
report at a time, each in a folder of its own under `build/save-check/`
(`--out`), several at once (`--jobs`). It starts the null build
(`build/host/ConkersBadFurDayRecompiled`, `--binary`; the ROM is `--rom`) with
`CONKER_SAVE_CHECK=<report>`, which loads the report a few seconds after boot
as LOAD REPORT does, writes the scene as the game loaded it just before the
restore (`capture-pre.resume`), as the restore left it (`capture-0.resume`) and
again after each of the `--at` times (`0,6,30` seconds unless set), in the
resume file's format with the running scripts' keys added, then makes a debug
report (a save made by the game from the restored scene) and quits
(`CONKER_SAVE_CHECK_OUT`, `_AT`, `_REPORT=1` and `_CHECKPOINT=1`, which starts
the save at its checkpoint instead, drive it by hand; `result.json` says how it
ended). `--no-run` only compares what an earlier run left.

The comparison, in `<out>/summary.md` (details in `details.json`), pairs each
saved character with the one the restore found for it (by model and id, as
`match_records` does) and reads, per table and offset:

* **lost**: a word not put back in the restore's own frame, not an address.
  Any is a restore that did not happen (a table skipped for its count, a
  character not matched).
* **undone**: a word put back, which the game then changed to what the fresh
  scene had before the restore and left so by the last capture: a restore the
  game overrides (the lives were one, swarms another). **settled** is one the
  game changed to anything else and left so, mostly characters walking to a
  spot; it is noise unless an offset shows in most reports.
* **missing** or **extra** characters, duplicate ids, characters that stand
  more than a unit from where they were saved.
* saved event scripts not running (in the restore's frame, or by the next
  capture), or running with other variables; swarms missing or elsewhere;
  Conker's kept actions and drunkenness.
* the save the game wrote from the restored scene against the one loaded: the
  slot's record byte by byte (`func_15006BEC`'s layout: event flags, the
  checkpoint, the money, bits of `D_800C3A60`), the play time left out.
* actor fields that follow the slots: where the fresh load put the characters
  in other slots than the save had them, a byte whose saved value names a
  slot and whose fresh value names that character's slot now (as the link byte
  `+0x65` does) is listed, since restored as saved it would point at the wrong
  character.

A report whose `save.resume` is not a version 2 file is refused, as LOAD
REPORT refuses it. Reports made in a session that had loaded another report
can carry what that load lost; the check tells restore fidelity (the report's
own scene comes back) on any report, but what happens in play afterwards is
only meaningful on reports made from unbroken play.

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
