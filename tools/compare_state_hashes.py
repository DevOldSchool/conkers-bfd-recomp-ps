#!/usr/bin/env python3
"""Compare two CONKER_STATE_HASH_LOG files frame by frame (docs/online-multiplayer-plan.md, M1).

Each line of a log is a frame number and one hash per 16 KiB block of RDRAM from 0x80000000. Prints
the first frame where the runs differ outside the ignored blocks, and for each block how many of the
common frames it differed in, so that noise (blocks that differ all the time) can be told from a
real divergence (blocks that match until some frame and then stay different).

    tools/compare_state_hashes.py a.log b.log [--ignore 0x803B0000-0x80400000 ...]
"""
import argparse
import sys

BLOCK = 0x4000
BASE = 0x80000000


def read(path):
    frames = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                frames[int(parts[0])] = parts[1:]
            except ValueError:
                continue
    return frames


def parse_range(text):
    start, _, end = text.partition("-")
    return int(start, 16), int(end, 16)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--ignore", action="append", default=[], type=parse_range,
                        help="an address range to leave out, START-END in hex (repeatable)")
    parser.add_argument("--blocks", type=int, default=20, help="how many of the most differing blocks to list")
    args = parser.parse_args()

    a, b = read(args.a), read(args.b)
    common = sorted(set(a) & set(b))
    if not common:
        print("no frames in common")
        return 1

    def ignored(block):
        address = BASE + block * BLOCK
        return any(start <= address < end or start < address + BLOCK <= end for start, end in args.ignore)

    differing = {}  # block -> [frames]
    first = None
    for frame in common:
        for block, (x, y) in enumerate(zip(a[frame], b[frame])):
            if x != y and not ignored(block):
                differing.setdefault(block, []).append(frame)
                if first is None:
                    first = (frame, block)

    print(f"{len(common)} frames in common ({common[0]} to {common[-1]})")
    if first is None:
        print("identical outside the ignored blocks")
        return 0
    frame, block = first
    print(f"first difference: frame {frame}, block 0x{BASE + block * BLOCK:08X}")
    print("blocks that differ (frames differing / from frame):")
    ranked = sorted(differing.items(), key=lambda item: (item[1][0], -len(item[1])))
    for block, frames in ranked[: args.blocks]:
        print(f"  0x{BASE + block * BLOCK:08X}  {len(frames):6d} / {len(common)}  from {frames[0]}")
    if len(ranked) > args.blocks:
        print(f"  ... {len(ranked) - args.blocks} more")
    return 1


if __name__ == "__main__":
    sys.exit(main())
