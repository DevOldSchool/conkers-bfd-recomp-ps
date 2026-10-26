# Recompilation setup

* Conker links its main executable for a TLB alias at `0x10000000` and its
  game overlay for `0x15000000`, demand-paged 4 KiB at a time from RZIP
  chunks. Sections are described at those runtime addresses so every call,
  address constant and jump table resolves. Details in
  [memory-layout.md](memory-layout.md).
* Function boundaries come from the decomp's `progress/functions.json`, its
  ELF for main's SDK code, `config/game/us-sdk.ld` for SDK code in the game
  overlay, and direct-call targets for anything left uncovered.
* The runtime makes the `0x10000000` window a shared-memory alias of RDRAM,
  inflates the game overlay into the `0x15000000` window, copies the debugger
  overlay to `0x16000000`, and registers every function at its runtime
  address so function-pointer calls resolve.
* The 13 stubbed functions are listed with reasons in `config/us.toml`.
* libultra internals Rare calls directly (SI/PIF access, SP task submission by
  raw DMA, VI/timer internals, controller-pak code, libc helpers) are
  implemented natively in `src/runtime/ultra_extras.cpp`;
  `tools/gen_extras_decl.py` keeps the list of required ones in sync.
* `[[patches.hook]]` snippets in `config/us.toml` call helpers in
  `src/fixes/` and `src/enhancements/`, for example to keep the music manager
  from dropping a sequence the audio thread has not started yet (see
  [rsp-microcode.md](rsp-microcode.md)), and to carry out the geometry code's
  escape from `func_150AE280` back to `func_150ADAF0`'s caller with
  `setjmp`/`longjmp` (`src/fixes/geometry_escape.h`). Hooks inside a shared
  code region go on the region's first function (`func_150A3A70`), because
  the region's single body is emitted with it.

## Source layout

* `src/runtime/`: boot, the address-space windows, overlay registration,
  RZIP, RSP task dispatch, the function-entry trace and the libultra
  internals above.
* `src/platform/`: host backends for audio, input, the window and the
  renderer, with a null/headless and an SDL/RT64 version of each.
* `src/fixes/`: hooks that make the recompiled game behave as it does on
  hardware, or mend Rare's own slips (`level_seams.cpp`: cracks in the level
  geometry). These have no switch.
* `src/enhancements/<feature>/`: one folder per feature the console does not
  have (see the README), each with its own `CONKER_*` switch. A new
  enhancement gets a folder here, its source in `CMakeLists.txt`, and its
  hooks in `config/us.toml` under a comment naming the folder.

Includes are written from `src/`, as in `#include "runtime/memory.hpp"`.

## Patches to the submodules

`tools/patches/n64recomp.patch` (apply in `lib/N64Recomp`):

* jump tables may live in a different section from their function (IDO puts
  main's tables in rodata at a KSEG0 address while the code runs through the
  TLB alias);
* one run reports every failing function instead of stopping at the first;
* **shared code regions**: a section may declare `regions = [{ start, end }]`.
  Every function inside is emitted as a thin wrapper around one body per
  region, which begins with a `switch` over every instruction address. Entries,
  computed jumps (`jr $reg`) and `jr $ra` with an explicitly loaded `$ra`
  resolve to labels; the wrapper seeds `$ra` with a sentinel so routines that
  return through another register (`or $t7, $ra` ... `jr $t7`) still return.
  Calls inside a region to targets in the same region link `$ra` and jump to
  the label (hardware semantics, since the callee may return through a
  continuation register), `jalr` links and dispatches, and the jump register
  of `jr`/`jalr` is read before the delay slot runs. This is how Rare's
  handwritten geometry code (0x150A3A70:0x150AE35C) and the handwritten
  prefix-code tree builder `func_15018500` (0x15018500:0x150186D0, returns
  through `$s4`) are recompiled;
