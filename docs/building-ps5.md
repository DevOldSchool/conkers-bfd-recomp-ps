# Building for the PS5 (experimental)

The game builds as a native PS5 homebrew title, for a console that runs homebrew (a jailbroken one;
tested firmware for the toolchain: 13.60). It is rendered by RT64 on Vulkan through Mihawk's PS5 port of
Mesa's RADV driver ([PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)).

Tested on a console on firmware 13.60, online with a Mac included.

## Build

Needs Docker (Linux, or macOS with Docker Desktop; Apple silicon works) and the recompiled code from
`tools/regenerate.sh`.

```sh
tools/ps5/build.sh            # deps (first time: RADV, SDK, PacBrew), configure, build, package
```

The steps can also be run one at a time: `tools/ps5/build.sh deps|configure|build|package`.

- **deps**, once per dependency volume: Mihawk's [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan),
  [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) and
  [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) at the commits `tools/ps5/build.sh`
  pins, and by PS5_Vulkan's own tools the payload SDK, the release build of RADV, `ps5-native-tool`
  and `runtime/libc.prx`; then PacBrew's prebuilt ports (v0.40.2, SDL2 among them). Building RADV
  takes the longest, the first time only.
- **configure**: RT64's `file_to_c` for the build machine, then CMake with `tools/ps5/toolchain.cmake`.
- **build**: the game, linked into the signed title by `tools/ps5/link-title.sh`.
- **package**: `dist/PPSA92064` (below).

The image is `conker-ps5-build` (`tools/ps5/Dockerfile`). `CONKER_GENERATED=<dir>` uses another
checkout's `build/us`. The dependencies (and the compiler cache) and the build folder are Docker
volumes, `conker-ps5-deps` and `conker-ps5-build`; `CONKER_PS5_DEPS_VOLUME` and
`CONKER_PS5_BUILD_VOLUME` name others, for CI or a build from scratch. `CONKER_BUILD_ARGS` is passed on to
Ninja. The title folder is `dist/PPSA92064`; `build/ps5/ConkersBadFurDayRecompiled.elf` in the build
volume is the title unsigned and with its symbols, for reading a crash report.

## Updating from the main project

This repository is the main project ([conkers-bfd-recomp](https://github.com/DevOldSchool/conkers-bfd-recomp))
with the PS5 port on top, and tracks it as the remote `upstream`. To take in its latest changes:

```sh
git fetch upstream
git merge upstream/main
git submodule update --init --recursive
```

The PS5 port changes the main project's files only in a few places, each behind `CONKER_PS5` (or
`if(PS5)` in CMake), so merges rarely conflict. Three files differ on purpose, and a conflict in them
keeps this repository's side:

* `README.md` is this repository's own (`git checkout --ours README.md`).
* `.github/workflows/release.yml` and `rom-build.yml` (the desktop releases and the regression
  build) are removed here: `git rm` them again. `.github/workflows/ps5.yml` builds the title instead.

A fresh clone adds the remote once:
`git remote add upstream git@github.com:DevOldSchool/conkers-bfd-recomp.git`.

## CI and releases

`.github/workflows/ps5.yml` regenerates the code from the ROM (from the private repository the
`rom` environment names, as in the main project) and runs `tools/ps5/build.sh all` on every push to
main, keeping the zipped title for three days. Nothing is cached between runs: the build's volumes
hold code made from the ROM.

A tag `NN.NNN.NNN` makes a draft release. The tag must be `contentVersion` in
`tools/ps5/sce_sys/param.json` (the version the console shows, and the one the homebrew catalog
takes), so a release raises that first. The draft carries:

* `Conkers-Bad-Fur-Day-Recompiled-PS5-<version>.zip`: the title folder, `PPSA92064/` at its top,
  licenses included;
* `…-symbols.zip`: the unsigned program with its symbols, for reading a crash report's addresses;
* the complete source, submodules included, and the PS5 libraries' source at the pinned revisions;
* `SHA256SUMS.txt`.

The draft's text starts from `tools/ps5/release-notes.md`: fill in its first lines (what the version
brings) before publishing. The homebrew catalog shows a release's text as the app's notes, on its
page and in ProsperoStore, as plain text and cut after about 1,800 characters, so what changed comes
first and the install steps (where the ROM goes) last.

## Licenses

The package step writes `licenses/` into the title folder (`tools/ps5/package_notices.py`): this
project's MIT license, the GPL-3.0 (N64ModernRuntime, PS5_Vulkan, the payload SDK and `libc.prx`
make the title GPL-3.0 as a whole), every library's notice, and `SOURCE.txt` with the exact commit
and dependency revisions. Licenses the build does not keep are in `tools/ps5/licenses`; a library
added to the title needs an entry in `tools/ps5/package_notices.py`.

