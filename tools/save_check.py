#!/usr/bin/env python3
"""Checks the pause-menu save/load restore over the debug reports.

For each report (a folder in conker-recomp-data/debug_reports/ with a save.bin), the null build is
started in a folder of its own with CONKER_SAVE_CHECK (src/enhancements/pause_save/save_check.cpp):
it loads the report, and once the restore has put the scene back it writes the scene as it is then
(capture-0.resume), again after each of the --at times, and a debug report, a save made by the game
from the restored scene. Then each capture is compared with the report's own save.resume:

  lost      a word the restore did not put back (differs in the restore's own frame), not an
            address, grouped by table and offset, so the same lost field across many reports
            stands out;
  undone    a word that was put back but the game had changed by the first later capture and
            left alone after that: a restore the game overrides;
  missing   a saved character (model, id) the restored scene does not have, or one it has extra;
  scripts   a saved event script not running, or running with other variables;
  swarms    a saved swarm not there, or elsewhere;
  save      the save the game wrote from the restored scene, against the one loaded: the slot's
            record must be the same but for its sequence number and checksum.

Usage:
  tools/save_check.py [--binary build/host/ConkersBadFurDayRecompiled] [--rom <conker-us.z64>] [--out <dir>]
                      [--jobs 6] [--at 0,6,30] [--no-run] [--repeat N] [reports...]
Reports are folders or names in debug_reports; none means all of them. --no-run only compares
what an earlier run left in --out. The summary goes to <out>/summary.md, the details to
<out>/details.json.
"""
import argparse
import concurrent.futures
import json
import os
import shutil
import struct
import subprocess
import time
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ROM = Path.home() / "Development/decompilation/conkers-bfd-decomp/roms/baserom.us.z64"
REPORTS = ROOT / "conker-recomp-data/debug_reports"

ACTOR_SIZE = 0x32C
ACTOR_MODEL = 0x4
ACTOR_ID = 0x3A
ACTOR_LINK = 0x65
ACTOR_PLACEMENT = 0x13F
ACTOR_POSITION = 0x14
OBJECT_STATE = 0x73
OBJECT_INDEX = 0x71
TRIGGER_FIRED = 0x14
EEPROM_RECORDS = 4
EEPROM_RECORD_BLOCK = 4
EEPROM_RECORD_STRIDE = 16
EEPROM_RECORD_SIZE = 0x80
SLOTS = 3


def is_address(word):
    return 0x80000400 <= word < 0x80800000


def restorable(then, now):
    return not is_address(then) and not (then == 0 and is_address(now))


