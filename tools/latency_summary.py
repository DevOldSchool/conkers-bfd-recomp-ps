#!/usr/bin/env python3
"""Sum up CONKER_LATENCY_LOG files (src/enhancements/frame_rate/latency.cpp).

Each line is "<present ns> <sample ns> <index> <count> <ms>": one presented picture, made from the
game frame whose input was sampled at <sample ns>, index of count pictures made from that frame.

  first:  the first picture showing anything of a frame's input (with interpolation, only a step of
          the way from the frame before)
  full:   the last picture of the frame, the first one showing it fully
  fps:    pictures presented a second; frames: game frames a second

Usage: tools/latency_summary.py <log> [<log> ...] [--skip <seconds>]
"""
import statistics
import sys


def summarize(path, skip_s):
    rows = []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 5:
                continue
            rows.append((int(parts[0]), int(parts[1]), int(parts[2]), int(parts[3]), float(parts[4])))
    if not rows:
        return f"{path}: no presents"
    start = rows[0][0] + int(skip_s * 1e9)
    rows = [r for r in rows if r[0] >= start]
    if len(rows) < 2:
        return f"{path}: too short"
    frames = {}
    for present, sample, index, count, ms in rows:
        frames.setdefault(sample, []).append((index, count, ms))
    first = [min(p, key=lambda x: x[0])[2] for p in frames.values()]
    full = [max(p, key=lambda x: x[0])[2] for p in frames.values()]
    seconds = (rows[-1][0] - rows[0][0]) / 1e9
    def stats(values):
        values = sorted(values)
        p95 = values[min(len(values) - 1, int(len(values) * 0.95))]
        return f"mean {statistics.mean(values):6.1f}  median {statistics.median(values):6.1f}  p95 {p95:6.1f} ms"
    return (f"{path}\n  {len(rows) / seconds:6.1f} fps shown, {len(frames) / seconds:5.1f} game frames/s, "
            f"{statistics.mean(len(p) for p in frames.values()):.2f} pictures a frame\n"
            f"  first  {stats(first)}\n  full   {stats(full)}")


def main():
    args = sys.argv[1:]
    skip = 0.0
    if "--skip" in args:
        i = args.index("--skip")
        skip = float(args[i + 1])
        del args[i:i + 2]
    for path in args:
        print(summarize(path, skip))


if __name__ == "__main__":
    main()
