# Enhancements

What this port adds to the original game. Each one is on by default unless
it says otherwise, and the `CONKER_*` environment variable it names turns it
off for a run. The summary is in the [README](../README.md#enhancements).

## Display options

The OPTIONS page of the title menu has five rows under the sound setting.

* SHARP TEXT or ORIGINAL TEXT: the [sharper text](#sharper-text) and
  [menus](#sharper-menus), or the game's own. It switches at once and is on
  by default.
* WINDOWED, BORDERLESS or FULL SCREEN. Full screen (the default) is macOS
  full screen in a Space of its own; borderless covers the screen in the
  current Space, with the menu bar and Dock hidden while the game is in
  front.
* 4:3 or WIDESCREEN. Widescreen widens the view to the shape of the window
  or screen instead of stretching the picture: the window becomes 16:9
  (384x216 for each multiple), and covering the screen the picture takes the
  display's own shape (16:9, 16:10 or ultrawide). The HUD and menus stay in
  the middle, and the circle wipe that opens and closes a scene covers the
  whole width. Covering the screen is widescreen by default; a window is 4:3
  by default, as on an N64. The game decides what to draw for the widened
  view too, so the level, props and characters at the sides are not left
  out as they were past the edges of its 4:3 view
  (`CONKER_WIDESCREEN_CULLING=0` keeps the 4:3 choice); the tiled sky gets
  vertex buffers big enough for the extra tiles. The metal frame of the
  turret's first-person view reaches the sides, its plates carrying on with
  more bolts (in 4:3 too it now closes the pixel of scene the game leaves
  past each end). Every view of a multiplayer split screen is widened alike
  (tools/patches/rt64-split-screen-wide.patch): RT64 used to widen only a view
  as wide as everything else it was drawn with, so of two players' views one
  could stay 4:3. A view's own fades, the circle wipe when a player dies and
  the rocket launcher's scope cover its widened width too; the wipe's circle
  stays centred on its player and its black stops at the view's edge. Each
  view's HUD (score, ammo, radar and the aiming reticle) stays centred in its
  widened view, so the reticle is over the view's middle. In the
  intro, Conker is not drawn while he waits for his cue just past the edge of
  the 4:3 view. Behind the pause menu the paused scene stays widescreen, but
  sharp: the game blurs only the 4:3 middle of the frame, so that blur is
  skipped in widescreen. Switching between the window and full screen while
  paused keeps the paused picture as it was, fitted to the new size; the new
  resolution and shape apply once you carry on.
* The resolution: how large the game's 292x216 picture is rendered, in whole
  multiples from 292x216 (1x). In a window it is the window's size, up to
  the largest that fits the display (1460x1080, 5x, on a 2560x1440 display
  with the Dock showing, which is also the default). Covering the screen it
  goes up to the first multiple that covers the screen's height (2044x1512,
  7x, on 1440 lines, the default). Widescreen widths are shown in
  widescreen. Sizes are in pixels, also on a Retina screen, where the game
  draws at the screen's full pixel density: on a 3024x1964 screen a
  window goes up to 2044x1512 (7x, half that in points), and covering the
  screen up to 2920x2160 (10x).
* FILL SCREEN or CENTERED: the picture is scaled to fill the window or
  screen, or drawn at its own size in the middle with black bars, so that
  every game pixel is an even block. Covering the screen it fills by
  default; a lower resolution then fills it with larger pixels. A window is
  centered by default, which only shows once it is resized by hand.

The window and the screen-covering modes each keep their own resolution,
shape and fill. F11, Cmd+Enter, Alt+Enter or Ctrl+Cmd+F switch between the
window and the last screen-covering mode. The choices are kept in
`conker-recomp-data/display.cfg`, not in the game's save, so the OPTIONS
saved to EEPROM are unchanged. `CONKER_DISPLAY=windowed`, `borderless` or
`fullscreen` overrides the mode for one run, `CONKER_DISPLAY_OPTIONS=0`
hides the rows, and `CONKER_ORIGINAL_SIZE=1` opens the original 960x720
window (in points) instead.

## Skip to the title menu

L at any point during the boot logos or the intro fades to black and then
into the GAME 1 menu, skipping the title scene's opening (the clock and the
Cock and Plucker) too. On an N64, Start only skips the intro after about
five seconds and after the reset button, and the title scene needs another
press. `CONKER_SKIP_INTRO=1` skips without a press; `CONKER_SKIP_INTRO=0`
keeps the original behaviour, where the intro cannot be skipped after a cold
boot.

## Skip any cutscene

On an N64, L only skips a cutscene already watched, and some, such as the
whole opening of a new game, not at all. Here L skips every cutscene, the
first time too. Skipping any part of the opening goes on to where Conker
wakes up, its own skip target in the game's data (about 45 seconds into a
new game instead of 7 minutes). `CONKER_SKIP_CUTSCENES=0` keeps the original
rules.

