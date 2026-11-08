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

`CONKER_GENERATED=<dir>` uses another checkout's `build/us`. The dependencies and the build folder are
Docker volumes (`conker-ps5-deps`, `conker-ps5-build`). The title folder is `dist/PPSA21064`.

## Updating from the main project

This repository is the main project ([conkers-bfd-recomp](https://github.com/DevOldSchool/conkers-bfd-recomp))
with the PS5 port on top, and tracks it as the remote `upstream`. To take in its latest changes:

```sh
git fetch upstream
git merge upstream/main
git submodule update --init --recursive
```

The PS5 port changes the main project's files only in a few places, each behind `CONKER_PS5` (or
`if(PS5)` in CMake), so merges rarely conflict. A fresh clone adds the remote once:
`git remote add upstream git@github.com:DevOldSchool/conkers-bfd-recomp.git`.

## Install

1. Copy `dist/PPSA21064` to `/data/homebrew/` on the console (FTP).
2. Put your own US ROM in that folder, with a lower-case `.z64`, `.n64` or `.v64` extension. On the first
   start the game keeps its own copy; the file you added can then be removed.
3. Register the title (ShadowMountPlus or etaHEN) and start it from the home screen.

`conker.log` in the title folder is the log, crash reports included. Saves and settings are in
`conker-recomp-data` there.

## Home screen

The package step writes the title's home-screen background (`sce_sys/pic0.dds` and `pic1.dds`,
3840x2160 BC7) from the port's own art, `assets/app-icon.png` (`tools/ps5/make_home_art.py`).
`CONKER_PS5_BACKGROUND=<picture>` uses a 16:9 picture of your own instead. The game's own art is
never used.

The console reads the title's home-screen files, music and age level once, when ShadowMount registers
the title: copies in `/user/app/PPSA21064/sce_sys/` and `/user/appmeta/PPSA21064/`, and rows in
`/system_data/priv/mms/app.db` (`tbl_contentinfo`: `snd0Info`, `pic0Info`, `pprAgeLevel`) and
`appinfo.db` (`tbl_appinfo` keys `AGE_LEVEL`, `_path_pic0_info`, `_path_pic0_info_time_stamp`,
`_path_snd0_info_time_stamp`). The shell rebuilds the `app.db` row from `appinfo.db` when the game
exits, so both must agree. A title registered before these files existed keeps empty
fields: register it again, or fill them in and restart the console. `tools/ps5/upload.sh` keeps the
`/user/appmeta` copy in step.

The age level (`ageLevel` in `param.json`) follows the original game's ratings: 17 in the US and Canada
(ESRB M), 15 in Australia (MA15+), 18 elsewhere (PEGI 18).

The home screen's music is the port's own: `tools/ps5/make_home_music.py` composes and renders it (dark
ambient pads and a low drone, soft rain and drips, two distant rolls of thunder; 42 s, 48 kHz stereo
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
| Files | No working directory: relative paths are resolved against the title folder (`tools/ps5/ps5_paths.c`). No file dialog: the ROM is found in the title folder. |
| DualSense | Adaptive triggers through `scePadSetTriggerEffect`; haptics and rumble synthesised as sound on the pad's vibration port (`src/platform/dualsense_ps5.cpp`). The `ps5.` lines of `feel.cfg` shape them: pitch, punch, the thud's roughness and length. |
| Players | The console lists every signed-in user as a controller: one whose pad is off holds no port, so players follow the order pads are switched on. |
| Not on the console | Mods, router port mapping. |
