# Contributing

Thanks for wanting to help. Bug reports, tests on hardware we do not have, fixes and new
enhancements are all welcome. This page says how the project works, so your time is not spent on
something that cannot be merged.

## What fits

The aim is the game as Rare made it, running natively: the same look, sound and feel. Changes fit
when they:

* fix something that differs from the original (a console or an accurate emulator is the
  reference: some oddities are Rare's own, and stay)
* make it run better on PC hardware, or on more of it
* add a quality-of-life enhancement that keeps to the original style. Every enhancement can be
  switched off (the debug menu's ENHANCEMENTS page, `src/enhancements/switches.hpp`) and is
  described in [docs/enhancements.md](docs/enhancements.md).

Bigger changes (a new enhancement, a new platform, anything touching online play) are best talked
over in an issue first.

Questions about the decompilation itself belong in
[conkers-bfd-decomp](https://github.com/DevOldSchool/conkers-bfd-decomp).

## No ROM data, ever

The repository holds no ROM and nothing made from one: no ROM, no recompiled code (`build/us`), no
extracted textures, models, music or text. `.gitignore` keeps the usual files out and
`tools/check_tree.py`, which CI runs on every pull request, refuses ROMs. A pull request or issue
that carries ROM data is closed. Screenshots and short videos of the game are fine.

Keep personal details out too: no home folder paths (`/Users/<you>`), names, addresses or join
codes in code, logs or commit messages. `tools/check_tree.py` checks for the first of these.

## Setting up

Follow the build guide for your system: [macOS](docs/building-macos.md),
[Windows](docs/building-windows.md) or [Linux](docs/building-linux.md). You need your own US ROM and
the decomp checked out next to this repository. [docs/recompilation.md](docs/recompilation.md)
explains how the pieces fit together.

## Making a change

* **Hooks into the game** are `[[patches.hook]]` entries in `config/us.toml` that call our C++.
  Run `tools/regenerate.sh` after changing `config/us.toml`: the build refuses recompiled code
  from an older config, but a stale build directory can still hide a missing hook.
* **Changes to a submodule** (N64Recomp, N64ModernRuntime, RT64) go in a patch in
  `tools/patches`, applied by `tools/apply_patches.sh`. Do not commit inside a submodule.
* **Game memory** is the game's own: an enhancement that writes to it changes what online
  machines must agree on frame by frame. Anything that does must give the same result on every
  machine and in every run (no clocks, no host-specific values); see
  [docs/online-play.md](docs/online-play.md) and [docs/diagnostics.md](docs/diagnostics.md) for the
  state hashes that catch the difference.
* **Comments say why**, with the game function or address they are about (`func_15015E80`,
  `D_800BE9C8`), so the next person can find it in the decomp.
* **Docs**: a new enhancement, switch, environment variable or tool gets its lines in the README or
  in `docs/`.

## Testing

* Play the part of the game you changed, on every platform you can.
* `tools/regress.py` runs the regression cases in `tests/regress` on the headless build (see
  [tests/regress/README.md](tests/regress/README.md)). Cases that need no debug report run
  anywhere; the others need reports that are kept private, so a maintainer runs those, and CI runs
  them all on `main`. A fix that has a case gets a `test_off`
  name, so the case can show it sees the bug.
* The checks CI runs on a pull request run locally too:

  ```sh
  python3 tools/check_tree.py
  python3 tools/check_links.py
  ruff check tools
  shellcheck --severity=warning tools/*.sh
  ```

## Pull requests

* One change a pull request, on a branch of your fork.
* Say what changed, why, and how you tested it (which platforms, which part of the game). The
  pull request template asks for these.
* Commit messages start with the area they touch, as the history does: `Widescreen: ...`,
  `Online: ...`, `CI: ...`.
* Code written with an AI assistant is welcome on the same terms as the rest: you have read it,
  tested it in the game, and can answer for it.

## Licence

Code you contribute to this project (`src`, `tools`, `config`, the docs) is under the
[MIT license](LICENSE). A patch in `tools/patches` is under the license of the project it patches.

## Security

Problems that could hurt players (online play, mods, files the game reads) are reported privately:
see [SECURITY.md](SECURITY.md).
