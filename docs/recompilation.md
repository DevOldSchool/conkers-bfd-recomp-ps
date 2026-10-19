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
  are discarded.

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
* A textured one with a clamped texture, or one without a mask (not copy
  mode or a frame buffer copy), is built 512 pixels past that side in `RDP::drawTexRect`, with its
  texture coordinates carried on at the same rate, and the renderer opens
  only that side of its scissor. Once the circle is wider than 4:3 and the
  game stops drawing the side rectangles, the quarter circles go on past the
  4:3 edge at their true shape, and past their texture's edge they clamp to
  its black outer texels. In 4:3 the scissor clips them as before.

All of these are candidates for upstreaming.
