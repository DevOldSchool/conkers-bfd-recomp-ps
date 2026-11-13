# Making a release (maintainers)

How the packages players download are made. To build and play the game yourself, the build guides
([macOS](building-macos.md), [Windows](building-windows.md), [Linux](building-linux.md)) are all you
need.

## The release workflow

Pushing a `v*` tag runs `.github/workflows/release.yml`: it builds and packages the game for every
platform, attaches the source archive, and makes a draft release with notes generated from the pull
requests merged since the last one (`.github/release.yml` groups them by label). Check the draft,
tick "Set as a pre-release" for a beta (a tag such as `v0.1.0-beta.1`), and publish it. Run by hand,
the workflow only builds: the packages are the run's artifacts, kept for 3 days.

The debug information (the macOS dSYM and the Windows `.pdb`s) is never attached to the release. It
is kept with the run as the `debug-*` artifacts, which only the repository's collaborators can
download, for 90 days. Copy them somewhere private and permanent for a release that goes out: a
build's debug information matches only that build, and building the same commit again does not
give it back.

The steps below are what the workflow runs, for making a package by hand.

## Before packaging

* Package from a clean, committed checkout: the version is `git describe --tags`, and a build with
  uncommitted changes is marked `-dirty`.
* Run `tools/build_mods.sh` first. It builds the speedrun mod every package carries, off by default
  (it needs clang with the MIPS target and `ld.lld`, `brew install llvm lld` on a Mac; see
  [modding.md](modding.md)). The packaging scripts refuse a build without it.
* Attach `tools/source_archive.sh`'s source tarball (the complete source, submodules included, at
  the commit built) to the release beside the packages.

Every package carries its licenses and a `SOURCE.txt` naming the commit it was built from
(`tools/package_notices.py`).

## macOS

`tools/package_macos.sh` turns the RT64 build into `dist/Conker's Bad Fur Day Recompiled.app` and a
zip of it, with `Add to Steam.command` beside the app. The app carries its own SDL, so the other Mac
needs no Homebrew, but it needs Apple silicon and at least the macOS version the build was made for
(see *For an older macOS* below). The licenses are in `Contents/Resources/Licenses`.

The app is not notarised: on another Mac, open it once, then choose Open Anyway in System Settings >
Privacy & Security, or run `xattr -dr com.apple.quarantine "Conker's Bad Fur Day Recompiled.app"`.

Keep `dist/ConkersBadFurDayRecompiled.dSYM` (privately, not in the release) for
`tools/symbolicate.sh` on a crash report a player sends back.

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
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DSDL2_DIR="$PWD/build/sdl14/lib/cmake/SDL2" -DCONKER_HIDE_PATHS=ON
cmake --build build/macos14
tools/package_macos.sh build/macos14
```

`tools/package_macos.sh` bundles the SDL3 next to the SDL2 the game links, and stops if either
needs a newer macOS than the game. Configure with `-DCONKER_HIDE_PATHS=ON` so that no folder of
yours ends up in the binary or its debug information: the script refuses a game that still names
your home folder (`tools/windows/cross_build.sh` sets it itself, and `tools/windows/package.sh`
checks the same).

## Windows

```sh
tools/windows/package.sh
```

makes `dist/Conkers-Bad-Fur-Day-Recompiled-<version>-windows-x64.zip` (or `windows-arm64`, for an
ARM64 build): the game, its DLLs, a README, the Add to Steam scripts and `licenses`. The release
builds are cross-compiled ([building-windows.md](building-windows.md#cross-compiling-from-macos-or-linux)).

The `.pdb` (debug information) goes in a zip of its own, `...-pdb.zip`. Keep it privately, not in
the release: a crash report from a player's copy is only addresses until the `.pdb` is next to the
game, where the crash reporter names each frame from it.

## Linux

```sh
tools/linux/build.sh [platform]
```

builds in an Ubuntu 24.04 Docker container, for the machine Docker runs on unless a platform is
given (`linux/amd64` or `linux/arm64`), into `build/linux-<arch>`, and packages
`dist/Conkers-Bad-Fur-Day-Recompiled-<version>-linux-<arch>.tar.gz`. It needs `tools/regenerate.sh`
to have been run first, and builds the speedrun mod on the host.

The tarball carries its own SDL2 in `libs/` (built from source with `-DCONKER_BUNDLED_SDL2=ON`, so it
needs only the C library and loads X11, Wayland and the sound system at run time), and runs on
Ubuntu 24.04 or newer with a desktop, which brings GTK 3 and a Vulkan driver. Its `install.sh` adds
the game to the desktop's applications with its icon, and `add-to-steam.sh` adds it to Steam.
