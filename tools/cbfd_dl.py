#!/usr/bin/env python3
"""Replay an F3DEX2CBFD display list offline from a CONKER_DUMP_RDRAM image.

The RDRAM image (conker-task-<n>.rdram, big-endian) is what the game handed the RSP, so this
answers "is the geometry already wrong in memory, or does the renderer misread it?" without
RT64 in the loop. It tracks segments, the modelview and projection matrices, the viewport and
the 32-slot vertex buffer the way the microcode does (vertices are transformed when VTX loads
them), and decodes TRI1/TRI2/QUAD and CBFD's TRI4 (0x10..0x1F).

Output: one line per model instance (draws grouped by the segment 3 base, which Conker points
at each character's bone-matrix palette) with the screen-space bounding box of its vertices, and
optionally the triangles with the largest screen extent and where their vertices came from.

    tools/cbfd_dl.py conker-task-2000.rdram 0x122C20 [--largest N] [--instance 0x12FE40]

The display-list address is the task's data pointer from the CONKER_DUMP_DL log.
"""

from __future__ import annotations

import argparse
import struct
from collections import defaultdict


def ident():
    return [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def xform(v, m):
    return [sum(v[k] * m[k][j] for k in range(4)) for j in range(4)]


class Walker:
    def __init__(self, image: bytes):
        self.m = image
        self.seg = [0] * 16
        self.mv = [ident()]
        self.proj = ident()
        self.viewport = None
        self.vtx = [None] * 32
        self.tris = []
        self.mtx_addr = None
        self.stack = []

    def u32(self, a):
        return struct.unpack_from(">I", self.m, a)[0]

    def s16(self, a):
        return struct.unpack_from(">h", self.m, a)[0]

    def u16(self, a):
        return struct.unpack_from(">H", self.m, a)[0]

    def resolve(self, a):
        return (self.seg[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0xFFFFFF

    def matrix(self, a):
        # Split 16.16: sixteen integer halves, then sixteen fraction halves, row-major.
        return [[self.s16(a + (i * 4 + j) * 2) + self.u16(a + 32 + (i * 4 + j) * 2) / 65536.0
                 for j in range(4)] for i in range(4)]

    def screen(self, c):
        if self.viewport is None or c[3] == 0:
            return None
        sx, sy, _, _, tx, ty, _, _ = self.viewport
        return (c[0] / c[3] * sx + tx, -c[1] / c[3] * sy + ty, c[3])

    def tri(self, cmd, idx):
        self.tris.append({"cmd": cmd, "idx": idx, "v": [self.vtx[i] for i in idx],
                          "seg1": self.seg[1], "seg3": self.seg[3]})

    def run(self, start):
        pc = start & 0xFFFFFF
        for _ in range(500000):
            w0, w1 = self.u32(pc), self.u32(pc + 4)
            op = w0 >> 24
            cmd = pc
            pc += 8
            if op == 0xDF:  # ENDDL
                if not self.stack:
                    return
                pc = self.stack.pop()
            elif op == 0xDE:  # DL (call or branch)
                if ((w0 >> 16) & 0xFF) == 0:
                    self.stack.append(pc)
                pc = self.resolve(w1)
            elif op == 0xDB and ((w0 >> 16) & 0xFF) == 6:  # MOVEWORD segment
                self.seg[((w0 & 0xFFFF) >> 2) & 0xF] = w1 & 0xFFFFFF
            elif op == 0xDA:  # MTX; F3DEX2 stores the push bit inverted
                params = (w0 & 0xFF) ^ 1
                a = self.resolve(w1)
                m = self.matrix(a)
                if params & 4:
                    self.proj = m if params & 2 else mul(m, self.proj)
                else:
                    if params & 1:
                        self.mv.append(self.mv[-1])
                    self.mv[-1] = m if params & 2 else mul(m, self.mv[-1])
                    self.mtx_addr = a
            elif op == 0xD8:  # POPMTX
                for _ in range(w1 // 64):
                    if len(self.mv) > 1:
                        self.mv.pop()
            elif op == 0xDC and (w0 & 0xFF) == 8:  # MOVEMEM viewport
                a = self.resolve(w1)
                self.viewport = [self.s16(a + i * 2) / 4.0 for i in range(8)]
            elif op == 0x01:  # VTX
                n = (w0 >> 12) & 0xFF
                v0 = ((w0 >> 1) & 0x7F) - n
                a = self.resolve(w1)
                mvp = mul(self.mv[-1], self.proj)
                for i in range(n):
                    va = a + i * 16
                    pos = (self.s16(va), self.s16(va + 2), self.s16(va + 4))
                    clip = xform([*pos, 1.0], mvp)
                    self.vtx[(v0 + i) & 31] = {"pos": pos, "src": va, "cmd": cmd,
                                               "mtx": self.mtx_addr, "scr": self.screen(clip)}
            elif op in (0x05, 0x06, 0x07):  # TRI1, TRI2, QUAD
                self.tri(cmd, [((w0 >> 16) & 0xFF) // 2, ((w0 >> 8) & 0xFF) // 2, (w0 & 0xFF) // 2])
                if op != 0x05:
                    self.tri(cmd, [((w1 >> 16) & 0xFF) // 2, ((w1 >> 8) & 0xFF) // 2, (w1 & 0xFF) // 2])
            elif 0x10 <= op <= 0x1F:  # CBFD TRI4
                idx = [(w0 >> 23) & 31, (w0 >> 18) & 31, (((w0 >> 15) & 7) << 2) | ((w1 >> 30) & 3),
                       (w0 >> 10) & 31, (w0 >> 5) & 31, w0 & 31,
                       (w1 >> 25) & 31, (w1 >> 20) & 31, (w1 >> 15) & 31,
                       (w1 >> 10) & 31, (w1 >> 5) & 31, w1 & 31]
                for t in range(4):
                    a, b, c = idx[t * 3:t * 3 + 3]
                    if not a == b == c:
                        self.tri(cmd, [a, b, c])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("rdram")
    parser.add_argument("dl", type=lambda s: int(s, 16))
    parser.add_argument("--largest", type=int, default=0, help="print the N triangles with the largest screen extent")
    parser.add_argument("--instance", type=lambda s: int(s, 16), help="restrict --largest to one segment 3 base")
    args = parser.parse_args()

    with open(args.rdram, "rb") as f:
        w = Walker(f.read())
    w.run(args.dl)
    print(f"{len(w.tris)} triangles, viewport {w.viewport}")

    groups = defaultdict(list)
    for t in w.tris:
        if t["seg3"] and all(v and v["scr"] and v["scr"][2] > 0 for v in t["v"]):
            groups[(t["seg3"], t["seg1"])].append(t)
    for (seg3, seg1), ts in groups.items():
        xs = sorted(v["scr"][0] for t in ts for v in t["v"])
        ys = sorted(v["scr"][1] for t in ts for v in t["v"])
        q = len(xs) // 20
        print(f"instance seg3 {seg3:06X} seg1 {seg1:06X}: {len(ts):4d} tris, "
              f"x {xs[0]:.0f}..{xs[-1]:.0f} (5-95% {xs[q]:.0f}..{xs[-1 - q]:.0f}), "
              f"y {ys[0]:.0f}..{ys[-1]:.0f} (5-95% {ys[q]:.0f}..{ys[-1 - q]:.0f})")

    if args.largest:
        rows = []
        for t in w.tris:
            if args.instance is not None and t["seg3"] != args.instance:
                continue
            if not all(v and v["scr"] and v["scr"][2] > 0 for v in t["v"]):
                continue
            xs = [v["scr"][0] for v in t["v"]]
            ys = [v["scr"][1] for v in t["v"]]
            rows.append((max(max(xs) - min(xs), max(ys) - min(ys)), t))
        rows.sort(key=lambda r: -r[0])
        for extent, t in rows[:args.largest]:
            print(f"{t['cmd']:06X} extent {extent:.0f}px idx {t['idx']}")
            for i, v in zip(t["idx"], t["v"]):
                print(f"    v{i:<2d} src {v['src']:06X} mtx {v['mtx'] or 0:06X} loaded@{v['cmd']:06X} "
                      f"pos {v['pos']} screen ({v['scr'][0]:.1f}, {v['scr'][1]:.1f}) w {v['scr'][2]:.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