## Install

1. Copy `dist/PPSA92064` to `/data/homebrew/` on the console (FTP).
2. Put your own US ROM in that folder, with a lower-case `.z64`, `.n64` or `.v64` extension. On the first
   start the game keeps its own copy; the file you added can then be removed.
3. Register the title (ShadowMountPlus or etaHEN) and start it from the home screen.

`tools/ps5/upload.sh` does the first two steps over etaHEN's FTP server:

```sh
PS5_HOST=<console's address> PS5_ROM=<your ROM> tools/ps5/upload.sh   # the whole title folder
PS5_HOST=<console's address> tools/ps5/upload.sh eboot.bin             # only what changed
```

The port is 2121 unless `PS5_HOST` names another (`<address>:<port>`); `PS5_DIR` changes
`/data/homebrew`.

`conker.log` in the title folder is the log, new each start, crash reports included: the signal, the
address and the return addresses found from the frame pointers. Its first lines say how much memory
the console gives the title and name any function the title imports that the console does not have
(a call to one would end the game). Saves and settings are in `conker-recomp-data` there.

## Home screen

The package step writes the title's home-screen background (`sce_sys/pic0.dds` and `pic1.dds`,
3840x2160 BC7) from the project's background, `assets/background.webp`
(`tools/ps5/make_home_art.py`). `CONKER_PS5_BACKGROUND=<picture>` uses a 16:9 picture of your own
instead. The game's own art is
never used.

The console reads the title's home-screen files, music and age level once, when ShadowMount registers
the title: copies in `/user/app/PPSA92064/sce_sys/` and `/user/appmeta/PPSA92064/`, and rows in
`/system_data/priv/mms/app.db` (`tbl_contentinfo`: `snd0Info`, `pic0Info`, `pprAgeLevel`) and
`appinfo.db` (`tbl_appinfo` keys `AGE_LEVEL`, `_path_pic0_info`, `_path_pic0_info_time_stamp`,
`_path_snd0_info_time_stamp`). The shell rebuilds the `app.db` row from `appinfo.db` when the game
exits, so both must agree. A title registered before these files existed keeps empty
fields: register it again, or fill them in and restart the console. `tools/ps5/upload.sh` keeps the
`/user/appmeta` copy in step.

The age level (`ageLevel` in `param.json`) follows the original game's ratings: 17 in the US and Canada
(ESRB M), 15 in Australia (MA15+), 18 elsewhere (PEGI 18).

The home screen's music is the port's own: `tools/ps5/make_home_music.py` renders it (a low
drone and wind, soft rain and drips, two distant rolls of thunder; 42 s, 48 kHz stereo
WAV; needs NumPy). The console wants it as `sce_sys/snd0.at9`, 48 kHz stereo ATRAC9, and no
open-source encoder exists for that. The encoded file is in `tools/ps5/sce_sys/snd0.at9` (Sony's at9tool,
`-e -br 144 -wholeloop`: the console wants the loop chunk), and every package includes it;
`CONKER_PS5_SND0=<file.at9>` uses another. Never put music made from your ROM in the repository or a release.

## What differs on the console

| | |
| --- | --- |
| Memory | N64 memory is a kernel reservation backed by direct memory (`tools/patches/n64modernruntime-ps5.patch`); the RDRAM aliases are one direct allocation mapped three times (`src/runtime/memory.cpp`). |
| Display | plume presents through `VK_KHR_display` (`tools/patches/plume-ps5.patch`); SDL uses its offscreen video driver. |
| Shaders | RT64 compiles its ubershader pipelines on the calling thread: a thread that compiled with RADV crashes as it ends (`tools/patches/rt64-ps5.patch`). |
| Files | No working directory: relative paths are resolved against one the title keeps, starting at the title folder (`tools/ps5/ps5_paths.c`). No file dialog: `nfd` is a stub (`tools/ps5/nfd_stub.c`) and the ROM is found in the title folder. |
| Linking | RADV is linked into the title (PS5_Vulkan's recipe, `tools/ps5/link-title.sh`), with C library functions the SDK lacks (`tools/ps5/ps5_libc.c`) and a table of every import, so the start-up can name the missing ones. |
| DualSense | Adaptive triggers through `scePadSetTriggerEffect`; haptics and rumble synthesised as sound on the pad's vibration port (`src/platform/dualsense_ps5.cpp`). The `ps5.` lines of `feel.cfg` shape them: pitch, punch, the thud's roughness and length. |
| Debug menu | The console keeps the Create button for itself: L3 and R3 pressed together open the debug menu. |
| Players | The console lists every signed-in user as a controller: one whose pad is off holds no port, so players follow the order pads are switched on. |
| Not on the console | Mods, router port mapping. |
