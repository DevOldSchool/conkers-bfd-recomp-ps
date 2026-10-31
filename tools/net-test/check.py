#!/usr/bin/env python3
# check.py NAME: compares a runsim.sh session's peers (game blocks 0x80084000-0x803B0000 of the hash
# logs, the input records) and sums the wait statistics.
import re
import sys
import os

base = os.path.join(os.environ.get("NET_TEST_DIR", "/tmp/conker-net-test"), "logs", sys.argv[1])
roles = [r for r in ("h", "g", "g2") if os.path.exists(f"{base}.{r}.hash.log")]


def hashes(r):
    out = {}
    for line in open(f"{base}.{r}.hash.log"):
        f = line.split()
        if len(f) == 257:
            out[int(f[0])] = f[1 + 0x21 : 1 + 0xEC]  # blocks 0x80084000 .. 0x803AC000
    return out


def inputs(r):
    out = {}
    for line in open(f"{base}.{r}.input.log"):
        if line.startswith("#"):
            continue
        f = line.split()
        out[int(f[0])] = " ".join(f[2:])
    return out


H = {r: hashes(r) for r in roles}
INP = {r: inputs(r) for r in roles}
for r in roles[1:]:
    common = sorted(set(H["h"]) & set(H[r]))
    bad = [f for f in common if H["h"][f] != H[r][f]]
    ci = sorted(set(INP["h"]) & set(INP[r]))
    badi = [f for f in ci if INP["h"][f] != INP[r][f]]
    print(
        f"h vs {r}: {len(common)} hashed frames in common (to {common[-1] if common else 0}), {len(bad)} differ"
        + (f" from {bad[0]}" if bad else "")
        + f"; inputs {len(ci)} frames, {len(badi)} differ"
        + (f" from {badi[0]}" if badi else "")
    )
for r in roles:
    frames = sum(1 for _ in open(f"{base}.{r}.frame.log"))
    scenes = []
    for line in open(f"{base}.{r}.frame.log"):
        sc = line.split()[2]
        if not scenes or scenes[-1][1] != sc:
            scenes.append((line.split()[0], sc))
    st = [
        m.groups()
        for m in re.finditer(
            r"frame (\d+): ([\d.]+) fps, waited (\d+) ms in all; (\d+) stalls of 50 ms or more, (\d+) ms, the longest (\d+) ms; round trip (-?\d+) ms",
            open(f"{base}.{r}.out").read(),
        )
    ]
    stalls = sum(int(x[3]) for x in st)
    stalled = sum(int(x[4]) for x in st)
    longest = max([int(x[5]) for x in st] or [0])
    fps = [float(x[1]) for x in st]
    rtt = [int(x[6]) for x in st]
    print(
        f"{r}: {frames} frames, scenes {' '.join(f'{s}@{f}' for f, s in scenes)}; stalls {stalls} ({stalled} ms, longest {longest} ms); "
        f"fps min {min(fps) if fps else 0:.1f} median {sorted(fps)[len(fps)//2] if fps else 0:.1f}; rtt median {sorted(rtt)[len(rtt)//2] if rtt else -1}"
    )
