#!/usr/bin/env python3
"""Game regression cases: headless runs of the null build that check our fixes still hold.

Each case is a JSON file in tests/regress/cases/. It starts the game from boot or from a debug report
(a pause save: CONKER_SAVE_CHECK with CONKER_SAVE_CHECK_PLAY=1), plays scripted input with
CONKER_FIXED_STEP=1 (so the run depends on the input only) at CONKER_TEST_SPEED times real time,
stops at a frame (CONKER_TEST_EXIT_AT) and checks what it expects: the run got there (no crash, no
hang), values in game memory at given frames (CONKER_STATE_DUMP_FRAMES), lines in the log, or that
two runs of the same input left the same memory. A case may also say how to run the game without
its fix ("without_fix"): --without-fix runs that and the case must then FAIL, which shows the case
can see the bug at all.

The reports cases start from are pause saves, not ROM data, but personal play: they are kept out of
this repository, in the folder named by CONKER_REGRESS_DATA (default: regress/ in the private
conkers-bfd-assets checkout next to this one), as reports/<name>/ (save.bin, save.resume,
report.json), the name the game gave the report (scene<N>-<date>-<time>, which it checks). `add-report`
copies one there.

    tools/regress.py [run] [cases...] [--jobs 4] [--without-fix] [--keep]
    tools/regress.py list
    tools/regress.py add-report <debug report folder>

A case file:

    {
      "description": "what it checks",
      "covers": ["src/fixes/...", "PR #..."],          # what breaks if this fails
      "start": {"report": "scene6-20261024-231154"},               # or {"boot": true} (CONKER_SKIP_INTRO=1)
      "frames": 900,                                   # frames to run after the start
      "env": {"CONKER_WIDESCREEN_CULLING": "1"},
      "switches": {"WIDESCREEN": 1},                   # debug-menu switches, by label
      "input": [{"at": 30, "hold": 8, "buttons": "A"},  # frames after the start
                {"at": 60, "hold": 90, "stick": [0, 80]}],
      "expect": [
        {"at": 900, "value": "scene", "eq": 6},
        {"at": 600, "value": "f32:[0x80088720]+0x18", "min": -3000, "max": -2000},
        {"log": "resumed scene state"},
        {"no_log": "heap check: .*corrupt"},
        {"deterministic": true, "every": 30, "ignore": ["0x80030000-0x80040000"]}
      ],
      "without_fix": {"env": {"CONKER_TEST_OFF": "sky_vertices"}}
    }

"ends": {"log": "regex"} instead says the game must stop on its own before the last frame, with such a
line (a self-test of a guard that stops the game). "matrix": [{"name": ..., "env": {...}, ...}] makes the
file one case per entry (<file>/<name>), each entry's env merged into the case's and its other keys
(start, expect, ends, input, frames) given in place of the case's.

{"delta": "camera.pitch", "from": 30, "to": 90, "min": 10}: the value changed by at least (or "max": at
most) that much between the two frames.
Input: each press holds "buttons", "stick" (left, about +-80), "right" (+-127, +y up) and "mouse" (pixels a
frame, +y down) from "at" for "hold" frames. "port_flags": ["invert_aim"] sets controller 1's INVERT AIM.
{"at": N, "actor_link": [162, 163]}: each character of model 162 keeps to (actor +0x65) one of model 163.
Values: "u8:", "s8:", "u16:", "s16:", "u32:", "s32:", "f32:" and an address, or "[pointer]+offset" to
read a pointer first; or a name from NAMES. Results go to build/regress/<case>/ and a summary to
build/regress/summary.md.
"""
import argparse
import concurrent.futures
import copy
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CASES = ROOT / "tests/regress/cases"
OUT = ROOT / "build/regress"
DEFAULT_BINARY = ROOT / "build/host/ConkersBadFurDayRecompiled"
SPEED = 8
TIMEOUT_MARGIN = 60  # seconds over the run's own length at SPEED before a run counts as hung
START_LOCK = threading.Lock()

