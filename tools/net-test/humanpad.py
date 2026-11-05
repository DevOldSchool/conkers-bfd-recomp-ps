#!/usr/bin/env python3
"""Pad scripts that play like a person, for measuring online traffic (CONKER_NET_STATS).

The f1/f2/f3 pads hold each stick position for seconds, so a record hardly ever changes and the wire's
change encoding looks better than it is. A person's thumb moves the stick a little nearly every frame.
This keeps a pad's lines up to the match (walking to MULTI, joining) and from FROM on plays one game
frame (2 VIs) a line: sticks that wander and wobble, buttons pressed for a few frames, the mouse now and
then. Seeded, so every run plays the same.

    humanpad.py f1.pad 9000 1 > hu1.pad    # the host: its pad from VI 9000 on
    humanpad.py f2.pad 9600 2 > hu2.pad
"""
import random
import sys

UNTIL = 36000  # VIs: 10 minutes of play from the start, longer than any test run


def main():
    source, start, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    rng = random.Random(seed)
    with open(source) as f:
        for line in f:
            words = line.split()
            if not words or words[0].startswith("#") or not words[0].isdigit() or int(words[0]) < start:
                sys.stdout.write(line)
    lx = ly = rx = ry = 0.0
    tx = ty = 0.0  # where the left stick heads
    hold = {}      # button: frames left
    still = 0      # frames the thumb rests (the stick wobbles by a step)
    camera = 0     # frames the right stick is in use
    mouse = 0
    mx = my = 0.0
    for vi in range(start, UNTIL, 2):
        if still > 0:
            still -= 1
            lx += rng.choice((-1, 0, 0, 1)) / 80
            ly += rng.choice((-1, 0, 0, 1)) / 80
        else:
            if rng.random() < 0.03:
                tx, ty = rng.uniform(-1, 1), rng.uniform(-1, 1)
            if rng.random() < 0.01:
                still = rng.randint(15, 90)
            lx += max(-0.08, min(0.08, (tx - lx) * 0.2)) + rng.uniform(-0.02, 0.02)
            ly += max(-0.08, min(0.08, (ty - ly) * 0.2)) + rng.uniform(-0.02, 0.02)
        lx, ly = max(-1, min(1, lx)), max(-1, min(1, ly))
        if camera > 0:
            camera -= 1
            rx = max(-1, min(1, rx + rng.uniform(-0.15, 0.15)))
            ry = max(-1, min(1, ry + rng.uniform(-0.1, 0.1)))
        elif rng.random() < 0.01:
            camera = rng.randint(20, 120)
        else:
            rx = ry = 0.0
        for button, chance in (("a", 0.02), ("b", 0.01), ("righttrigger=1", 0.03), ("rightshoulder", 0.005)):
            if button not in hold and rng.random() < chance:
                hold[button] = rng.randint(3, 12) if button != "rightshoulder" else rng.randint(30, 150)
        if mouse > 0:
            mouse -= 1
            mx, my = mx * 0.8 + rng.uniform(-2, 2), my * 0.8 + rng.uniform(-1, 1)
        elif seed == 1 and rng.random() < 0.01:
            mouse = rng.randint(20, 90)
        else:
            mx = my = 0.0
        words = [f"leftx={lx:.3f}", f"lefty={ly:.3f}"]
        if rx or ry:
            words += [f"rightx={rx:.3f}", f"righty={ry:.3f}"]
        if mx or my:
            words += [f"mousex={mx:.2f}", f"mousey={my:.2f}"]
        words += list(hold)
        for button in list(hold):
            hold[button] -= 1
            if hold[button] == 0:
                del hold[button]
        print(vi, 2, " ".join(words))


main()
