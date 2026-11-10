# Building on Windows

Build on Windows itself, as below, or cross-compile on macOS or Linux (see
[Cross-compiling](#cross-compiling-from-macos-or-linux)). RT64 renders through Direct3D 12 (or Vulkan)
on Windows 10 version 1803 or later, or Windows 11. The build scripts are Bash,
so the steps run in Git Bash. See the [README](../README.md#building) for what
every build needs and the build options.

## One-time setup

1. Install [Visual Studio 2022](https://visualstudio.microsoft.com/) (the
   free Community edition is enough) with the **Desktop development with
   C++** workload, and in it the **C++ Clang tools for Windows** and
   **Windows 11 SDK** components. This brings clang-cl, CMake and Ninja.
   MSVC's own compiler (`cl`) is not supported.

2. Install [Git for Windows](https://git-scm.com/download/win), which brings
   Git Bash, and Python 3 from the Microsoft Store, which provides the
   `python3` command the scripts call. SDL2 comes with RT64, so it needs no
   install.

3. Open the **x64 Native Tools Command Prompt for VS 2022** and start Git
   Bash from it, so the compiler is on the path:

   ```bat
   "C:\Program Files\Git\bin\bash.exe"
   ```

   Do this every time you build.

4. Clone the two repositories side by side and put your ROM in the decomp's
   `roms` folder:

   ```sh
   git clone https://github.com/DevOldSchool/conkers-bfd-decomp.git
   git clone --recursive https://github.com/DevOldSchool/conkers-bfd-recomp.git
   cp /path/to/conker-us.z64 conkers-bfd-decomp/roms/baserom.us.z64
   ```

## Build

In `conkers-bfd-recomp`, from the Git Bash of step 3, with clang-cl as the
compiler:

```sh
export CC=clang-cl CXX=clang-cl
tools/apply_patches.sh
tools/regenerate.sh
cmake -S . -B build/rt64 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCONKER_RENDERER=rt64
cmake --build build/rt64
```

`regenerate.sh` builds N64Recomp itself the first time. The build compiles
about 54 MB of generated C, so it takes a while. It puts the DLLs the game needs
(SDL2 and RT64's shader compiler) next to `ConkersBadFurDayRecompiled.exe`, so
the `.exe` and `.dll` files in `build/rt64` are all another computer needs: the
C++ runtime is linked in, so no Visual C++ Redistributable is needed.

## Run

From the repository's folder, since settings, saves and texture packs live in
`conker-recomp-data` in the current folder:

```sh
build/rt64/ConkersBadFurDayRecompiled.exe /path/to/conker-us.z64
```

The ROM is kept in `conker-recomp-data`, so later runs can leave it out.

Opened from Explorer instead (a double-click, which starts it in its own
folder), the game asks for the ROM the first time and keeps a copy, with
settings, saves and `last-run.log`, in
`%LOCALAPPDATA%\ConkersBadFurDayRecompiled\conker-recomp-data`; the console
window it opens with closes. A run from a folder that already has a
`conker-recomp-data` folder keeps using that one.

The game is not signed, so Windows blocks a downloaded copy: before extracting
the zip, right-click it, choose **Properties** and tick **Unblock**.

A crash prints the faulting N64 address and a backtrace to the console (or the
log), named from `ConkersBadFurDayRecompiled.pdb` when it is next to the game.

## Packaging

Making the packages for a release is in [releasing.md](releasing.md).

## Windows on ARM

An ARM64 build runs natively on ARM PCs (Snapdragon and the like). The x64 build
runs there too, under Windows' emulation and more slowly; started that way it
says so in its log, and the first time in a window. RT64's own SDL2 and DXC are
x64 only, so an ARM64 build fetches DXC's release (which has the ARM64 DLLs) and
builds SDL2 from source, both at pinned versions (`CMakeLists.txt`).

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

## Cross-compiling from macOS or Linux

`tools/windows/cross_build.sh` builds the Windows game on macOS or Linux,
with the speedrun mod built in and off by default (`tools/build_mods.sh`, which
needs clang with the MIPS target and `ld.lld`; see [modding.md](modding.md)). It
uses clang-cl and lld-link, and the MSVC C runtime and Windows SDK that
[xwin](https://github.com/Jake-Shadle/xwin) downloads.

The D3D12 shaders compiled at build time must be signed, and must come from the
very same DXC version as the `dxcompiler.dll` the game ships: RT64 links them
with shaders it compiles at run time, and DXC refuses to link pieces from two
versions ("Cannot link libraries with conflicting compiler versions"). Only
Windows' `dxc.exe` matches the DLL, so with Docker running the build compiles
them with the `dxc.exe` of the DXC release whose DLLs it ships, under Wine in an
x86-64 Linux container (`tools/windows/dxc-wine.Dockerfile`,
`tools/windows/dxc_signed.sh`; emulated on Apple silicon, about 5 seconds a
shader). Without Docker this machine's DXC compiles them and the game crashes on
its first frame on Windows, so that build is only good for checking that the
code compiles. A build made on Windows needs none of this.

```sh
brew install llvm lld xwin    # macOS; on Linux, clang, lld and xwin from your package manager or cargo
tools/apply_patches.sh
tools/regenerate.sh
XWIN_ACCEPT_LICENSE=1 tools/windows/cross_build.sh
XWIN_ACCEPT_LICENSE=1 tools/windows/cross_build.sh rt64 arm64    # Windows on ARM
```

The first run downloads the runtime and SDK into `build/xwin` (about 800 MB),
which needs `XWIN_ACCEPT_LICENSE=1`: it accepts Microsoft's license for them.
The game lands in `build/windows-rt64` (ARM64: `build/windows-rt64-arm64`, with
the ARM64 runtime and SDK downloaded on its first run); `tools/windows/cross_build.sh null`
builds the headless one into `build/windows-null`.

Wine runs both; the RT64 one needs a Vulkan driver (Mesa's lavapipe works,
slowly, on a machine without a GPU).