BUTTONS = {
    "A": 0x8000, "B": 0x4000, "Z": 0x2000, "START": 0x1000,
    "DUP": 0x0800, "DDOWN": 0x0400, "DLEFT": 0x0200, "DRIGHT": 0x0100,
    "L": 0x0020, "R": 0x0010, "CUP": 0x0008, "CDOWN": 0x0004, "CLEFT": 0x0002, "CRIGHT": 0x0001,
}
PORT_CONNECTED = 0x01
PORT_RIGHT_STICK = 0x02  # frame_input::Port::kRightStick
PORT_FLAGS = {"invert_aim": 0x08}  # "port_flags": controller 1's settings that go with its record

# Game memory worth naming (src/enhancements/pause_save/resume_point.cpp).
NAMES = {
    "scene": "u32:0x800BE9F0",
    "conker.x": "f32:[0x80088720]+0x14",
    "conker.y": "f32:[0x80088720]+0x18",
    "conker.z": "f32:[0x80088720]+0x1C",
    # Conker's camera block (*(object + 0x31C)): forward vector +0x130, eye +0x13C, look-at +0x148.
    "camera.fx": "f32:[[0x80088720]+0x31C]+0x130",
    "camera.fy": "f32:[[0x80088720]+0x31C]+0x134",
    "camera.fz": "f32:[[0x80088720]+0x31C]+0x138",
    # Conker's player block (*(actor 0 + 0x31C)): item held +0x78, sight +0x197, R aim +0x198,
    # a mounted gun's pitch f32 +0x170 (src/enhancements/consistent_controls).
    "player.item": "u8:[0x800CC5EC]+0x78",
    "player.sight": "u8:[0x800CC5EC]+0x197",
    "player.aim": "u8:[0x800CC5EC]+0x198",
    "gun.pitch": "f32:[0x800CC5EC]+0x170",
}
# Values worked out from others: degrees, the view's yaw (0 along +z) and pitch (up is +).
COMPUTED = {
    "camera.yaw": lambda d: math.degrees(math.atan2(d.read("camera.fx"), d.read("camera.fz"))),
    "camera.pitch": lambda d: math.degrees(math.asin(max(-1.0, min(1.0, d.read("camera.fy"))))),
    "conker.facing": lambda d: d.read("f32:[0x80088720]+0x40"),
}
ACTORS, ACTOR_SIZE, ACTOR_COUNT = 0x800CC2D0, 0x32C, 25  # characters: model byte +0x4, link +0x65
RDRAM_BASE = 0x80000000
DUMP_SIZE = 0x400000  # CONKER_STATE_DUMP_FRAMES writes the first 4 MiB


def git_common_parent():
    """The folder holding the main checkout (worktrees share its git folder)."""
    try:
        common = subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"], cwd=ROOT,
                                capture_output=True, text=True, check=True).stdout.strip()
        return Path(common).parent.parent
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ROOT.parent


def data_dir():
    if os.environ.get("CONKER_REGRESS_DATA"):
        return Path(os.environ["CONKER_REGRESS_DATA"])
    return git_common_parent() / "conkers-bfd-assets/regress"


def default_rom():
    for candidate in (os.environ.get("CONKER_ROM"), git_common_parent() / "conkers-bfd-assets/baserom.us.z64",
                      git_common_parent() / "conkers-bfd-decomp/roms/baserom.us.z64"):
        if candidate and Path(candidate).exists():
            return Path(candidate)
    return None


# --- memory -------------------------------------------------------------------------------------