def words(data):
    return struct.unpack(">%dI" % (len(data) // 4), data[: len(data) // 4 * 4])


def f32(data, offset):
    return struct.unpack(">f", data[offset : offset + 4])[0]


# --- the files -------------------------------------------------------------------------------


def load_points(path):
    """The first slot's point of a points file, with the tables as bytes by name."""
    with open(path) as f:
        doc = json.load(f)
    if not doc.get("slots"):
        return None, doc
    point = doc["slots"][0]
    tables = {}
    for table in point.get("tables", []):
        tables[table["name"]] = {
            "count": table["count"],
            "size": table["record_size"],
            "bytes": bytes.fromhex(table["bytes"]),
        }
    point["tables"] = tables
    point["scripts"] = {s["key"] & 0x3FF: (s["key"], bytes.fromhex(s["variables"])) for s in point.get("scripts", [])}
    point["swarms"] = {s["id"]: s for s in point.get("swarms", [])}
    for key in ("script_keys", "frame", "restore", "cutscene_playing", "transition"):
        if key in doc:
            point[key] = doc[key]
    return point, doc


def records(table):
    size = table["size"]
    return [table["bytes"][i * size : (i + 1) * size] for i in range(table["count"])]


def actor_in_use(record):
    return any(record[:4])


def actor_key(record):
    return record[ACTOR_MODEL], struct.unpack(">H", record[ACTOR_ID : ACTOR_ID + 2])[0]


def match_actors(saved, now):
    """Saved slot -> slot now, as resume_point.cpp's match_records pairs them."""
    now_of = [-1] * len(saved)
    taken = [False] * len(now)

    def fits(i, j, by_id):
        if now[j][ACTOR_MODEL] != saved[i][ACTOR_MODEL]:
            return False
        return not by_id or actor_key(now[j]) == actor_key(saved[i])

    for i in range(len(saved)):
        if actor_in_use(saved[i]) and i < len(now) and fits(i, i, True):
            now_of[i] = i
            taken[i] = True
    for i in range(len(saved)):
        if now_of[i] >= 0 or not actor_in_use(saved[i]):
            continue
        for j in range(len(now)):
            if not taken[j] and fits(i, j, True):
                now_of[i] = j
                taken[j] = True
                break
    for i in range(len(saved)):
        if now_of[i] < 0 and i < len(now) and not taken[i]:
            both_empty = not actor_in_use(saved[i]) and not any(now[i][:4])
            if fits(i, i, False) or both_empty:
                now_of[i] = i
                taken[i] = True
    return now_of


def match_objects(saved, now):
    """Saved object -> object now, by own index, as resume_point.cpp's match_objects."""
    now_of = [-1] * len(saved)
    taken = [False] * len(now)

    def fits(i, j):
        return now[j][OBJECT_INDEX] == saved[i][OBJECT_INDEX]

    for i in range(len(saved)):
        if i < len(now) and fits(i, i):
            now_of[i] = i
            taken[i] = True
    for i in range(len(saved)):
        for j in range(len(now)):
            if now_of[i] < 0 and not taken[j] and fits(i, j):
                now_of[i] = j
                taken[j] = True
    return now_of


def eeprom_slot_record(image, slot):
    """The newest valid record of `slot` in a save image, or None."""
    best = None
    for record in range(EEPROM_RECORDS):
        base = (EEPROM_RECORD_BLOCK + record * EEPROM_RECORD_STRIDE) * 8
        data = image[base : base + EEPROM_RECORD_SIZE]
        total = 0xCC
        for i in range(2, EEPROM_RECORD_SIZE):
            total = (total + (data[i] << (i & 3))) & 0xFFFF
        if total != struct.unpack(">H", data[:2])[0] or data[2] != slot:
            continue
        counter = struct.unpack(">I", data[3:7])[0]
        if best is None or counter > best[0]:
            best = (counter, data)
    return best


def save_field(offset):
    """What a byte of a save record holds (func_15006BEC): the event flags, D_800D2E60, the
    checkpoint, the money, the play time (left out of the comparison), then bits of D_800C3A60."""
    if 0x7 <= offset <= 0x21:
        return "event flags %d-%d" % ((offset - 7) * 8, (offset - 7) * 8 + 7)
    if 0x22 <= offset <= 0x2A:
        return "D_800D2E60[%d]" % (offset - 0x22)
    if offset == 0x2B:
        return "checkpoint scene"
    if offset == 0x2C:
        return "checkpoint entrance"
    if 0x2D <= offset <= 0x2E:
        return "money"
    if 0x2F <= offset <= 0x32:
        return "play time"
    return "D_800C3A60 bits"


# --- the comparison ---------------------------------------------------------------------------


def compare_table(name, saved, now, later, pre=None):
    """Differences between a saved table and the same table in the restore's frame (`now`), with
    what the game did to the restored words by the later captures (`later`, a list): undone when
    it put a word back to what it was before the restore (`pre`, the scene as the game loaded it)
    and left it so; settled when it changed it to anything else and left it so."""
    out = {"lost": [], "undone": [], "settled": [], "address": 0, "moved": [], "skipped": None}
    if now is None:
        out["skipped"] = "not captured"
        return out
    if saved["count"] != now["count"] and name != "objects":
        out["skipped"] = "%d saved, %d now" % (saved["count"], now["count"])
        return out
    saved_records = records(saved)
    now_records = records(now)
    same = (lambda t: t is not None) if name == "objects" else (lambda t: t is not None and t["count"] == saved["count"])
    later_records = [records(t) if same(t) else None for t in later]
    pre_records = records(pre) if same(pre) else None
    if name == "actors":
        now_of = match_actors(saved_records, now_records)
        later_of = [match_actors(saved_records, r) if r is not None else None for r in later_records]
        pre_of = match_actors(saved_records, pre_records) if pre_records is not None else None
    elif name == "objects":
        now_of = match_objects(saved_records, now_records)
        later_of = [match_objects(saved_records, r) if r is not None else None for r in later_records]
        pre_of = match_objects(saved_records, pre_records) if pre_records is not None else None
        if saved["count"] != now["count"]:
            out["count_now"] = now["count"]
        unmatched = [i for i, j in enumerate(now_of) if j < 0]
        if unmatched:
            out["unmatched"] = unmatched
    else:
        now_of = list(range(saved["count"]))
        later_of = [now_of if r is not None else None for r in later_records]
        pre_of = now_of if pre_records is not None else None
    for i, j in enumerate(now_of):
        if j < 0:
            if name == "actors" and actor_in_use(saved_records[i]):
                out.setdefault("missing", []).append({"slot": i, "model": saved_records[i][ACTOR_MODEL],
                                                      "id": "%04x" % actor_key(saved_records[i])[1]})
            continue
        then_words = words(saved_records[i])
        now_words = words(now_records[j])
        pre_words = words(pre_records[pre_of[i]]) if pre_of is not None and pre_of[i] >= 0 else None
        later_words = []
        for k, r in enumerate(later_records):
            if r is None or later_of[k] is None or later_of[k][i] < 0:
                later_words.append(None)
            else:
                later_words.append(words(r[later_of[k][i]]))
        for w, (then, cur) in enumerate(zip(then_words, now_words)):
            offset = w * 4
            if name == "actors" and i == 0:
                continue  # Conker: placed after the restore, and he moves
            if not restorable(then, cur):
                out["address"] += 1
                continue
            if name == "actors" and offset == 0x64:
                # the link byte (+0x65) is mapped through the slots, so it may differ by design
                if (then & 0xFF00FFFF) == (cur & 0xFF00FFFF):
                    continue
            if then != cur:
                out["lost"].append({"record": i, "offset": offset, "then": "%08x" % then, "now": "%08x" % cur,
                                    "healed": any(lw is not None and lw[w] == then for lw in later_words)})
                continue
            if len(later_words) >= 2 and later_words[0] is not None and later_words[1] is not None:
                held = later_words[0][w]
                if held != then and held == later_words[1][w]:
                    item = {"record": i, "offset": offset, "then": "%08x" % then, "later": "%08x" % held}
                    if pre_words is not None and held == pre_words[w] and pre_words[w] != then:
                        out["undone"].append(item)
                    else:
                        out["settled"].append(item)
        if name == "actors":
            d = 0.0
            for axis in range(3):
                a = f32(saved_records[i], ACTOR_POSITION + 4 * axis)
                b = f32(now_records[j], ACTOR_POSITION + 4 * axis)
                d += (a - b) ** 2
            if d ** 0.5 > 1.0 and i != 0:
                out["moved"].append({"slot": i, "now": j, "model": saved_records[i][ACTOR_MODEL],
                                     "distance": round(d ** 0.5, 1)})
    if name == "actors":
        saved_keys = Counter(actor_key(r) for r in saved_records if actor_in_use(r))
        now_keys = Counter(actor_key(r) for r in now_records if actor_in_use(r))
        extra = now_keys - saved_keys
        if extra:
            out["extra"] = [{"model": m, "id": "%04x" % i, "n": n} for (m, i), n in extra.items()]
        dup = [k for k, n in now_keys.items() if n > 1 and k[0] != 0xFF]
        if dup:
            out["duplicate_ids"] = ["model %d id %04x" % k for k in dup]
    return out


def slot_fields(saved, pre):
    """Byte and word offsets of the actor record that hold a slot number, found where the slots
    were shuffled between the save and the fresh load: a field whose saved value names slot s
    (as s or s + 1) and whose fresh value names the slot the same character is in now. Restored
    as saved, such a field would point at the wrong character (scene 60's bat, actor +0x65)."""
    found = Counter()
    if saved is None or pre is None or "actors" not in saved["tables"] or "actors" not in pre["tables"]:
        return found
    saved_records = records(saved["tables"]["actors"])
    pre_records = records(pre["tables"]["actors"])
    if len(saved_records) != len(pre_records):
        return found
    now_of = match_actors(saved_records, pre_records)
    if all(j == i or j < 0 for i, j in enumerate(now_of)):
        return found  # nothing shuffled: nothing to tell
    for i, j in enumerate(now_of):
        if j < 0 or i == 0 or not actor_in_use(saved_records[i]):
            continue
        a, b = saved_records[i], pre_records[j]
        for offset in range(ACTOR_SIZE):
            sv, pv = a[offset], b[offset]
            if sv == pv or sv == 0 or pv == 0:
                continue
            for base in (0, 1):  # slot, or slot + 1
                s_slot, p_slot = sv - base, pv - base
                if 0 <= s_slot < len(now_of) and s_slot != i and now_of[s_slot] == p_slot and p_slot != s_slot:
                    found[(offset, "+%d" % base)] += 1
    return found


def compare(report_dir, run_dir, at):
    result = {"report": report_dir.name, "status": "?", "problems": [], "tables": {}}
    result_path = run_dir / "save-check/result.json"
    if not result_path.exists():
        result["status"] = "no result (crashed?)"
        log = run_dir / "run.log"
        if log.exists():
            tail = log.read_text(errors="replace").splitlines()[-5:]
            result["log_tail"] = tail
        return result
    with open(result_path) as f:
        res = json.load(f)
    result["status"] = res["status"]
    if res["status"] != "done":
        result["problems"].append("%s: %s" % (res["status"], res.get("detail", "")))
        return result
    log = (run_dir / "run.log").read_text(errors="replace") if (run_dir / "run.log").exists() else ""
    for line in log.splitlines():
        if "heap damaged" in line or "fatal signal" in line:
            result["problems"].append(line.strip())
        if "resumed scene state" in line:
            result["restore"] = line.split("resumed scene state:")[1].strip()
        if "starting at its checkpoint" in line or "starting slot" in line:
            result["problems"].append(line.strip())
    saved, saved_doc = load_points(report_dir / "save.resume")
    if saved is None:
        result["problems"].append("the report has no point")
        return result
    captures = []
    for seconds in at:
        path = run_dir / ("save-check/capture-%d.resume" % seconds)
        captures.append(load_points(path)[0] if path.exists() else None)
    c0 = captures[0]
    if c0 is None:
        result["problems"].append("no capture at the restore")
        return result
    if c0.get("scene") != saved["scene"]:
        result["problems"].append("restored in scene %s, saved in %s" % (c0.get("scene"), saved["scene"]))
    later = captures[1:]
    pre_path = run_dir / "save-check/capture-pre.resume"
    pre = load_points(pre_path)[0] if pre_path.exists() else None
    result["slot_fields"] = {"%s %s" % ("0x%x" % o, b): n for (o, b), n in slot_fields(saved, pre).items()}
    for name, table in saved["tables"].items():
        cmp = compare_table(name, table, c0["tables"].get(name), [c["tables"].get(name) if c else None for c in later],
                            pre["tables"].get(name) if pre else None)
        result["tables"][name] = cmp
    # scripts: saved ones with variables must be running with the same variables in the restore frame
    running0 = {k & 0x3FF for k in c0.get("script_keys", [])}
    scripts = {"not_running": [], "variables_differ": [], "not_running_later": []}
    for event, (key, variables) in saved["scripts"].items():
        if event not in running0:
            scripts["not_running"].append("%04x" % key)
        elif event in c0["scripts"] and c0["scripts"][event][1] != variables:
            scripts["variables_differ"].append("%04x" % key)
    if later and later[0] is not None:
        running1 = {k & 0x3FF for k in later[0].get("script_keys", [])}
        for event, (key, variables) in saved["scripts"].items():
            if event in running0 and event not in running1 and not later[0].get("cutscene_playing"):
                scripts["not_running_later"].append("%04x" % key)
    result["scripts"] = scripts
    # swarms
    swarms = {"missing": [], "moved": []}
    if saved.get("swarms"):
        for sid, swarm in saved["swarms"].items():
            now = c0["swarms"].get(sid)
            if now is None:
                swarms["missing"].append(sid)
            elif now["position"] != swarm["position"] or now.get("target_actor") != swarm.get("target_actor"):
                swarms["moved"].append(sid)
    result["swarms"] = swarms
    # Conker's own kept fields
    conker = {}
    for field in ("conker_actions", "conker_drunk"):
        if saved.get(field) is not None and c0.get(field) != saved.get(field):
            conker[field] = {"then": saved.get(field), "now": c0.get(field)}
    if conker:
        result["conker"] = conker
    # the save the game wrote from the restored scene
    if res.get("report"):
        new_dir = run_dir / "conker-recomp-data/debug_reports" / res["report"]
        try:
            with open(new_dir / "report.json") as f:
                new_report = json.load(f)
            slot = new_report["save"]["slot"]
            a = eeprom_slot_record((report_dir / "save.bin").read_bytes(), slot)
            b = eeprom_slot_record((new_dir / "save.bin").read_bytes(), slot)
            if a is None or b is None:
                result["save"] = {"problem": "no valid record for slot %d" % slot}
            else:
                diffs = [i for i in range(7, EEPROM_RECORD_SIZE) if a[1][i] != b[1][i] and not 0x2F <= i <= 0x32]
                result["save"] = {"slot": slot,
                                  "differs_at": ["%s 0x%02x: %02x->%02x" % (save_field(i), i, a[1][i], b[1][i])
                                                 for i in diffs]}
            # and the new report's point against the last capture (the save path records what the capture saw)
            new_point, _ = load_points(new_dir / "save.resume")
            result["save"]["new_report"] = str(new_dir)
            if new_point is not None and captures[-1] is not None:
                mism = [n for n, t in new_point["tables"].items()
                        if n in captures[-1]["tables"] and t["count"] != captures[-1]["tables"][n]["count"]]
                if mism:
                    result["save"]["table_counts_differ"] = mism
        except (OSError, KeyError, json.JSONDecodeError) as error:
            result["save"] = {"problem": str(error)}
    return result


# --- running ---------------------------------------------------------------------------------


START_LOCK = __import__("threading").Lock()


def run_one(binary, rom, report_dir, run_dir, at, make_report, timeout):
    """Runs the check for one report; a run that could not open the ROM is started again, once."""
    for attempt in range(2):
        name, code, seconds = run_once(binary, rom, report_dir, run_dir, at, make_report, timeout)
        log = run_dir / "run.log"
        if code == 0 or not log.exists() or "Error opening stored ROM" not in log.read_text(errors="replace"):
            return name, code, seconds
    return name, code, seconds


def run_once(binary, rom, report_dir, run_dir, at, make_report, timeout):
    if run_dir.exists():
        shutil.rmtree(run_dir)
    (run_dir / "conker-recomp-data/saves").mkdir(parents=True)
    env = dict(os.environ)
    env.update({
        "CONKER_SKIP_INTRO": "1",
        "CONKER_AUDIO": "0",
        "CONKER_SAVE_SNAPSHOTS": "0",
        "CONKER_SAVE_CHECK": str(report_dir),
        "CONKER_SAVE_CHECK_OUT": str(run_dir / "save-check"),
        "CONKER_SAVE_CHECK_AT": ",".join(str(s) for s in at),
        "CONKER_SAVE_CHECK_REPORT": "1" if make_report else "0",
    })
    started = time.time()
    with open(run_dir / "run.log", "w") as log:
        # The game copies the ROM into its config folder as it starts, which two starting at once
        # race (one reads a half-written copy, drops it and dies), so starts are a few seconds apart.
        with START_LOCK:
            proc = subprocess.Popen([str(binary), str(rom)], cwd=run_dir, env=env, stdout=log, stderr=subprocess.STDOUT)
            time.sleep(3)
        try:
            code = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            code = "timeout"
    # The game keeps a 64 MB copy of the ROM in its data folder: not 124 times over.
    for copy in (run_dir / "conker-recomp-data").glob("*.z64"):
        copy.unlink()
    return report_dir.name, code, round(time.time() - started)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("reports", nargs="*")
    parser.add_argument("--binary", default=str(ROOT / "build/host/ConkersBadFurDayRecompiled"))
    parser.add_argument("--rom", default=str(DEFAULT_ROM))
    parser.add_argument("--out", default=str(ROOT / "build/save-check"))
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--at", default="0,6,30")
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--no-run", action="store_true")
    parser.add_argument("--no-report", action="store_true", help="skip the save the game writes at the end")
    args = parser.parse_args()
    at = [int(s) for s in args.at.split(",")]
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    if args.reports:
        reports = [Path(r) if "/" in r else REPORTS / r for r in args.reports]
    else:
        reports = sorted(p for p in REPORTS.iterdir() if p.is_dir())
    reports = [r for r in reports if (r / "save.bin").exists() and (r / "save.resume").exists()]
    print("%d reports" % len(reports))

    if not args.no_run:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [pool.submit(run_one, Path(args.binary), Path(args.rom), r, out / r.name, at, not args.no_report,
                                   args.timeout) for r in reports]
            for future in concurrent.futures.as_completed(futures):
                name, code, seconds = future.result()
                print("  %-28s exit %-8s %3d s" % (name, code, seconds), flush=True)

    results = [compare(r, out / r.name, at) for r in reports]
    with open(out / "details.json", "w") as f:
        json.dump(results, f, indent=1)
    write_summary(results, out / "summary.md", at)
    print("summary: %s" % (out / "summary.md"))


def write_summary(results, path, at):
    lines = ["# Save/load check", "", "%d reports, captures at %s s after the restore." % (len(results), at), ""]
    by_status = Counter(r["status"] for r in results)
    lines.append("Status: " + ", ".join("%s %d" % (k, v) for k, v in by_status.items()))
    lines.append("")
    # aggregated lost / undone offsets
    lost = defaultdict(set)
    healed = defaultdict(set)
    undone = defaultdict(set)
    settled = defaultdict(set)
    skipped = Counter()
    for r in results:
        for name, t in r.get("tables", {}).items():
            if t.get("skipped"):
                skipped[(name, t["skipped"])] += 1
            for item in t.get("lost", []):
                (healed if item["healed"] else lost)[(name, item["offset"])].add(r["report"])
            for item in t.get("undone", []):
                undone[(name, item["offset"])].add(r["report"])
            for item in t.get("settled", []):
                settled[(name, item["offset"])].add(r["report"])
    def table(title, data):
        if not data:
            return
        lines.append("## " + title)
        lines.append("")
        lines.append("| table | offset | reports | example |")
        lines.append("|---|---|---|---|")
        for (name, offset), reps in sorted(data.items(), key=lambda kv: (-len(kv[1]), kv[0])):
            lines.append("| %s | 0x%x | %d | %s |" % (name, offset, len(reps), sorted(reps)[0]))
        lines.append("")
    table("Lost: not put back in the restore's frame (and not recomputed later)", lost)
    table("Lost but recomputed by the game later", healed)
    table("Undone: put back, then changed by the game to what the fresh scene had, and left so", undone)
    table("Settled: put back, then changed by the game to something else and left so", settled)
    slot_like = Counter()
    slot_like_reports = defaultdict(set)
    for r in results:
        for field, n in r.get("slot_fields", {}).items():
            slot_like[field] += n
            slot_like_reports[field].add(r["report"])
    if slot_like:
        lines.append("## Actor fields that follow the slots (a slot number, as the link byte +0x65 is)")
        lines.append("")
        lines.append("| offset | as | matches | reports |")
        lines.append("|---|---|---|---|")
        for field, n in slot_like.most_common():
            offset, base = field.split()
            lines.append("| %s | slot %s | %d | %d |" % (offset, base, n, len(slot_like_reports[field])))
        lines.append("")
    if skipped:
        lines.append("## Tables skipped")
        lines.append("")
        for (name, why), n in skipped.most_common():
            lines.append("- %s: %s (%d reports)" % (name, why, n))
        lines.append("")
    lines.append("## Per report")
    lines.append("")
    for r in results:
        notes = []
        if r["status"] != "done":
            notes.append(r["status"])
        notes += r.get("problems", [])
        for name, t in r.get("tables", {}).items():
            if t.get("skipped"):
                notes.append("%s skipped (%s)" % (name, t["skipped"]))
            n_lost = len([i for i in t.get("lost", []) if not i["healed"]])
            if n_lost:
                notes.append("%s %d lost" % (name, n_lost))
            if t.get("undone"):
                notes.append("%s %d undone" % (name, len(t["undone"])))
            if t.get("settled"):
                notes.append("%s %d settled" % (name, len(t["settled"])))
            if t.get("unmatched"):
                notes.append("objects unmatched: %s" % t["unmatched"])
            if t.get("count_now"):
                notes.append("objects %d now" % t["count_now"])
            if t.get("missing"):
                notes.append("actors missing: " + ", ".join("m%d/%s" % (m["model"], m["id"]) for m in t["missing"]))
            if t.get("extra"):
                notes.append("actors extra: " + ", ".join("m%d/%s" % (m["model"], m["id"]) for m in t["extra"]))
            if t.get("duplicate_ids"):
                notes.append("duplicate ids: " + ", ".join(t["duplicate_ids"]))
            if t.get("moved"):
                notes.append("moved: " + ", ".join("m%d %.0f" % (m["model"], m["distance"]) for m in t["moved"]))
        s = r.get("scripts", {})
        for k in ("not_running", "variables_differ", "not_running_later"):
            if s.get(k):
                notes.append("scripts %s: %s" % (k, ", ".join(s[k])))
        w = r.get("swarms", {})
        for k in ("missing", "moved"):
            if w.get(k):
                notes.append("swarms %s: %s" % (k, w[k]))
        if r.get("conker"):
            notes.append("conker: %s" % r["conker"])
        sv = r.get("save", {})
        if sv.get("problem"):
            notes.append("save: " + sv["problem"])
        elif sv.get("differs_at"):
            notes.append("save differs at " + ", ".join(sv["differs_at"]))
        if sv.get("table_counts_differ"):
            notes.append("new save's table counts differ: %s" % sv["table_counts_differ"])
        lines.append("- **%s**: %s" % (r["report"], "; ".join(notes) if notes else "clean"))
    path.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
