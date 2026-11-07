# Building on macOS

The tested platform. See the [README](../README.md#building) for what every
build needs and the build options.

## One-time setup

1. Install Xcode from the App Store. The Command Line Tools alone are not
   enough: RT64 compiles its shaders with Xcode's `metal`. Select it, accept
   its license and install its system components:

   ```sh
   sudo xcode-select -s /Applications/Xcode.app
   sudo xcodebuild -license accept
   sudo xcodebuild -runFirstLaunch
   ```

   If `xcode-select` says `invalid developer directory`, Xcode is not at that
   path: it is not installed yet, the App Store download has not finished, or
   the app has another name (such as `Xcode-beta.app`). `ls -d /Applications/Xcode*.app`
   shows what is there; pass that path to `xcode-select -s` instead.

   On Xcode 26 or later the Metal toolchain is a separate download (it fails
   with a missing `CoreSimulator` framework if `-runFirstLaunch` has not run):

   ```sh
   xcodebuild -downloadComponent MetalToolchain
   ```

   `xcrun -sdk macosx metal --version` should now print a version. The build
   checks this too when it is configured.

2. Install the tools with [Homebrew](https://brew.sh). If `brew` is not found
   afterwards, add it to your shell as its installer says
   (`eval "$(/opt/homebrew/bin/brew shellenv)"` in `~/.zprofile`) and open a
   new terminal:

   ```sh
   brew install cmake ninja python sdl2
   ```

3. Clone the two repositories side by side and put your ROM in the decomp's
   `roms` folder:

   ```sh
   git clone https://github.com/DevOldSchool/conkers-bfd-decomp.git
   git clone --recursive https://github.com/DevOldSchool/conkers-bfd-recomp.git
   cp /path/to/conker-us.z64 conkers-bfd-decomp/roms/baserom.us.z64
   ```

## Build

In `conkers-bfd-recomp`:

```sh
tools/apply_patches.sh
tools/regenerate.sh
cmake -S . -B build/rt64 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCONKER_RENDERER=rt64
cmake --build build/rt64
```

`regenerate.sh` builds N64Recomp itself the first time, and should end with
`done: N files in build/us/RecompiledFuncs`. If it stops before that, fix what
it reports before running `cmake`, which otherwise fails with
`No recompiled sources`. The build compiles about 54 MB of generated C, so it takes a while.

## Run

From the repository's folder, since settings, saves and texture packs live in
`conker-recomp-data` in the current folder:

```sh
build/rt64/ConkersBadFurDayRecompiled /path/to/conker-us.z64
```

The ROM is kept in `conker-recomp-data`, so later runs can leave it out.

## Packaging for another Mac

Run `tools/build_mods.sh` before building a release: it builds the speedrun
mod the packaged game carries, off by default (it needs `brew install llvm lld`;
see [modding.md](modding.md)), and `tools/package_macos.sh` refuses a build
without it.

`tools/package_macos.sh` turns the RT64 build into `dist/Conker's Bad Fur Day Recompiled.app` and
a zip of it to send. The app carries its own SDL, so the other Mac needs no
Homebrew, but it needs the same kind of chip (Apple silicon) and at least the
macOS version the build was made for (see *For an older macOS* below). The app is not notarised: on the other
Mac, right click it and choose Open the first time, or run
`xattr -dr com.apple.quarantine "Conker's Bad Fur Day Recompiled.app"`.

The app carries its licenses, and a `SOURCE.txt` naming the commit it was
built from, in `Contents/Resources/Licenses`. Package from a clean, committed
checkout, and attach `tools/source_archive.sh`'s source tarball to the release
beside the zip.

The app asks for the player's own ROM the first time it opens and keeps a
copy, along with settings, saves and `last-run.log`, in
`~/Library/Application Support/ConkersBadFurDayRecompiled/conker-recomp-data`.
`dist/ConkersBadFurDayRecompiled.dSYM` stays with you, for `tools/symbolicate.sh` on a crash
they send back.

### For an older macOS

Homebrew builds SDL for the macOS it runs on, so an app made with it needs that macOS too. For an
older one (macOS 14 here), build SDL3 and sdl2-compat from their source releases (the versions
`brew info sdl3 sdl2-compat` names) for it, then the game against them:

```sh
cmake -S SDL3-<version> -B build/sdl3 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DCMAKE_INSTALL_PREFIX="$PWD/build/sdl14" -DSDL_TESTS=OFF
cmake --build build/sdl3 && cmake --install build/sdl3
cmake -S sdl2-compat-<version> -B build/sdl2c -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DCMAKE_INSTALL_PREFIX="$PWD/build/sdl14" \
  -DCMAKE_PREFIX_PATH="$PWD/build/sdl14" -DSDL2COMPAT_TESTS=OFF
cmake --build build/sdl2c && cmake --install build/sdl2c
cmake -S . -B build/macos14 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCONKER_RENDERER=rt64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DSDL2_DIR="$PWD/build/sdl14/lib/cmake/SDL2"
cmake --build build/macos14
tools/package_macos.sh build/macos14
```

`tools/package_macos.sh` bundles the SDL3 next to the SDL2 the game links, and stops if either
needs a newer macOS than the game. Add `-ffile-prefix-map=$PWD=.` to the C and C++ flags so that
no folder of yours ends up in the binary.

## After pulling changes

```sh
git submodule update --init --recursive
tools/apply_patches.sh
tools/regenerate.sh
cmake --build build/rt64
```

The build stops with `config/us.toml changed since the code in ... was recompiled`
when `regenerate.sh` was skipped: the game's hooks live in that file and only reach
the build through `regenerate.sh`.