class Dump:
    """A CONKER_STATE_DUMP_FRAMES file: the first 4 MiB, each 32-bit word in host (little-endian) order."""

    def __init__(self, path):
        self.data = path.read_bytes()

    def bytes_at(self, address, size):
        offset = (address - RDRAM_BASE) & 0x1FFFFFFF
        if offset + size > len(self.data):
            raise ValueError(f"0x{address:08X} is outside the dumped 4 MiB")
        # Word-swapped: byte n of the console's big-endian memory is at n ^ 3.
        return bytes(self.data[(offset + i) ^ 3] for i in range(size))

    def address(self, where):
        """0x8xxxxxxx, or [where]+offset: the pointer at `where`, plus the offset (None if it is not set)."""
        where = where.strip()
        if not where.startswith("["):
            return int(where, 16)
        depth, i = 0, len(where)
        for i, c in enumerate(where):
            depth += {"[": 1, "]": -1}.get(c, 0)
            if depth == 0:
                break
        inner = self.address(where[1:i])
        if inner is None:
            return None
        pointer = struct.unpack(">I", self.bytes_at(inner, 4))[0]
        if pointer & 0xFF000000 != 0x80000000:
            return None  # nothing there (no Conker in the scene, say)
        rest = where[i + 1:].replace(" ", "")
        return pointer + (int(rest, 0) if rest else 0)

    def read(self, spec):
        if spec in COMPUTED:
            try:
                return COMPUTED[spec](self)
            except TypeError:
                return None  # a pointer on the way is not set
        spec = NAMES.get(spec, spec)
        kind, _, where = spec.partition(":")
        if kind not in ("u8", "s8", "u16", "s16", "u32", "s32", "f32"):
            raise ValueError(f"cannot read {spec!r}")
        try:
            address = self.address(where)
        except ValueError:
            raise ValueError(f"cannot read {spec!r}") from None
        if address is None:
            return None
        size = {"u8": 1, "s8": 1, "u16": 2, "s16": 2}.get(kind, 4)
        raw = self.bytes_at(address, size)
        return struct.unpack(">" + {"u8": "B", "s8": "b", "u16": "H", "s16": "h", "u32": "I", "s32": "i",
                                    "f32": "f"}[kind], raw)[0]


# --- input --------------------------------------------------------------------------------------


def replay_text(case, start, end, switches):
    """A CONKER_FRAME_INPUT_REPLAY log for frames 1..end, with the case's presses from `start`. Each
    press holds its buttons, left stick ("stick", about +-80), right stick ("right", +-127, +y up) and
    mouse motion ("mouse", pixels a frame, +y down; the mouse counts as captured) for `hold` frames.
    Controller 1 is a pad with a right stick, as the live input records one."""
    buttons = [0] * (end + 1)
    sticks = [(0, 0)] * (end + 1)
    rights = [(0, 0)] * (end + 1)
    mice = [None] * (end + 1)
    for press in case.get("input", []):
        mask = 0
        for name in re.split(r"[+ ,]+", press.get("buttons", "").upper()):
            if name:
                mask |= BUTTONS[name]
        for frame in range(start + press["at"], min(end, start + press["at"] + press.get("hold", 1) - 1) + 1):
            buttons[frame] |= mask
            if "stick" in press:
                sticks[frame] = tuple(press["stick"])
            if "right" in press:
                rights[frame] = tuple(press["right"])
            if "mouse" in press:
                mice[frame] = tuple(press["mouse"])
    flags = PORT_CONNECTED | PORT_RIGHT_STICK
    for name in case.get("port_flags", []):
        flags |= PORT_FLAGS[name]
    lines = ["# conker-replay 1 build=regress fixed_step=1"]
    lines += [f"# switch {label}={1 if on else 0}" for label, on in switches.items()]
    idle = " 0:0000,0,0,0,0" * 3
    for frame in range(1, end + 1):
        x, y = sticks[frame]
        rx, ry = rights[frame]
        mouse = f" m1:{mice[frame][0]},{mice[frame][1]},0" if mice[frame] else " m0:0,0,0"
        lines.append(f"{frame} 0 {flags:x}:{buttons[frame]:04x},{x},{y},{rx},{ry}{idle}{mouse}" + " m0:0,0,0" * 3)
    return "\n".join(lines) + "\n"


# --- running ------------------------------------------------------------------------------------


class Run:
    def __init__(self, folder, code, log, seconds):
        self.folder, self.code, self.log, self.seconds = folder, code, log, seconds

    def dump(self, frame):
        path = self.folder / "dumps" / f"frame{frame}.bin"
        return Dump(path) if path.exists() else None


def start_frame_cache():
    path = OUT / "start-frames.json"
    try:
        return path, json.loads(path.read_text())
    except (OSError, ValueError):
        return path, {}


CACHE_LOCK = threading.Lock()