* stack accesses with negative or unaligned offsets, tables without in-function
  entries and undecodable words no longer fail a function; writes to `$zero`
  are discarded;
* `div`, `divu`, `ddiv` and `ddivu` by zero give the VR4300's results (a
  quotient of -1, or 1 for a negative signed dividend, and the dividend as the
  remainder) instead of dividing on the host. Rare's hand-written geometry code
  divides by zero without a check: `div $s3, $fp` at `0x150A3EE4` does so
  during boot, as found in our own testing by logging every division by zero
  in the recompiled game. A plain C division trapped there on x86 (SIGFPE, so
  Windows and Linux builds would crash) and gave a quotient of 0 on ARM64.

`tools/patches/n64modernruntime-running-thread.patch` (apply in
`lib/N64ModernRuntime`) adds `ultramodern::set_running_thread_address`, a
libultra-style `__osRunningThread` mirror that Rare's code reads directly.

`tools/patches/n64modernruntime-pi-dma-message.patch` (apply in
`lib/N64ModernRuntime`) makes `osPiStartDma` fill in the `OSIoMesg` (type,
priority, return queue, addresses, size) and makes PI DMA completion send that
`OSIoMesg` pointer to the return queue, as libultra's PI manager does, instead
of a null message.

`tools/patches/n64modernruntime-keep-rdram.patch` (apply in
`lib/N64ModernRuntime`) stops `recomp::start` from unmapping the game's memory
on quit. The game's own threads are never stopped, so one still running after
that faulted on freed memory (`fatal signal 11` at a valid N64 address as the
window closed). The process is exiting, so the OS frees it.

`tools/patches/rt64-f3dex2cbfd.patch` (apply in `lib/rt64`) adds Rare's
F3DEX2CBFD microcode: GBI database entries for the eight variants, a
`GBI_F3DEX2CBFD` command table on top of F3DEX2 and the RSP state for its
vertex normals, 48-byte lights, coordinate modifiers and TRI4. Details in
[rsp-microcode.md](rsp-microcode.md).

`tools/patches/rt64-native-size.patch` (apply in `lib/rt64`) adds
`presentation.nativeSize` to RT64's enhancement configuration: the rendered
picture is presented at its own size, centred, instead of being scaled to fill
the window. With a manual resolution multiplier it draws an exact multiple of
the picture in a window or on a screen whose size is not one (the OPTIONS
CENTERED setting, `src/platform/renderer_rt64.cpp`).

`tools/patches/rt64-edge-rects.patch` (apply in `lib/rt64` after
`rt64-native-size.patch`) adds `rect.extendEdgeRects`. With a widened aspect
ratio (the OPTIONS WIDESCREEN setting), RT64 keeps 2D rectangles in the
original 4:3 area unless one spans the whole scissor. Conker's circle wipe
masks the picture with black fill rectangles on each side of four textured
quarter-circle tiles, all clipped to the 4:3 screen, so the scene showed at
both edges. With the option, a rectangle that reaches one side of the scissor
but not the other carries on to that side of the frame buffer:

* An untextured one has that edge, and its scissor's, moved to the frame
  buffer's edge, so it meets it exactly rather than leaving a sliver where
  RT64's rounding of the 3D scissor differs.
* A textured one (not copy mode or a frame buffer copy) that the game cut off
  at the screen's edge (its texture coordinate there is not at the tile's
  edge), such as a speech bubble or a cloud partly off screen, is drawn on in
  `RDP::drawTexRect` to the tile's edge, with its texture coordinates carried
  on at the same rate, so the rest of it shows. It carries on further, up to
  512 pixels, only when the tile's column on that side is at least two thirds
  opaque (read from the RDRAM its queued load comes from): clamping to it then
  continues a solid edge, like the black outside the wipe's quarter circles
  once the circle is wider than 4:3 and the game stops drawing the side
  rectangles. Clamping a sprite's edge would smear it into a streak. In 4:3
  the scissor clips them as before.