## SAVE and LOAD in the pause menu

Two signs sit under the lever, LOAD below QUIT and SAVE below CONT.... Push
down from the sign above, or use the stick's down-left and down-right
diagonals, and press A. SAVE saves the game where Conker stands, and LOAD,
or starting that save from the title menu, puts him back on that spot,
facing the same way, with the camera put back where it looked once it has
settled behind him: the same direction, distance and pitch, and the game's
camera kept at that distance (saves made before this keep only the spot;
saves from before the distance was kept take the distance the camera comes
to rest at, and a debug report's takes it from its `report.json`). Quitting
from the pause menu keeps the spot too. The spot is kept in
`conker_us.resume` next to the save; a checkpoint reached later takes over
as usual. The game's own save only records finished events, so the spot
also keeps the scene as it was: the game's own tables of characters, scene
objects, triggers and event progress, copied whole (so a gate a conversation
opened stays open, a conversation that plays once does not play again, and a
count a character keeps is kept), the players' records (so the lives count
is kept; loading a save otherwise starts it again), the variables of the
event scripts running, and the swarms of bees in scene 12, where each one
was and what it flew after (so a swarm already led to the sunflower stays
there, and one following Conker goes on following him), what a script has
left B doing for Conker away from any B pad and how long he has left drunk
(so after drinking at Rock Solid he is still drunk: he hiccups and staggers,
and B pees).
Each character is
put back over the same character, found by the number the scene gives it,
since a scene does not always fill its character slots in the same order;
characters the game had brought in while playing (scene 6's three wasps) are
brought in again, and ones it had taken away are taken away. Each scene
object is put back over the one with its own index, so a pickup taken before the save, which a fresh load makes again, does not
stop the rest of the table (a gate) from being put back. The checkpoint the
save restarts from is kept too: starting a scene as the game is started
would otherwise take the entrance the spot came in by as the checkpoint's. A
save made during a cutscene (a debug report can be) keeps the scene as the
cutscene found it, and the cutscene plays again from its start.
Words in those tables that hold memory addresses belong to the run of the
game that made them and are left as they are. The file is JSON. Neither
sign is offered when replaying a chapter, where the game does not save. The signs are lettered with Rare's own letters, cut from
the game's other green signs in your ROM. `CONKER_PAUSE_SAVE=0` turns them
off.

Every save made this way is also kept as its own snapshot in
`saves/snapshots/`, named after the scene number and when it was made
(`scene12-20261020-143205.bin` with a matching `.resume`). LOAD still starts
the latest save; to go back to an earlier one, copy its two files over
`conker_us.bin` and `conker_us.resume` while the game is closed, or load it
from the debug menu. `CONKER_SAVE_SNAPSHOTS=0` stops keeping them.

## Debug menu

View on an Xbox pad, Create (Share) on a PlayStation pad, or the `` ` `` key opens
it anywhere: in play, in the pause menu, on the title and the game's other
menus, and in cutscenes. In play it is the game's pause with its own page in
place of the lever, PAUSED and the HUD, so the game stops as it does for the
pause menu. Where the pause cannot open, the menu stops the game itself with
the same pause flag (the world, the cutscene and the sound stop; the screen's
own menu waits underneath). The game reads no buttons while it is open, nor
after it closes until they are let go. It closes with B, Start or the same
button. Up and down choose a row, A or left and right change it:

* WIREFRAME: the game's 3D triangles drawn as their edges on black; text,
  sprites and the HUD stay solid. Metal only for now.
* SHOW FPS: the frames the game draws per second, in the top left corner.
* SHOW POSITION: the scene, Conker's X, Y and Z and his facing, and the
  camera's yaw and pitch, under the frame rate during play. Angles are in
  degrees, and facing and yaw share their zero (+Z) and direction (towards
  +X), so walking away from the camera they agree. The first page shows them
  too, and opening the menu prints them in the terminal, with the camera's
  position, the point it looks at and its forward vector.
* INSPECT: for telling exactly which model or object has a problem. With it
  on (or `CONKER_INSPECT=1` from the start) the mouse cursor shows, and what
  is under it is tinted cyan and named under the other overlay lines: an
  actor with its index, model number and model name (from the decomp's model
  registry), an attachment with its parent actor, a prop with its index in
  the prop table, an object of a layer (grass and other effects) with the
  callback that drew it, a 2D sprite tile by its resource number (and, for
  the menus' signs, which sign and tile: `SPRITE GAME1 1/3 (2011)`), or else
  the game function that drew it
  (LEVEL for the terrain, `FUNC 1515D6D0`, ...), then the display list it came from, its
  triangle count, its texture hash and, for an actor or prop, where it is. A
  left click picks it out: every part of it pulses magenta (for the level,
  every part with the same texture), and the terminal gets a full report
  (scene, tag, display list, the command that drew it and its vertices,
  combiner, texture hash, and for an actor its table address, model, kind and
  position, for a prop its position and record). A right click lets it go.
  Texture transparency is not known, so a cut-out (a grass sprite's clear
  parts) covers what is behind it: the mouse wheel steps through everything
  under the cursor, front to back (shown as 2/3), until the mouse moves. Text
  is never picked. RT64 build only; see
  `src/enhancements/object_inspector/object_inspector.cpp`.
* ENHANCEMENTS: every enhancement that can be switched while the game
  runs, each ON or OFF, starting as its `CONKER_*` variable set it: SHARP
  TEXT (the sharper text and menus), WIDESCREEN (the current mode's shape),
  WIDESCREEN CULLING, SKIP INTRO, SKIP CUTSCENES, PAUSE SAVE AND LOAD, SAVE
  SNAPSHOTS, SURROUND DEFAULT, FULL DETAIL, DRAW DISTANCE, NO RUMBLE
  PAK BANNER, CONSISTENT CONTROLS, PAD BUTTONS and 60 FPS. A or left and right switch the
  chosen one. SHARP TEXT and WIDESCREEN are the OPTIONS rows' settings and
  are kept in `display.cfg` as those rows keep them (a new shape applies once
  the game carries on); the rest last until the game is closed, and a debug
  report records them as they are. Each acts the next time the game comes to
  it: SKIP INTRO only during the boot logos and the intro, PAUSE SAVE AND
  LOAD at the next pause, SURROUND DEFAULT only when the game finds no saved
  options, the banner when the title menu next comes up. The tiled sky's
  room is set when a scene starts, so WIDESCREEN CULLING switched on in a
  scene may leave out sky tiles at the sides until the next one. The OPTIONS
  page's own rows (`CONKER_DISPLAY_OPTIONS`) and the debug menu are not
  switched here.
* TELEPORT: X, Y, Z, FACING and CAMERA YAW, set to where Conker is and
  changed with left and right by STEP (1, 10, 100 or 1000; held, they keep
  changing). GO puts him there facing that way, and the camera where it
  looks along CAMERA YAW at him, as far away as it was: the same view as the
  numbers on screen, to the hundredth of a degree, for repeating what
  someone saw. The camera's pitch is the game's own for that spot (it keeps
  the camera at the height it wants), so it is shown and recorded but not
  set. RESET TO CURRENT sets the values back to where he is and how he and
  the camera face, REMEMBER keeps the values shown in
  `conker-recomp-data/debug_spots.txt` (one line each: `scene x y z angle
  facing camera-yaw camera-pitch`) and shows what it kept, and SPOTS lists
  those of the scene being played with their facing, yaw and pitch; A on one
  puts him there. Teleports stay within the scene.
* LOAD SNAPSHOT: the snapshots in `saves/snapshots/`, newest first. A makes
  the chosen one the save, as copying its files over `conker_us.bin` and
  `conker_us.resume` would, and starts it where it was saved, as LOAD does.
  Progress since the last snapshot is not kept.
* SAVE DEBUG REPORT: for reporting an issue. Makes a folder in
  `conker-recomp-data/debug_reports/`, named after the scene and the time,
  with `report.json` and a save of the game as it is at that moment
  (`save.bin`, `save.resume`). The JSON has the build (`git describe` of the
  commit it was built from), the scene and entrance, Conker's position and
  facing, the camera (position, look-at point, forward vector, yaw, pitch),
  every enhancement (`id`, `name`, `on`, its `CONKER_*` `variable` and its
  `options` as values), the settings that go with them (display, texture
  packs, controller, frame rate, `display.cfg`), the computer (operating
  system, CPU, memory, GPU and graphics API, display size; nothing that
  names the computer or its user), the `CONKER_*` variables set (a path
  keeps only its file name), and what save is in the folder. The save is
  made by the game's own save routine into a copy: the player's save is not
  changed, and no snapshot is made. Where the game does not save (a chapter
  replayed from CHAPTERS), there is no save.
* LOAD REPORT: the reports, newest first; A starts a report's save where and
  as it was made, Conker's facing and the camera included. It works from the
  title menu too, so a report is a few seconds from starting the game
  (`CONKER_SKIP_INTRO=1` skips the intro). Reports can come
  from anyone, so one is checked first and refused, with the reason on
  screen and in the log, unless: its files are regular files (no links) of
  the right sizes (`save.bin` exactly 2048 bytes), `report.json` is a
  version 1 report with a scene, entrance and position in range, and
  `save.resume` is a well-formed resume file (the points and their scene
  state, within its limits) that starts with the report's slot and
  matches the JSON's slot, save number, scene and position. A report from
  another build is loaded, and the log says so. The save is played on a
  copy, `saves/debug/<report>.bin`, until the game is closed, so the
  player's own save is not touched (saves made meanwhile go to the copy).

The settings last until the game is closed. `CONKER_DEBUG_MENU=0` turns it off.

## Surround by default

With no saved options (a new save file), the OPTIONS menu's sound setting is
surround instead of stereo. The game keeps its options in EEPROM next to the
saved games, so a sound setting chosen and saved from that menu stays as it
was. `CONKER_SURROUND_DEFAULT=0` keeps the stereo default.

## Characters at full detail

The game swaps Conker for a lower-detail model once the camera is more than
500 units away, which in play is often its usual distance (the blockier tail
is the easiest place to see it), and for cruder ones further out. He, and
the few other characters with more than one model, keep their most detailed
model at any distance. `CONKER_MAX_DETAIL=0` keeps the game's choice.

## Everything seen from farther

The game draws a scene out to its far distance, 10000 to 20000 units from
the camera, where its fog closes in, but what stands in the scene had
shorter limits of its own: characters faded out past 3000 units (the
sunflower on the ledge past the stone steps in scene 12 only appeared on
walking up to it), most of the props in scene 12 past 4500 or 5000, the
swarms of bees in scene 12 past 2500, and the grass, flowers and mushrooms
past 1500 units from Conker (the grass across the clearing in scene 57).
Each of those limits is now two and a half times as far, though never
past the scene's own far distance: enough that nothing pops in across the
picture, while what is still left out is small and deep in the fog. Only
their drawing changes: what the characters and props do does not depend on
it, and the little effects the grass and flowers set off stay around Conker
as before. A scene with more grass and flowers than the game builds in a
frame (110) would keep their limit, so that farther ones never take the
place of nearer ones; none has been seen. `CONKER_DRAW_DISTANCE=0`
keeps the game's distances.

## No Rumble Pak banner with a rumbling pad

The title menu's "INSERT RUMBLE PAK" banner shows on an N64 even with the
pak inserted. It is left out when controller 1's pad can rumble;
`CONKER_RUMBLE_BANNER=1` shows it.

## Pad haptics

A pad that can rumble plays taps and buzzes as well as the Rumble Pak's
on and off, and some of the game's sound effects now play one: the
slingshot coming out, each shot in the grip of the trigger that fired it,
the elastic stretched again and the click when it is ready, a hard hit
and a short heavy rumble in both grips for each shotgun blast, zoomed or
not, a thump when Conker lands from a jump, on any ground, and a much heavier one when
he splats after a long fall or is squashed flat. The game's own rumble is unchanged. More sounds are a
line each in src/enhancements/haptics/haptics.cpp, found with
`CONKER_SOUND_LOG=1` (see [diagnostics](diagnostics.md)).

On macOS a DualSense plays them shaped, through Core Haptics, which reaches
the pad over Bluetooth too. Every other pad, and every pad on Windows and
Linux, plays them on its rumble motors: the heavy one in the left grip takes
the soft thumps, the light one in the right the crisp taps. An Xbox One or
Series pad also has a motor in each trigger, and the slingshot is felt in
the trigger that fired it. `CONKER_HAPTICS=0` turns it off, and
`CONKER_HAPTICS_MOTORS=1` plays them on the motors on a DualSense on macOS
too.

## Peeing and the machine gun in the triggers

A DualSense's triggers (both are Z) take the feel of the two things
Conker does by holding Z:

- **Peeing, drunk.** Once B starts the stream, the triggers resist
  lightly, then harder as holding Z sends it further, firmest when it
  reaches as far as it goes, and they ease off again as it falls back.
  They go free when he stops.
- **The mounted machine gun**, as in the wasps' hive: the triggers pull
  heavy past the first fifth of their travel, and every round the gun
  fires thumps in both grips, since the gun is held with both hands.
  Nothing shakes while the barrels wind up, and each thump is short
  enough to leave a gap before the next, so the rounds are felt one by
  one.

The triggers only resist; they don't shake. On macOS a DualSense's
thumps go through Core Haptics; any other pad that can rumble plays them
on its motors, without the trigger resistance. `CONKER_ADAPTIVE_TRIGGERS=0`
turns it all off, and ADAPTIVE TRIGGERS on the debug menu's ENHANCEMENTS
page switches it.

## Consistent controls

The controls stay the same across the game, the way a modern pad's games
have them: the left stick moves Conker, the right stick turns the view or
aims, and pushing up looks or aims up. The N64 game changes them from one
place to the next, and aims like a plane, pushing up to look down.

* The right stick turns the view the way it is pushed: C-left and C-right
  are swapped (also on J/L), as the N64's C-left swings the view right.
* Holding R to look around in first person, the stick pushed up looks up.
* With the shotgun (Spooky's mansion, out with B), the left stick moves him
  as before, and the right stick steps him (C-up and C-down forwards and
  back, C-left and C-right sideways, still facing the same way), pushed
  right stepping right, where the camera's swap would turn it round.
  With R held to aim, the right stick aims, and the left stick walks and
  steps.
* The multiplayer matches' guns (the shotgun in TEMPLE, the Uzi and the
  others that aim with R) handle as the shotgun does, and get the same.
* With the rocket launcher or the scoped gun in a multiplayer match, while
  its scope is up (R), the right stick aims and the left stick walks.

With the keyboard and mouse the mouse is that right stick: it turns the
camera itself (0.15 degrees a pixel, times `CONKER_MOUSE_SENSITIVITY`), and
while aiming it aims (see the controls in the README). The keyboard alone,
with the mouse not captured, keeps the game's aiming. In a multiplayer match every player's pad gets
these controls with their own right stick; only controller 1 has the mouse. `CONKER_CONSISTENT_CONTROLS=0`
leaves the game's controls, and CONSISTENT CONTROLS on the debug menu's
ENHANCEMENTS page switches it.

## Your pad's buttons

Where the game shows a button to press (in the menus and the tutorial's
speech bubbles), it shows the N64's. With an Xbox or PlayStation pad as
controller 1 they are drawn as that pad's buttons instead, in the style of
Rare's pre-rendered ones, and change when another kind of pad is connected:

| N64 | Xbox | PlayStation |
| --- | --- | --- |
| A | A | Cross |
| B | B | Square |
| START | Menu | Options |
| Z | RT | R2 |
| L, R | LB, RB | L1, R1 |
| C-left, C-right | right stick pushed right, left (left, right without consistent controls) | the same |
| C-up, C-down | right stick pushed up, down | the same |

B is also on the other side face button (X on Xbox, Circle on PlayStation) and Z on the left trigger,
and the C pictures point the way to push the stick, which is swapped left to
right by [consistent controls](#consistent-controls); switching those off in the debug menu turns them round. The Xbox buttons are black with coloured letters, the
PlayStation ones black with the DualShock 4's coloured symbols. With a
PlayStation pad the B pads on the ground, wooden (octagonal, hexagonal and
square), stone, teal, grey stone, the rug in Count Batula's house and the round stone plate on a post in the water, have a Square where their B was, made from their textures in your
ROM and shaded as the B is;
an Xbox pad's B is B, and other pads keep the N64's, so they stay as they
are.

With no pad connected (the keyboard and mouse alone) the pictures are the
keys that press each button, dark keycaps with white letters: Space for A, E
for B, Esc for START, C for Z (crouch), Q for L, F for R, and I, K, J and L
for the C buttons (J and L swapped with the camera, as the stick is), and
the B pads on the ground have an E where their B was, drawn as the
PlayStation Square is. With another pad (an N64 pad, a Switch Pro) the
N64's buttons are shown, redrawn the same way, sharper than the game's small
pictures; ORIGINAL TEXT in the OPTIONS menu keeps Rare's. `CONKER_PAD_ICONS=xbox`,
`playstation`, `keyboard` or `n64` picks a set whatever the pad, and `0` keeps Rare's
pictures, as does PAD BUTTONS switched off in the debug menu. The pictures
are drawn when the game starts (about a fifth of a
second, for the connected pad's set only; `src/enhancements/pad_icons`), so
none is stored; `CONKER_DUMP_PAD_ICONS=<dir>` writes the set in use out as a
pack, and a texture pack can replace them.

## Sharper text

The game's text font is enlarged 8 times with Super-xBR, a pixel-art
upscaler, from the glyphs in your ROM when the game starts: the same
letters, smoother and less blurred than the N64's filtering. The game draws
each glyph a little short of its edges, which its blur hides, so those
rectangles are widened over each glyph's empty border and the sharper
letters are not cut off (`src/platform/font_rects.cpp`). The bubbles'
pictures, such as the "more..." under a thought bubble, are drawn starting
on a whole texel: the game sometimes starts them half a texel into a row,
which blends each row into the next so the word looks doubled.
`CONKER_SHARP_FONT=0` keeps the original font.

## Sharper menus

The menus' lettered signs and headings (GAME1, OPTIONS, PLAY, the chapter
names, the multiplayer modes, PAUSED and the rest) are drawn unfiltered, so
at high resolutions they show as blocks. They are enlarged 4 times with
Super-xBR from your ROM when the game starts, each heading as one picture so
no seams show between its tiles, and so are the menus' joystick, the OPTIONS
speakers, the button prompts, the multiplayer heads and player pieces and the
pause menu's coloured digits and dollar sign. The small P1 to P4 badges use the
large badges' artwork. The speech and thought bubbles, and their shadows,
are enlarged the same way. Other pictures such as the small statistics icons are left
as they are. `CONKER_SHARP_MENUS=0` keeps the original signs; the OPTIONS
row turns the sharper text and menus off together.

## Texture packs

RT64 replacement packs (a folder, `.rtz` or `.zip` with an `rt64.json`) in
`conker-recomp-data/texture_packs` are loaded at start, in name order; where
two replace the same texture, the later name wins, and any of them wins over
the sharper text and menus and your pad's buttons. `CONKER_TEXTURE_PACKS=0` loads none.
`CONKER_DUMP_SHARP_FONT=<dir>` and `CONKER_DUMP_SHARP_MENUS=<dir>` write the
sharper text and menus out as packs to edit; `tools/font_pack.py` writes the
font's glyphs with their hashes as a template for new art.

## 60 fps (experimental)

Off by default. `CONKER_60FPS=1` lets the game start a frame on every retrace
instead of every second one. Its logic already steps by elapsed time, so
movement, jumps, the camera, animation and cutscenes keep their speed; a few
things still assume 30 fps. See
[investigations.md](investigations.md#60-fps-conker_60fps1-experimental) for
what was measured.