def run_game(binary, rom, case, folder, start, frames, variant, dump_frames=(), hash_every=0):
    """One run of the game for a case. Returns the Run; the start frame it reports is in run.start."""
    if folder.exists():
        shutil.rmtree(folder)
    (folder / "conker-recomp-data/saves").mkdir(parents=True)
    (folder / "dumps").mkdir()
    end = start + frames
    env = dict(os.environ)
    env.update({
        "CONKER_AUDIO": "0",
        "CONKER_SKIP_INTRO": "1",
        "CONKER_SAVE_SNAPSHOTS": "0",
        "CONKER_FIXED_STEP": "1",
        "CONKER_TEST_SPEED": str(SPEED),
        "CONKER_TEST_EXIT_AT": str(end),
        "CONKER_FRAME_INPUT_REPLAY": str(folder / "input.log"),
    })
    env.update({k: str(v) for k, v in case.get("env", {}).items()})
    switches = dict(case.get("switches", {}))
    if variant:
        env.update({k: str(v) for k, v in variant.get("env", {}).items()})
        switches.update(variant.get("switches", {}))
    report = case.get("start", {}).get("report")
    if report:
        env["CONKER_SAVE_CHECK"] = str(data_dir() / "reports" / report)
        env["CONKER_SAVE_CHECK_PLAY"] = "1"
    if dump_frames:
        env["CONKER_STATE_DUMP_FRAMES"] = ",".join(str(f) for f in sorted(set(dump_frames)))
        env["CONKER_STATE_DUMP_DIR"] = str(folder / "dumps")
    if hash_every:
        env["CONKER_STATE_HASH_LOG"] = str(folder / "hashes.log")
        env["CONKER_STATE_HASH_EVERY"] = str(hash_every)
    (folder / "input.log").write_text(replay_text(case, start, end, switches))
    (folder / "env.txt").write_text("".join(f"{k}={v}\n" for k, v in sorted(env.items()) if k.startswith("CONKER_")))
    timeout = end / 30 / SPEED + TIMEOUT_MARGIN + (60 if report else 0)
    started = time.time()
    with open(folder / "run.log", "w") as log:
        with START_LOCK:  # the game copies the ROM into its data folder as it starts; two at once race
            proc = subprocess.Popen([str(binary), str(rom)], cwd=folder, env=env, stdout=log, stderr=subprocess.STDOUT)
            time.sleep(1.5)
        try:
            code = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            code = "hang"
    for rom_copy in (folder / "conker-recomp-data").glob("*.z64"):
        rom_copy.unlink()
    text = (folder / "run.log").read_text(errors="replace")
    run = Run(folder, code, text, round(time.time() - started, 1))
    found = re.search(r"save check: restored at frame (\d+)", text)
    run.start = int(found.group(1)) if found else (0 if not report else None)
    return run


