# Conker's Bad Fur Day: Recompiled for PS5

The PS5 port of [Conker's Bad Fur Day: Recompiled](https://github.com/DevOldSchool/conkers-bfd-recomp),
a faithful native port of *Conker's Bad Fur Day* (Nintendo 64, US), as a homebrew title for a PS5
that runs homebrew. It is the same game and the same code as the desktop builds, with the console's
own parts on top: RT64 renders through Mesa's RADV driver on the PS5's GPU, the DualSense's adaptive
triggers and haptics follow the game's weapons, and MULTI can be played online against the desktop
builds.

It is a static recompilation built with [N64Recomp](https://github.com/N64Recomp/N64Recomp) and
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime). This repository tracks the main
project and changes it only where the console needs it; what the game does, its enhancements and
its online play are described there.

This repository contains no ROM data. You need your own US ROM
(SHA-1 `4cbadd3c4e0729dec46af64ad018050eada4f47a`).

## Status

Tested on a console on firmware 13.60: the story and MULTI, local and online with the desktop builds.
It is a homebrew title: it needs a console that runs homebrew.

## Installing

1. Download `Conkers-Bad-Fur-Day-Recompiled-PS5-<version>.zip` from the releases and unzip it: it
   holds the title folder, `PPSA92064`.
2. Copy `PPSA92064` to `/data/homebrew/` on the console (FTP).
3. Put your own US ROM in that folder, as a `.z64` file (lower case). On the
   first start the game keeps its own copy; the file you added can then be removed.
4. Register the title with ShadowMountPlus or etaHEN (ShadowMountPlus registers a new folder by
   itself) and start it from the home screen.

Saves and settings are in `conker-recomp-data` in the title folder, and `conker.log` there is the log.

## On the console

* **Debug menu:** the console keeps the Create button for itself, so L3 and R3 pressed together open
  the debug menu (Back on a desktop pad).
* **DualSense:** adaptive triggers and haptics for the weapons, shaped by `feel.cfg` in the data
  folder (its `ps5.` lines tune the haptics).
* **Players:** the console lists every signed-in user as a controller; one whose pad is off holds no
  port, so players follow the order pads are switched on.
* **Online:** MULTI online works with the desktop builds of the same version. Router port mapping is
  not on the console: a host there needs its port forwarded by hand.
* **Not on the console:** mods.

## Enhancements

The same as the desktop builds: widescreen, higher resolutions, sharper text and menus, skippable
cutscenes and the rest, listed in [docs/enhancements.md](docs/enhancements.md).

## Building

[docs/building-ps5.md](docs/building-ps5.md) says how: Docker (Linux, or macOS on Apple silicon) and
the code recompiled from your ROM by `tools/regenerate.sh`, then `tools/ps5/build.sh`. CI
(`.github/workflows/ps5.yml`) builds the title when started by hand, and a version tag (`00.001.000`,
the title's `contentVersion`) builds it and makes a draft release.

The home screen's background is the project's own picture (`assets/background.webp`) and its music
is synthesised by `tools/ps5/make_home_music.py`; the game's own art and music are never used.

## Updating from the main project

This repository is the main project with the PS5 port on top, and takes in its changes by merging
it ([docs/building-ps5.md](docs/building-ps5.md#updating-from-the-main-project)).

## AI disclosure

This project is developed with the help of AI. Much of the code, the tools and the documentation is
written by an AI coding assistant. The direction and the quality assurance are mine: I decide what is
worked on and how it should behave, test the changes in the game and on the console, and review them
before they are merged.

The PS5 port's platform code was written for this project, not taken from another port. The
home screen's background, `assets/background.webp`, was made with AI image generation for this
project; it is a new picture, not taken from the game.

## License

The code written for this project (`src`, `tools`, `config` and the docs) is under the MIT license;
see [LICENSE](LICENSE). Everything else keeps its own license. The title links GPL-3.0 code
(N64ModernRuntime, and Mihawk's [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) and
[PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK)) and ships `libc.prx` (GPL-3.0-or-later),
so a built title is passed on under the GPL-3.0 as a whole and has to come with its source. Mesa's
RADV ([PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa)) is MIT, PacBrew's SDL2 and zlib are zlib,
and the main project's [License](https://github.com/DevOldSchool/conkers-bfd-recomp#license) lists the
rest.

Every title carries these licenses in its `licenses` folder: the notices of every library in it and
a `SOURCE.txt` naming the exact commit and dependency revisions it was built from
(`tools/ps5/package_notices.py`). Each release attaches the complete source, submodules included,
and the source of the PS5 libraries at the revisions the build pins.

*Conker's Bad Fur Day* and its code, art and sound belong to Rare and Microsoft. This project is not
affiliated with or endorsed by them, contains none of the game's data, and needs your own
ROM.
