#!/usr/bin/env python3
"""roll_compare.py BINARY LOG_30 LOG_60 [RECOMPILED_DIR]: two CONKER_ROLL_LOG files of the same pad
script over the same retraces, one at 30 fps and one at 60 (docs/diagnostics.md), compared by caller.

A place that rolls once a frame has about twice the count at 60 fps; one that rolls by the time that
passed has about the same. Prints the places whose count grew by more than half, most rolls first,
each with the MIPS address of its call (read from the recompiled C beside the line the caller's
address belongs to, found with macOS's atos) and whether config/us.toml already has a 60 fps hook
there. macOS only.
"""
import os
import re
import subprocess
import sys


def read(path):
    out = {}
    for line in open(path):
        if line.startswith("#"):
            continue
        count, kind, offset, name = line.split()[:4]
        out[(kind, int(offset, 16))] = (int(count), name)
    return out


def main():
    binary, log30, log60 = sys.argv[1:4]
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    recompiled = sys.argv[4] if len(sys.argv) > 4 else os.path.join(here, "build", "us", "RecompiledFuncs")
    a, b = read(log30), read(log60)
    hooks = open(os.path.join(here, "config", "us.toml")).read()
    rows = []
    for key in sorted(set(a) | set(b)):
        n30 = a.get(key, (0, ""))[0]
        n60, name = b.get(key, (0, a.get(key, (0, "?"))[1]))
        rows.append((n60, n30, key, name))
    rows.sort(reverse=True)
    grew = [r for r in rows if r[0] > 1.5 * max(r[1], 1) and r[0] >= 20]
    print(f"{len(rows)} callers; {len(grew)} rolled more than half again as often at 60 fps (20 rolls or more)")
    if not grew:
        return
    # The return address is the instruction after the call: one before it is in the call's line.
    addresses = [hex(0x100000000 + key[1] - 1) for _, _, key, _ in grew]
    lines = subprocess.run(["atos", "-o", binary, *addresses], capture_output=True, text=True).stdout.splitlines()
    for (n60, n30, key, name), where in zip(grew, lines):
        mips = "?"
        match = re.search(r"\((\w+\.c):(\d+)\)", where)
        if match:
            source = os.path.join(recompiled, match.group(1))
            if os.path.exists(source):
                text = open(source).read().splitlines()
                for i in range(int(match.group(2)) - 1, max(int(match.group(2)) - 12, 0), -1):
                    found = re.match(r"\s*// (0x[0-9A-Fa-f]+): jal", text[i])
                    if found:
                        mips = found.group(1)
                        break
        # A chance hook sits before the instruction after the call and its delay slot.
        after = f"0x{int(mips, 16) + 8:X}" if mips != "?" else ""
        hooked = "hooked" if after and re.search(rf"before_vram = {after}\b", hooks, re.IGNORECASE) else ""
        print(f"{n60:8d} {n30:8d} x{n60 / max(n30, 1):4.1f} {key[0]} {name:16s} call at {mips} {hooked}")


if __name__ == "__main__":
    main()