def run_case(binary, rom, name, case, variant=None, keep=False):
    """Runs a case (or its without_fix variant); returns (passed, [problems], seconds, started), where
    started is False when the run never got to the case's start (no report, report not restored)."""
    folder = OUT / (name + ("-without-fix" if variant else ""))
    frames = case["frames"]
    expects = case.get("expect", [])
    dump_frames = [e["at"] for e in expects if "value" in e or "actor_link" in e]
    dump_frames += [f for e in expects if "delta" in e for f in (e["from"], e["to"])]
    deterministic = next((e for e in expects if e.get("deterministic")), None)
    hash_every = deterministic.get("every", 30) if deterministic else 0
    report = case.get("start", {}).get("report")
    if report and not (data_dir() / "reports" / report / "save.bin").exists():
        return False, [f"report {report} is not in {data_dir() / 'reports'} (tools/regress.py add-report)"], 0, False

    cache_path, cache = start_frame_cache()
    key = f"{report}" if report else "boot"
    # A report is restored some 300 frames in (the save check loads it at frame 300); the frame it was
    # last restored at is kept in build/regress/start-frames.json, and the run is made again from the
    # frame it reports when that has moved.
    start = cache.get(key, 312) if report else 0
    seconds = 0
    for attempt in range(4):
        run = run_game(binary, rom, case, folder, start, frames, variant, [start + f for f in dump_frames], hash_every)
        seconds += run.seconds
        if not report or run.start == start:
            break
        if run.start is None:
            if "test exit at frame" not in run.log:
                break  # not restored, and not because the run ended first
            start += 300
            continue
        start = run.start
        with CACHE_LOCK:
            cache_path, cache = start_frame_cache()
            cache[key] = start
            OUT.mkdir(parents=True, exist_ok=True)
            cache_path.write_text(json.dumps(cache, indent=1, sort_keys=True) + "\n")

    problems = []
    ends = case.get("ends")
    if report and run.start is None:
        problems.append("the report was not restored" + tail(run.log))
    elif ends:
        # The game must stop on its own before the last frame, saying why.
        if "test exit at frame" in run.log or run.code == "hang":
            problems.append(f"the game did not stop (expected a line matching {ends['log']!r})" + tail(run.log))
        elif not re.search(ends["log"], run.log):
            problems.append(f"the game stopped ({describe_code(run.code)}) without a line matching "
                            f"{ends['log']!r}" + tail(run.log))
        problems += check_expects(case, run, start)
    elif run.code == "hang":
        problems.append(f"no frame {start + frames} before the time limit (hang?)" + tail(run.log))
    elif run.code != 0 or "test exit at frame" not in run.log:
        problems.append(f"the game ended with {describe_code(run.code)} before frame {start + frames}" + tail(run.log))
    else:
        problems += check_expects(case, run, start)
        if deterministic and not problems:
            again = run_game(binary, rom, case, folder.with_name(folder.name + "-again"), start, frames, variant, (),
                             hash_every)
            seconds += again.seconds
            problems += compare_hashes(run.folder / "hashes.log", again.folder / "hashes.log",
                                       deterministic.get("ignore", []))
            if not keep:
                shutil.rmtree(again.folder, ignore_errors=True)
    if not keep and not problems:
        shutil.rmtree(folder / "dumps", ignore_errors=True)
    started = not (report and run.start is None)
    return not problems, problems, seconds, started


def check_expects(case, run, start):
    problems = []
    for expect in case.get("expect", []):
        if "log" in expect and not re.search(expect["log"], run.log):
            problems.append(f"log has no line matching {expect['log']!r}")
        if "no_log" in expect:
            found = re.search(expect["no_log"], run.log)
            if found:
                problems.append(f"log has {found.group(0)!r}")
        if "delta" in expect:
            problems += check_delta(run, start, expect)
        if "actor_link" in expect:
            dump = run.dump(start + expect["at"])
            problems += check_actor_link(dump, expect) if dump else [f"no memory dump at start + {expect['at']}"]
        if "value" in expect:
            dump = run.dump(start + expect["at"])
            if dump is None:
                problems.append(f"no memory dump at frame {start + expect['at']} (start + {expect['at']})")
                continue
            try:
                value = dump.read(expect["value"])
            except ValueError as error:
                problems.append(str(error))
                continue
            where = f"{expect['value']} at start + {expect['at']}"
            if value is None:
                problems.append(f"{where}: the pointer is not set")
            elif "eq" in expect and value != expect["eq"]:
                problems.append(f"{where} is {fmt(value)}, expected {fmt(expect['eq'])}")
            elif "ne" in expect and value == expect["ne"]:
                problems.append(f"{where} is {fmt(value)}, expected anything else")
            elif "min" in expect and value < expect["min"]:
                problems.append(f"{where} is {fmt(value)}, below {fmt(expect['min'])}")
            elif "max" in expect and value > expect["max"]:
                problems.append(f"{where} is {fmt(value)}, above {fmt(expect['max'])}")
            elif "mask" in expect and (value & expect["mask"]) != expect.get("bits", expect["mask"]):
                problems.append(f"{where} is {fmt(value)}, bits 0x{expect['mask']:X} should be "
                                f"0x{expect.get('bits', expect['mask']):X}")
    return problems


