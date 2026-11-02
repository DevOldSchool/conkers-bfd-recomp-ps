# Modding

Code mods are N64Recomp `.nrm` files, the format Zelda64Recomp mods use. A
mod is C compiled for the N64, which can replace a game function or run its
own code at the start or end of one. librecomp (in N64ModernRuntime) loads
mods when the game starts and recompiles their code for the host.

**Only install mods from people you trust.** A mod is a program someone else
wrote, running inside the game on your computer. See [Safety](#safety) for
what the game does to limit what a mod can do, and what it cannot promise.

## Installing a mod

Put the `.nrm` file in the `mods` folder of `conker-recomp-data` (next to the
stored ROM and `saves`; the game makes the folder the first time it runs). A
mod found there for the first time is off until you turn it on.

The debug menu's MODS page (Back on the pad, or the `` ` `` key) lists the mods,
with A or left and right turning one on or off. Mods load when the game
starts, so after a change the page adds RESTART TO APPLY, which starts the
game again (what is not saved is lost). The choice is kept in
`conker-recomp-data/mods.json`: `enabled_mods`, and `mod_order`, where mods
later in the list win where two touch the same thing.

The log (`last-run.log` for the app) shows `Loading mod <id>`, and the
game shows a message if a mod fails to load.

Online sessions load no mods, on any machine, so they stay in sync. A debug
report lists the installed mods and which were on.

## Making a mod

`mods/speedrun` is a complete mod to copy: Speedrun, a speedrun timer
for chapters replayed from CHAPTERS. Playing a sub-chapter from the title's
CHAPTERS page (a hook on `func_151E30C4`) times the rest of that chapter, in a
pane at the left edge of the window. A hook on `func_15042D78`, which the
game's menu handler calls once a frame before it queues any text, adds up the
time from `osGetCount`, leaving out the time the pause menu is open, and
queues the pane as HUD text with `func_15042D94`, the way the game writes its
own; each digit of a time has a place of its own, as the game's letters are
not all as wide. The flag `HUD_TEXT_LEFT_EDGE`, which the port adds, counts
the text's x from the window's left side in widescreen instead of the 4:3
picture's.

Splits come from `event_flags`: each sub-chapter has an event that ends it,
and splits when that is set, in whatever order they are played, with its time
counted from the split before. Where nothing better is known that is the event
the CHAPTERS page lists it by, which the game sets as some sub-chapters start
and as others end. Most ends in the table come from replaying every
sub-chapter from CHAPTERS and comparing the events each replay sets beforehand
with the next one's; a few were found by playing (SCAREDY BIRDY as Conker
sobers up, PAN HANDLED as the key opens the door, MRS BEE as she has her hive
back). GARGOYLE's event is set as he is spoken to, so it splits when Conker
leaves that level. Each split shows how far ahead (-) or behind (+) the best
time from before the run it is. A sub-chapter keeps two bests, one for a split
after another and one for the run's first, which a replay times from its start
and so leaves out what came before it in the chapter; the chapter keeps one
total for each sub-chapter a run can start at. WINDY's sub-chapters are played
in between BARN BOYS' (and other chapters run into each other the same way),
so another chapter's page event being set ends the run, and so does an end
marked as the end of a stretch (MRS BEE's, before BARN BOYS). The bests are
kept with `conker_mod_data_save`. While it runs, hooks on `func_1501E400`,
which runs a cutscene for a frame, hold L for the game, so every cutscene is
skipped as soon as the game allows (with SKIP CUTSCENES on, the port's
default, that is every one). `conker.h` declares what it uses, and names every
cheat's bit in `cheat_flags`.

You need:

* clang with the MIPS target and `ld.lld`. On macOS Apple's clang has no
  MIPS target: `brew install llvm lld`.
* RecompModTool, built from the repository root:
  `cmake -S lib/N64Recomp -B build/N64Recomp -G Ninja && cmake --build build/N64Recomp --target RecompModTool`
  (`tools/regenerate.sh` builds the rest of that folder).

Then in `mods/speedrun`:

```sh
make
make install DATA=/path/to/conker-recomp-data
```

`make` writes `build/<mod_filename>.nrm`. To start a new mod, copy the folder,
then change `id`, `mod_filename`, `display_name` and the rest of the
`[manifest]` in `mod.toml`. A copy outside the repository needs `REPO` set to
the repository (`make REPO=/path/to/conkers-bfd-recomp`), and the two symbol
files in `mod.toml`'s `[inputs]` pointed at its `config` folder.

### Game functions and data

Mods name the game's functions and data as the
[decomp](https://github.com/DevOldSchool/conkers-bfd-decomp) does. Functions
(`func_15042D94` and so on) link against `config/us.syms.toml`, and data
against `config/us.data.syms.toml`. Most data has no real name yet, only the
decomp's placeholder for its address, so a mod declares it as that:

```c
extern u32 D_800E9D00;  // the cheats entered
```

`mods/include/conker.h` declares what the example uses, under names of its
own for what is known (`cheat_flags` for `D_800E9D00`). `CONKER_DATA(type,
address)` reaches memory no symbol names. When the decomp names a function or
data, `CONKER_UPDATE_SYMS=1 tools/regenerate.sh` rewrites both symbol files
and mods can use the name (the placeholder goes away with it).

### What a mod can do

The markers are in `mods/include/modding.h`:

* `RECOMP_PATCH` before a function named like a game function replaces it.
  The replacement has to do the whole job; it cannot call the original. For
  example, this one makes all text queued after a scale change twice as big:

  ```c
  RECOMP_PATCH void func_150432BC(f32 scale) {
      hud_text_scale = scale * 2.0f;
  }
  ```

  A game function with variable arguments can be written with fixed ones
  (the extra arguments are on the caller's stack either way) and given the
  game function's name with an asm label, as in
  `void my_text(s32 x, s32 y, u8 flags, const char* format, s32 a0, s32 a1, /* to a15 */ s32 a15) __asm__("func_15042D94");`
  followed by its definition.
* `RECOMP_HOOK("func_X")` runs a function at the start of `func_X`, with the
  same arguments. `RECOMP_HOOK_RETURN("func_X")` runs one just before it
  returns.
* `RECOMP_EXPORT`, `RECOMP_IMPORT`, `RECOMP_DECLARE_EVENT` and
  `RECOMP_CALLBACK` share functions and events between mods.
* librecomp's own functions are imported from `"*"`: `recomp_alloc` and
  `recomp_free` (memory outside the game's), the mod config calls
  (`recomp_get_config_u32` and the rest, for options declared in the
  manifest's `config_schema`; the game has no page to change them yet, so
  they keep their defaults), `recomp_get_save_file_path`,
  `recomp_get_mod_folder_path`, `recomp_get_mod_file_path` and
  `recomp_change_save_file` (a save file of the mod's own).
* The port's own, also imported from `"*"` and declared in `conker.h`:
  `conker_mod_data_load` and `conker_mod_data_save` keep up to 16 KiB of the
  mod's own data between runs (best times, settings), in
  `saves/mods/<mod id>.bin` in the data folder. The file is named by the
  loader's id for the mod, not by the mod.

### What a mod cannot do

* Hook or replace a function the port itself changes: the functions with a
  hook in `config/us.toml`, the stubs, and Rare's handwritten geometry code
  (the shared code regions in `config/us.syms.toml`). Such a mod fails to
  load with "Function is not hookable" or "Attempted to replace a function
  that's been patched by the base recomp". The list is generated into
  `build/<build folder>/generated/mod_locks.h`. `RECOMP_FORCE_PATCH`
  replaces one anyway, and drops the port's change to it (an enhancement or
  fix may stop working).
* Replace the functions the port implements natively (libultra internals,
  `memcpy` and the rest in `src/runtime/ultra_extras.cpp`).
* Use the C library: there is none (no `printf`, `memcpy` or `sinf`). `sqrtf`
  and `sqrt` work, as the FPU's own instruction.

`float` and `double` both work. The game runs its FPU with 32 independent
registers (FR=1); the mods tested ran float and double arithmetic,
conversions, `sqrt`, float arguments between a mod's functions, into a game
function and into a hook, and a matrix product in the odd registers, all
with exact results. The Makefile still keeps singles out of odd registers
(`-mno-odd-spreg`), which costs nothing.

## Safety

A mod can change anything in the game: its memory, what it draws and plays,
and so your saves. Beyond that the game limits it:

* **No native code.** librecomp can load a mod's own native library (the
  manifest's `native_libraries`, or a native build beside an `.offline.nrm`),
  which would run with everything your account can do. This game refuses such
  mods before opening anything ("Mod contains native code").
* **Addresses stay in the game's memory.** A mod's MIPS code is recompiled
  for your computer, and every address it uses is meant to be a game
  address. Only an address's low 32 bits count, as on the N64, in the mod's
  code, in the game's and in librecomp's: whatever a register holds, a read or
  write lands in the game's 4 GiB of address space, and going past its end
  faults in the guard pages around it. Mods with MIPS III's 64-bit
  instructions are still refused ("Mod contains 64-bit instructions"); the
  mod template builds MIPS II code, which never has them.
* **Save files stay in the saves folder.** `recomp_change_save_file` only
  takes a name of letters, digits, `_` and `-` (at most 64); any other name
  leaves the save file as it is.

  Both are checked by `tests/mod_sandbox` on every pull request (see
  [recompilation.md](recompilation.md)).
* **New mods start off.** Dropping a file in `mods` is not enough: it has to
  be turned on on the MODS page, which says to trust where it came from.
* **Not online.** Sessions load no mods.

What it cannot promise: the code that reads a mod (its zip, its manifest
and symbols, and the recompiler) was not written to withstand a mod built to
attack it, and a bug there could let one out. A mod can also make the game
crash, or ruin a save. That is why the rule is the one at the top: only
install mods from people you trust, and keep a copy of saves you care about.

## How it works

* The game overlay is compressed in the ROM, and librecomp recompiles a
  hooked function from its MIPS code, so the game gives librecomp the same
  virtual ROM `tools/gen_syms.py` builds for N64Recomp, with the inflated
  game code at 64 MiB (`build_virtual_rom` in `src/runtime/game.cpp`).
* librecomp replaces a function by writing a jump over its start. macOS on
  Apple silicon does not let a program write to its own code, so instead
  librecomp records the replacement
  (`tools/patches/n64modernruntime-mods.patch`) and each recompiled
  function checks for one on entry (`CONKER_MOD_REDIRECT` in
  `src/runtime/trace.h`). Until a mod replaces or hooks something that check
  is two loads and a branch. This is used on every platform.
* IDO put almost every jump table in the data, not next to its function's
  code. The static recompiler finds them through `tools/patches/n64recomp.patch`;
  librecomp's own copy of N64Recomp, which recompiles a hooked function,
  gets the data sections from the game
  (`tools/patches/n64modernruntime-n64recomp-jump-tables.patch`, and
  `kModJumpTableData` in the generated `mod_locks.h`). Without it a hook on
  a function with a `switch` failed to load.
* The locked functions get a ROM size of 0 in the section table
  (`src/runtime/overlays.cpp`), which librecomp already refuses to hook, and
  the patch makes it refuse to let a mod replace them too.
