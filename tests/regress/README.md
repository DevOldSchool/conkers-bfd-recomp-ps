# Game regression cases

`tools/regress.py` runs the headless (null) build through the cases in `cases/` and checks that our
fixes to the game still hold. It needs the ROM, so it is run locally, not in CI.

```bash
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCONKER_RENDERER=null
cmake --build build/host
tools/regress.py --without-fix
```

The whole set takes about two minutes with 6 jobs (`--jobs`). Name cases to run only those
(`tools/regress.py scenes resume-checkpoint/scene27`); `tools/regress.py list` lists them. Each run is
in `build/regress/<case>/` (`run.log`, `env.txt`, `input.log`), and the summary is in
`build/regress/summary.md`.

## How a case runs

- It starts from boot, or from a debug report: the pause save the debug menu's SAVE DEBUG REPORT
  writes. The game loads it at frame 300 and restores the scene (`CONKER_SAVE_CHECK` with
  `CONKER_SAVE_CHECK_PLAY=1`). Frames in a case count from the frame the restore finished.
- Input is a replay (`CONKER_FRAME_INPUT_REPLAY`) made from the case's presses, with
  `CONKER_FIXED_STEP=1`, so the run depends on the input only. `CONKER_TEST_SPEED=8` runs it at 8 times
  real time, which the fixed step makes the same run.
- `CONKER_TEST_EXIT_AT` ends it at the case's last frame. A run that ends earlier crashed; one that
  does not end in time hung.
- Values are read from the game's memory at given frames (`CONKER_STATE_DUMP_FRAMES`). Log lines
  are matched. With `"deterministic"` the case is run twice and the state hashes must agree.
- The null build has no window, so `CONKER_TEST_ASPECT=16:9` gives the game a widescreen picture to
  work for (the culling and the sky depend on it).

## Showing a case can see its bug

A case with `"without_fix"` names how to run the game without the fix, usually
`CONKER_TEST_OFF=<name>` (`src/runtime/test_off.hpp`). With `--without-fix` that run must FAIL: a
case that still passes is reported as BLIND. A new fix that has a case should get a `test_off` name
for this, at the one place that makes the fix:

| name | turns off |
|---|---|
| `sky_vertices` | the larger sky vertex buffers and the tile check (reports `sky buffer overrun` instead) |
| `resume_camera_distance` | the saved camera distance put back on resume |
| `resume_checkpoint` | the saved checkpoint (scene, entrance) put back on resume |
| `resume_actor_link` | the actor link (+0x65) mapped to the restored slots |

## The reports

Reports are pause saves from play, so they stay out of this repository: they are in `regress/reports/`
of the private conkers-bfd-assets checkout next to this one (or the folder `CONKER_REGRESS_DATA`
names). `tools/regress.py add-report <report folder>` copies one there. The game checks a report's
folder name (`scene<N>-<date>-<time>`), so the copy keeps it.

## Adding a case

Write `cases/<name>.json` (the format is at the top of `tools/regress.py`). Pick the frames and
values from the run that shows the bug, then check both ways:

```bash
tools/regress.py <name> --without-fix --keep
```

`--keep` keeps the memory dumps, which `tools/regress.py`'s `Dump` class reads:
`python3 -c "import sys; sys.path.insert(0, 'tools'); import regress; print(regress.Dump(path).read('scene'))"`.

## What is covered

| case | checks |
|---|---|
| `boot-title` | boot to the title (scene 29) with the intro skipped; two runs of the same input match |
| `runtime-guards/*` | each untrusted-memory guard stops the game; the snapshot self-test refuses tampered snapshots |
| `scenes/*` | 36 scenes load from a report and play 30 s in 16:9 with no crash, hang, heap damage or sky overrun |
| `sky-vertex-overflow` | scene 6 in 16:9, camera turning: the sky stays inside its buffers (308 of 304 without the fix) |
| `resume-camera-distance` | scene 6's saved camera distance (901) comes back, not the zone's 530 |
| `resume-checkpoint/*` | the save's checkpoint survives a resume in scenes 25, 27 and 65 |
| `resume-actor-link/*` | scene 60's giant bat keeps to Batula's perch after a resume |

Not covered yet: fixes in the RT64 renderer and its patches (they need the GPU build and
screenshots), and drawing fixes that the null build only shows as display lists (level
visibility, seams, the Heist alcove, the weapon rack, shadows). `tools/save_check.py` still checks
the save/load restore over the whole report corpus, and `tools/net-test` the online play.