def check_delta(run, start, expect):
    """{"delta": value, "from": a, "to": b, "min"/"max": n}: how much the value changed from frame a to
    frame b after the start (angles named *yaw or *facing taken the short way round)."""
    name = expect["delta"]
    dumps = [run.dump(start + expect["from"]), run.dump(start + expect["to"])]
    if None in dumps:
        return [f"no memory dump at start + {expect['from']} or + {expect['to']}"]
    try:
        before, after = (d.read(name) for d in dumps)
    except ValueError as error:
        return [str(error)]
    if before is None or after is None:
        return [f"{name}: a pointer is not set at start + {expect['from']} or + {expect['to']}"]
    change = after - before
    if name.endswith(("yaw", "facing")):
        change = (change + 180) % 360 - 180
    where = f"{name} from start + {expect['from']} to + {expect['to']} changed by {change:g} ({before:g} to {after:g})"
    if "min" in expect and change < expect["min"]:
        return [f"{where}, expected at least {expect['min']:g}"]
    if "max" in expect and change > expect["max"]:
        return [f"{where}, expected at most {expect['max']:g}"]
    return []


def check_actor_link(dump, expect):
    """{"actor_link": [model, to_model]}: every character of that model keeps to (+0x65, slot + 1) one of
    the other model."""
    model, to_model = expect["actor_link"]
    models = [dump.read(f"u8:0x{ACTORS + slot * ACTOR_SIZE + 4:08X}") for slot in range(ACTOR_COUNT)]
    slots = [slot for slot, m in enumerate(models) if m == model]
    if not slots:
        return [f"no character of model {model} at start + {expect['at']}"]
    problems = []
    for slot in slots:
        link = dump.read(f"u8:0x{ACTORS + slot * ACTOR_SIZE + 0x65:08X}")
        linked = models[link - 1] if 0 < link <= ACTOR_COUNT else None
        if linked != to_model:
            problems.append(f"model {model} in slot {slot} keeps to slot {link - 1} (model {linked}), "
                            f"not to model {to_model}")
    return problems


def compare_hashes(a, b, ignore):
    ranges = []
    for spec in ignore:
        low, _, high = spec.partition("-")
        ranges.append((int(low, 16), int(high, 16)))

    def load(path):
        frames = {}
        for line in path.read_text().splitlines():
            parts = line.split()
            if len(parts) > 1 and parts[0].isdigit():
                frames[int(parts[0])] = parts[1:]
        return frames

    first, second = load(a), load(b)
    for frame in sorted(set(first) & set(second)):
        for block, (x, y) in enumerate(zip(first[frame], second[frame])):
            address = RDRAM_BASE + block * 0x4000
            if x != y and not any(low <= address < high for low, high in ranges):
                return [f"two runs of the same input differ at frame {frame}, block 0x{address:08X} "
                        f"(tools/compare_state_hashes.py {a} {b})"]
    if not set(first) & set(second):
        return ["no state hashes to compare"]
    return []


def fmt(value):
    if isinstance(value, float):
        return f"{value:g}"
    return f"{value} (0x{value & 0xFFFFFFFF:X})"


def describe_code(code):
    if isinstance(code, int) and code < 0:
        try:
            import signal
            return f"signal {signal.Signals(-code).name}"
        except ValueError:
            return f"signal {-code}"
    return f"status {code}"


def tail(log, lines=6):
    kept = [line for line in log.splitlines() if line.strip()][-lines:]
    return "".join("\n      | " + line for line in kept)


# --- commands -----------------------------------------------------------------------------------


def expand(name, case):
    """A case with a "matrix" is one case per entry: each entry's name, its env merged into the case's,
    and anything else it gives (start, expect, ends, input, frames) in place of the case's."""
    if "matrix" not in case:
        return {name: case}
    cases = {}
    for entry in case["matrix"]:
        sub = copy.deepcopy({k: v for k, v in case.items() if k != "matrix"})
        for key, value in entry.items():
            if key == "env":
                sub["env"] = {**sub.get("env", {}), **value}
            elif key != "name":
                sub[key] = value
        cases[f"{name}/{entry['name']}"] = sub
    return cases


def load_cases(names):
    cases = {}
    for path in sorted(CASES.glob("*.json")):
        for name, case in expand(path.stem, json.loads(path.read_text())).items():
            if not names or path.stem in names or name in names:
                cases[name] = case
    missing = {n for n in names if n not in cases and not any(c.startswith(n + "/") for c in cases)}
    if missing:
        sys.exit(f"no such case: {', '.join(sorted(missing))} (tools/regress.py list)")
    return cases


