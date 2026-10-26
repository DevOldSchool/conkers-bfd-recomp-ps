# Building on Linux

Not tested yet. RT64 renders through Vulkan on an X11 window, so you need a
GPU driver with Vulkan support. See the [README](../README.md#building) for
what every build needs and the build options.

## One-time setup

1. Install the tools and libraries.

   Debian and Ubuntu:

   ```sh
   sudo apt install git clang cmake ninja-build python3 libsdl2-dev libx11-dev libxrandr-dev
   ```

   Fedora:

   ```sh
   sudo dnf install git clang cmake ninja-build python3 SDL2-devel libX11-devel libXrandr-devel
   ```

   Arch:

   ```sh
   sudo pacman -S git clang cmake ninja python sdl2 libx11 libxrandr
   ```

2. Clone the two repositories side by side and put your ROM in the decomp's
   `roms` folder:

   ```sh
   git clone https://github.com/DevOldSchool/conkers-bfd-decomp.git
   git clone --recursive https://github.com/DevOldSchool/conkers-bfd-recomp.git
   cp /path/to/conker-us.z64 conkers-bfd-decomp/roms/baserom.us.z64
   ```

## Build

In `conkers-bfd-recomp`, with clang as the compiler:

```sh
export CC=clang CXX=clang++
tools/apply_patches.sh
tools/regenerate.sh
cmake -S . -B build/rt64 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCONKER_RENDERER=rt64
cmake --build build/rt64
```

`regenerate.sh` builds N64Recomp itself the first time. The build compiles
about 54 MB of generated C, so it takes a while.

## Run

From the repository's folder, since settings, saves and texture packs live in
`conker-recomp-data` in the current folder:

```sh
build/rt64/ConkersBadFurDayRecompiled /path/to/conker-us.z64
```

## Building in Docker

`tools/linux/build.sh` builds in an Ubuntu 24.04 Docker container for the
machine Docker runs on (arm64 on Apple silicon), into `build/linux-arm64`, and
packages `dist/Conkers-Bad-Fur-Day-Recompiled-<version>-linux-arm64.tar.gz`. It runs on
Ubuntu 24.04 or newer with SDL2, GTK 3 and a Vulkan driver; the README in the
tarball has the `apt` line. It needs `tools/regenerate.sh` to have been run
first.

In the tarball, `install.sh` adds the game to the desktop's applications with
its icon. Started from there, it asks for the player's own ROM the first time
and keeps a copy, with settings, saves and `last-run.log`, in
`~/.local/share/ConkersBadFurDayRecompiled/conker-recomp-data`. A run from a folder that
already has a `conker-recomp-data` folder, such as this checkout, keeps using
that one.

## After pulling changes

```sh
git submodule update --init --recursive
tools/apply_patches.sh
tools/regenerate.sh
cmake --build build/rt64
```