* Any textured rectangle that reaches past a side of the scissor, like a
  speech bubble near the edge of the screen, gets that side of its scissor
  opened, so it is not cut off at the edge of the 4:3 area.

`presentation.expandCropSides` leaves columns out at each side of the frame
buffer when it is presented with a widened aspect ratio, and Expand widens the
aspect ratio to make up for them. Conker never draws the two columns at each
side (in 4:3 they are the picture's black edges); widened, nothing draws there
either, so they showed as thin bars, black or holding stale pixels.

The patch also keeps RT64's render targets and frame copies when the window
changes size while the resolution is manual and the aspect ratio does not
follow the window. RT64 dropped them on every resize, which lost the pause
menu's copy of the last frame; the renderer holds a manual resolution and a
fixed aspect ratio while that menu is open (`src/platform/renderer_rt64.cpp`).

All of these are candidates for upstreaming.

`tools/patches/rt64-memory-packs.patch` (apply in `lib/rt64`) lets a
`ReplacementDirectory` carry a `FileSystem` the application made, used in place
of a folder or zip. The sharper text (`src/enhancements/sharp_font/sharp_font.cpp`)
is such a pack, made in memory from the ROM's glyphs, so it needs no files and
loads before the packs in `conker-recomp-data/texture_packs`.

`tools/patches/rt64-wireframe.patch` (apply in `lib/rt64` after the patches
above) and `tools/patches/plume-wireframe.patch` (apply in
`lib/rt64/src/contrib/plume`) add the debug menu's wireframe view
(`src/enhancements/debug_menu/debug_menu.cpp`). plume's command lists get
`setWireframe`, which Metal applies as the triangle fill mode of each draw;
the other back ends ignore it. `RT64::SetRenderWireframe` turns it on for the
game's 3D triangles (indexed draws) only, so rectangles (text, sprites) stay
solid, and clears each color target to black the first time it is drawn to in
a frame: Conker never clears its frame, since the scene covers it, so without
that the lines would be drawn over an old picture.

`tools/patches/plume-gpu-errors.patch` (apply in `lib/rt64/src/contrib/plume`
after `plume-wireframe.patch`) and `tools/patches/rt64-queue-names.patch`
(apply in `lib/rt64`) report command buffers the GPU fails. RT64 never looked
at whether one succeeded, so on macOS a GPU fault only showed in the system
log. plume's command queues get a name (`setName`, Metal only), which RT64 sets
to the worker's ("Workload Graphics", "Present Graphics", ...), and while
`plume::renderCommandErrorCallback()` is set, Metal command buffers keep their
encoders' execution status and call it with the queue, the error and the
encoders that faulted (`src/platform/gpu_faults.cpp`).

`tools/patches/rt64-metal-float-to-int.patch` (apply in `lib/rt64`) fixes the
GPU faults those reports found. RT64's pixel shader picks a draw's LOD tile from
`int(floor(log2(maxDst)))`, where `maxDst` is the largest texture-coordinate
derivative. When the coordinates do not change across a pixel, `maxDst` is 0
and `log2` gives -inf. D3D12 and Vulkan convert that to INT_MIN, which the clamp
that follows brings back to tile 0, but on Metal converting an infinite or NaN
float to an int is undefined, and some of the specialized pipelines the
compiler made indexed `RDPTiles` with garbage: a read far outside the buffer
("GPU Address Fault Error"), and after a handful of them macOS stopped running
the game's GPU work, leaving the window black. Metal's shader validation showed
it as "Invalid device load" in `PSMain` at offsets whose low bits were an
`RDPTile` field and whose tile index was INT_MIN plus the draw's first tile. The
patch keeps the value finite before converting it, and does the same for the
mip level of replacement textures with mipmaps (a negative or infinite float
converted to uint). RT64's build does not track the shaders' `.hlsli` includes,
so an existing build needs `touch lib/rt64/src/shaders/RasterPS.hlsl` to
rebuild them. A candidate for upstreaming.

`tools/patches/rt64-dl-command-limit.patch` (apply in `lib/rt64`) keeps a
broken display list from hanging the game. If the game rewrites the display-list
buffer RT64 is still reading, RT64 runs on into vertex and matrix data, where a
G_POPMTX count can be millions and a G_DL can branch back on itself, so the task
never ends, no DP-done reaches the scheduler, and the game waits for that frame
for ever while VIs and audio go on. The patch stops a task after 4M commands (a
frame is tens of thousands) with `RT64: display list at ... stopped after ...`
on stderr, and pops at most the matrices on the stack. The game then loses that
one frame and runs on.

The rewrite came from Rare's scheduler (func_800049E0) under CPU load, and
`src/fixes/scheduler_yield.cpp` now prevents it. When the audio timer fires
while a graphics task runs, the scheduler asks the RSP to yield and waits in
state 3 for the SP completion, which then starts the audio task in D_8002AC54.
The timer message has already cleared D_8002AC6C, so a retrace that falls in
that wait takes the next audio task into D_8002AC54 and sets a second timer. On
hardware the yield takes microseconds. Here yields are ignored and a graphics
task's SP completion is sent when the gfx thread picks the task up, which on a
busy machine can be several retraces later. The second audio task is then
started twice, its second SP completion is taken for a graphics one, and
func_80005020 completes the frame a second time. The game's pending-frame count
(D_800BEAA8) is then one below the frames really queued. At the next scene
change the game sees it reach 0 with a frame still queued, frees both
display-list buffers, and builds the new scene's first two frames into the same
memory while RT64 draws that old frame from it. The fix keeps D_8002AC6C set
while state 3 waits, as the short hardware yield effectively does. In stress
runs (three games and ten busy loops) 4 of 24 runs reached the rewrite without
it and 0 of 33 with it.

`tools/patches/rt64-object-picking.patch` (apply in `lib/rt64` after the
others) is for the debug menu's INSPECT (`src/enhancements/object_inspector`).
While `RT64::SetDrawTagging` is on, a G_SPNOOP whose low 24 bits are
`RT64_DRAW_TAG_PUSH` or `RT64_DRAW_TAG_POP` pushes or pops a draw tag (its
second word), and each draw call records the innermost tag and the addresses
of the display list its triangles came from, of the command that drew the
first of them and of that command's vertices (`DrawCall::drawTag`, `drawList`,
`drawCommand`, `drawVertices`); a change of tag or display list ends the draw
call. `RT64::SetWorkloadHook` sees each workload
with its draw calls complete, before it is handed to the renderer, so a hook
can set their highlight colours (RT64's debugger tint), and
`RT64::GetPresentedPicture` gives the window rectangle the last frame was
presented in and its widening, to take the mouse to frame buffer pixels.

`tools/patches/rt64-wide-draw-rect.patch` (apply in `lib/rt64` after the
patches above) fixes objects that vanish beside the original picture when it is
widened. RT64 sizes each framebuffer pair's render target from the pair's draw
rect, which only counts what lands inside the game's 4:3 scissor. Conker splits
its frame into many pairs (each switch to a small off-screen image starts a new
one), so a pair whose triangles inside the scissor all lie near the top got a
target that ended above an object drawn wholly beside the scissor further down,
and that object was clipped away: the plank bridge on the left pillar in scene
41 disappeared once all of it was left of the 4:3 picture. With a widened
aspect ratio such a triangle now counts its rows at the scissor's nearest
column.

`tools/patches/rt64-near-draw-rect.patch` (apply in `lib/rt64` after the
patches above) fixes the bottom of the picture going missing when the camera
stands over level geometry, as in the Uga Buga tunnel (scene 2). The same draw
rect counted each triangle by its vertices' screen positions, but a vertex
behind the camera (w <= 0) divides to the wrong side of the screen, so a floor
triangle running from under the camera to in front of it seemed to end well
above the bottom edge. When no other triangle in the pair reached lower, the
target ended there (row 167-186 of 216) and the rows below kept whatever the
frame buffer held before: black after a load, older frames otherwise. The
triangles themselves were drawn; only the target was too short. A triangle with
a vertex behind the camera now counts the screen bounds of the part in front of
it (clipped at a small positive w), and the backface test, which the flipped
positions make meaningless, no longer leaves it out of the rect.

`tools/patches/rt64-grey-from-green.patch` (apply in `lib/rt64` after the
patches above) brings back the black and white of cutscenes such as the Tediz
war briefing in scene 6. The game greys those frames on the CPU after the RDP
has drawn them: at the swap, the scheduler (`func_80005020`) runs the
colour-framebuffer effects (`func_1515FDA0`) when the frame's scheduler task
has flag `0x40`, and effect type 2 (`func_150AF790`) has `func_150B1DB0` copy
each RGBA5551 pixel's green bits into red and blue. RT64 shows the frame it drew
on the GPU, so the CPU's change in RDRAM never reached the screen and the
cutscene stayed in colour; the pause menu's copy of the frame read the CPU's
grey back at the N64's resolution instead. The patch adds a `GreyFromGreen`
framebuffer operation, which draws a copy of a colour target back over it with
green in all three channels (`GreyFromGreenPS.hlsl`), and
`State::requestGreyFromGreen`, which queues it after the last pair that draws
to an image in the display lists about to be processed.
`src/fixes/frame_effects.cpp` asks for it when a task is handed to RT64, by the
same rules `func_1515FDA0` applies at the swap, and skips the CPU pass.

`tools/patches/rt64-frame-effects.patch` (apply after
`rt64-grey-from-green.patch`) does the same for the other two
colour-framebuffer effects, which were lost the same way. Effect type 0
(`func_150CE450`) is the mosaic that censors the caveman in scene 20's
cutscene 21 (script opcode 5, selector `0x60`, the only use in the game's
scripts): `func_150CE200` keeps a rectangle on the character each frame, and
inside it each pixel takes a source pixel that steps on once every
`0x10000 / step` pixels across and down (step 10000, blocks of about 6.5
pixels). Effect type 1 (`func_151CB110`) is the lens in the middle of the
sight that `func_151CAD28` makes for the aiming modes `0x0C`, `0x16`, `0x1F`
and `0x29` (`func_151C9BA0`; the mounted guns use `0x1B`, which has none):
within 12 pixels of the view's middle, a point `d` radii out shows the point
`d * (1 - 0.67 * sqrt(1 - d.y^2) * sqrt(1 - |d|^2))` from a copy of the 24x24
square around it, magnifying up to 3x. The patch adds `FrameMosaic` and
`FrameLens` framebuffer operations (`FrameMosaicPS.hlsl`, `FrameLensPS.hlsl`)
and `State::requestFrameEffect`, which queues each after the last pair that
draws to its image. Positions are the frame's own pixels, placed in the middle
of a target widened for the aspect ratio. The mosaic computes the N64's
source pixel for each block, in closed form, and fills the block with the
middle of that source pixel, so its blocks are the N64's at any resolution.
The lens is computed for every target pixel rather than for whole N64
pixels, so it magnifies smoothly; the N64's loop also never writes the lens's
middle row, a quirk left out here. `src/fixes/frame_effects.cpp` reads the
rectangle and step, or the view's middle and where the copy was taken, from
each effect when its task is handed to RT64, and skips the CPU passes.

RT64's CMake turns plume's Retina support off, which would size the Metal
swap chain in points and leave macOS to upscale it 2x on a Retina screen. Our
`CMakeLists.txt` defines `PLUME_APPLE_RETINA_ENABLED` for the `plume` target
again, so the swap chain is in pixels; the SDL window is sized in pixels to
match (`src/platform/window_sdl.cpp`).
