# Building

Step-by-step guides:

* [macOS](building-macos.md) (the only platform tested so far)
* [Windows](building-windows.md)
* [Linux](building-linux.md)

## What every build needs

* CMake 3.20 or later, Ninja, Python 3 and SDL2
* a C++20 compiler (clang)
* the [decomp](https://github.com/DevOldSchool/conkers-bfd-decomp) checked
  out next to this repository, as `../conkers-bfd-decomp`. Only its scripts
  and inventory are used; it does not have to be built, so its Docker
  toolchain is not needed.
* your US ROM (SHA-1 `4cbadd3c4e0729dec46af64ad018050eada4f47a`)

The build has three stages: `tools/apply_patches.sh` patches the submodules,
`tools/regenerate.sh` turns the ROM into C with N64Recomp, and CMake compiles
that with the runtime and the renderer.

## Options

* Cloned without `--recursive`? Run `git submodule update --init --recursive`
  before `tools/apply_patches.sh`.
* Leave out `-DCONKER_RENDERER=rt64` for the null build: no window, sound or
  input, useful for scripted runs.
* With the decomp or the ROM somewhere else, set
  `CONKER_DECOMP=/path/to/conkers-bfd-decomp` or `CONKER_ROM=/path/to/rom.z64`
  for `regenerate.sh` (from a git worktree, `CONKER_DECOMP` is needed).
* Run `tools/regenerate.sh` again after changing `config/us.toml`.