def command_run(args):
    binary = Path(args.binary)
    rom = Path(args.rom) if args.rom else default_rom()
    if not binary.exists():
        sys.exit(f"{binary} is not built (cmake -S . -B build/host -G Ninja -DCONKER_RENDERER=null)")
    if rom is None or not rom.exists():
        sys.exit("no ROM: pass --rom or set CONKER_ROM")
    cases = load_cases(args.cases)
    jobs = []
    for name, case in cases.items():
        jobs.append((name, case, None))
        if args.without_fix and case.get("without_fix"):
            jobs.append((name, case, case["without_fix"]))
    OUT.mkdir(parents=True, exist_ok=True)
    results = []
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run_case, binary, rom, name, case, variant, args.keep): (name, variant)
                   for name, case, variant in jobs}
        for future in concurrent.futures.as_completed(futures):
            name, variant = futures[future]
            passed, problems, seconds, started = future.result()
            label = name + (" without its fix" if variant else "")
            if not started:
                ok = False
                verdict = "ERROR"
            elif variant:
                # Without the fix the case must fail: otherwise it cannot see the bug.
                ok = not passed
                verdict = "ok (fails without the fix)" if ok else "BLIND (passes without the fix)"
            else:
                ok = passed
                verdict = "PASS" if ok else "FAIL"
            failed += not ok
            shown = problems if not ok and (not variant or not started) else []
            results.append((label, verdict, seconds, shown))
            print(f"{verdict:<5} {label} ({seconds:.0f} s)")
            for problem in shown:
                print("      " + problem)
            sys.stdout.flush()
    results.sort()
    lines = ["# Regression run", "", f"{len(results) - failed} of {len(results)} ok.", "", "| case | result | s |",
             "|---|---|---|"]
    lines += [f"| {label} | {verdict} | {seconds:.0f} |" for label, verdict, seconds, _ in results]
    for label, _, _, problems in results:
        if problems:
            lines += ["", f"## {label}", ""] + [f"- {p}" for p in problems]
    (OUT / "summary.md").write_text("\n".join(lines) + "\n")
    print(f"\n{len(results) - failed} of {len(results)} ok; summary in {(OUT / 'summary.md').relative_to(ROOT)}")
    return 1 if failed else 0


def command_list(args):
    for name, case in load_cases([]).items():
        start = case.get("start", {})
        where = f"report {start['report']}" if "report" in start else "boot"
        fix = " [without_fix]" if case.get("without_fix") else ""
        print(f"{name:<32} {where:<28} {case.get('description', '')}{fix}")
    return 0


def command_add_report(args):
    source = Path(args.report)
    if source.is_file():
        source = source.parent
    if not (source / "save.bin").exists() or not (source / "save.resume").exists():
        sys.exit(f"{source} is not a debug report (no save.bin and save.resume)")
    target = data_dir() / "reports" / source.name
    if target.exists():
        sys.exit(f"{target} exists")
    target.mkdir(parents=True)
    for name in ("save.bin", "save.resume", "report.json"):
        if (source / name).exists():
            shutil.copy2(source / name, target / name)
    print(f"copied to {target}")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command")
    run = sub.add_parser("run", help="run cases (the default)")
    run.add_argument("cases", nargs="*")
    run.add_argument("--binary", default=str(DEFAULT_BINARY))
    run.add_argument("--rom")
    run.add_argument("--jobs", type=int, default=4)
    run.add_argument("--without-fix", action="store_true", help="also run each case without its fix: it must fail")
    run.add_argument("--keep", action="store_true", help="keep memory dumps of passing cases")
    sub.add_parser("list", help="list the cases")
    add = sub.add_parser("add-report", help="copy a debug report into the regression data")
    add.add_argument("report")
    argv = sys.argv[1:]
    if not argv or argv[0] not in ("run", "list", "add-report", "-h", "--help"):
        argv = ["run"] + argv
    args = parser.parse_args(argv)
    return {"run": command_run, "list": command_list, "add-report": command_add_report}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
