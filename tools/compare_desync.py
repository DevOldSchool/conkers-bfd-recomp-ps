#!/usr/bin/env python3
"""compare_desync.py HOST_FILE GUEST_FILE: two machines' desync files (conker-recomp-data/net/desync-...)
from the same session. Prints the first frame whose hashes differ and, for the frames both files have
block hashes for, which 64 KiB blocks of game memory differ (addresses as the game sees them).

compare_desync.py --ram HOST.ram GUEST.ram: the game memory both machines kept of their last frames
(the .ram file next to each desync file). Prints, for the first frame both have whose memory differs,
the 32-bit words that differ, as the game reads them, grouped into runs."""
import struct
import sys
import zlib

# Words the hash leaves out (src/net/net_play.cpp kHashSkips): clock readings and a queue count.
SKIPS = [(0x000BE6E8, 0x000BE6F8), (0x000BEA18, 0x000BEA1C), (0x000BEA28, 0x000BEA68)]


def read_ram(path):
    frames = {}
    with open(path, 'rb') as f:
        if f.read(6) != b'CKRAM1':
            sys.exit(f'{path}: not a desync memory file')
        start, size, count = struct.unpack('<III', f.read(12))
        previous = None
        for _ in range(count):
            head = f.read(8)
            if len(head) < 8:
                break
            frame, packed = struct.unpack('<II', head)
            data = zlib.decompress(f.read(packed))
            if previous is not None:
                data = bytes(a ^ b for a, b in zip(data, previous))
            frames[frame] = data
            previous = data
    return start, frames


def compare_ram(host_path, guest_path):
    start, host = read_ram(host_path)
    _, guest = read_ram(guest_path)
    common = sorted(set(host) & set(guest))
    print(f'frames kept by both: {common[0] if common else "-"} to {common[-1] if common else "-"}')
    for frame in common:
        a, b = host[frame], guest[frame]
        words = []
        for at in range(0, min(len(a), len(b)) - 3, 4):
            address = start + at
            if a[at:at + 4] != b[at:at + 4] and not any(lo <= address < hi for lo, hi in SKIPS):
                words.append(at)
        if not words:
            continue
        print(f'frame {frame}: {len(words)} words differ (host / guest, 32-bit as the game reads them)')
        runs = []
        for at in words:
            if runs and at - runs[-1][-1] <= 16:
                runs[-1].append(at)
            else:
                runs.append([at])
        for run in runs[:40]:
            print(f'  0x{0x80000000 + start + run[0]:08X}: {len(run)} words')
            for at in run[:8]:
                h = struct.unpack_from('<I', a, at)[0]
                g = struct.unpack_from('<I', b, at)[0]
                print(f'    0x{0x80000000 + start + at:08X}  {h:08X} / {g:08X}')
        if len(runs) > 40:
            print(f'  ... {len(runs) - 40} more runs')
        return
    print('no frame both kept differs')


def read(path):
    hashes, blocks, base = {}, {}, 0
    with open(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == '#' and 'from' in parts:
                base = int(parts[parts.index('from') + 1].rstrip(':'), 16)
            elif parts[0] == 'b':
                blocks[int(parts[1])] = parts[2:]
            elif parts[0].isdigit() and len(parts) == 2:
                hashes[int(parts[0])] = parts[1]
    return hashes, blocks, base


def main():
    if len(sys.argv) == 4 and sys.argv[1] == '--ram':
        compare_ram(sys.argv[2], sys.argv[3])
        return
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    (h1, b1, base), (h2, b2, _) = read(sys.argv[1]), read(sys.argv[2])
    common = sorted(set(h1) & set(h2))
    differ = [f for f in common if h1[f] != h2[f]]
    print(f'{len(common)} frames in both, {len(differ)} differ' + (f', first {differ[0]}' if differ else ''))
    shown = 0
    for f in sorted(set(b1) & set(b2)):
        bad = [i for i, (x, y) in enumerate(zip(b1[f], b2[f])) if x != y]
        if bad and shown < 8:
            ranges = ', '.join(f'0x{base + i * 0x10000:08x}' for i in bad)
            print(f'frame {f}: {len(bad)} blocks differ: {ranges}')
            shown += 1


if __name__ == '__main__':
    main()
