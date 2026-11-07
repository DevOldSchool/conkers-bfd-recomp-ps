# Online play test harness

Scripts that run online MULTI sessions on one Mac, unattended, to check that
every machine stays in step. macOS only (zsh, `screencapture`, and a small
Swift helper, `winid.swift`, that finds the game's windows). Each game runs in
its own folder under `$NET_TEST_DIR` (default `/tmp/conker-net-test`), windowed
and without sound, and logs to its `logs/` folder.

Set first (`env.sh` reads them):

* `CONKER_ROM`: your US ROM.
* `NET_TEST_SAVE`: an EEPROM save with MULTI open (`conker_us.bin` from a game
  past the first chapters). The host plays on a copy of it.
* `BIN` (optional): the game, by default `build/rt64/ConkersBadFurDayRecompiled`.

| Script | What it does |
|---|---|
| `runsim.sh NAME SECS PORT` | A session of 2 or 3 players (`PLAYERS`), each playing a pad script (`P1`, `P2`, `P3`; `f1.pad` to `f3.pad` by default). `HSIM`/`GSIM` add simulated delay and loss (`CONKER_NET_SIM`), `HENV`/`GENV` set other variables. |
| `check.py NAME` | After `runsim.sh`: compares the players' state hashes and input records frame by frame, and sums how long each one waited. |
| `rejoinrun.sh NAME PORT SECS [KILL_AT REJOIN_AT]` | `runsim.sh` with the guest killed mid-match and a new one joining in its place; `FAULT=term` or `FAULT=stop` for a guest that quits or freezes. |
| `lrun.sh NAME SECS` | Two lobbies driven by `NAME.h.script` and `NAME.g.script` (`CONKER_LOBBY_SCRIPT`), with screenshots; port mapping goes to `tools/fake_gateway.py`, never a real router. `pn` is an example (each lobby shows the other's pad by model, its own by name). |
| `humanpad.py` | Makes `hu1.pad` to `hu3.pad` from `f1.pad` to `f3.pad`, with sticks that move every frame as a person's do, for measuring traffic (`CONKER_NET_STATS`). |

A pad script is one line per press: the frame it starts on, how many frames it
lasts, and the button or stick (see `f1.pad`). The switches these runs use are in
[docs/diagnostics.md](../../docs/diagnostics.md). To compare two players'
desync files from a real session, use `tools/compare_desync.py`.
