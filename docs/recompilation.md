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
  hardware. These have no switch.
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

RT64's CMake turns plume's Retina support off, which would size the Metal
swap chain in points and leave macOS to upscale it 2x on a Retina screen. Our
`CMakeLists.txt` defines `PLUME_APPLE_RETINA_ENABLED` for the `plume` target
again, so the swap chain is in pixels; the SDL window is sized in pixels to
match (`src/platform/window_sdl.cpp`).
