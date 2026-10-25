# Building on Windows

Not tested yet. RT64 renders through Direct3D 12 on Windows 10 or 11. The
build scripts are Bash, so the steps run in Git Bash. See the
[README](../README.md#building) for what every build needs and the build
options.

## One-time setup

1. Install [Visual Studio 2022](https://visualstudio.microsoft.com/) (the
   free Community edition is enough) with the **Desktop development with
   C++** workload, and in it the **C++ Clang tools for Windows** and
   **Windows 11 SDK** components. This brings clang-cl, CMake and Ninja.

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
cp lib/rt64/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/lib/x64/SDL2.dll build/rt64/
```

`regenerate.sh` builds N64Recomp itself the first time. The build compiles
about 54 MB of generated C, so it takes a while. The last line puts SDL2's
DLL next to the game, which needs it to start.

## Run

From the repository's folder, since settings, saves and texture packs live in
`conker-recomp-data` in the current folder:

```sh
build/rt64/ConkerRecomp.exe /path/to/conker-us.z64
```

## After pulling changes

```sh
git submodule update --init --recursive
tools/apply_patches.sh
tools/regenerate.sh
cmake --build build/rt64
```
