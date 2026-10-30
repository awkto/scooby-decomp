#!/usr/bin/env python3
"""Generate port/scripts/pointer_play.txt: seeded random play that stays in pointer mode
a lot (B/A enter it) and visits the verb bar / inventory (pointer at the bottom of the
screen), for difftest captures of the main-loop code (#12). Play it after lobby.txt:

    difftest capture D3E 200 --script port/scripts/lobby.txt --script port/scripts/pointer_play.txt
"""
import random
import sys

rnd = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else 1212)
out = ["# Pointer-mode / verb-bar / inventory play after lobby.txt (tools/py/pointerscript.py, seed 1212)"]
for i in range(260):
    r = rnd.random()
    if r < 0.25:
        out.append("press B 3")                           # enter pointer mode / click
        out.append(f"run {rnd.randint(5, 40)}")
    elif r < 0.35:
        out.append("press A 3")
        out.append(f"run {rnd.randint(5, 40)}")
    elif r < 0.55:
        # down to the verb bar, slide sideways, click
        out.append(f"press D {rnd.randint(30, 90)}")
        out.append(f"press {rnd.choice('LR')} {rnd.randint(5, 60)}")
        out.append("press B 3")
        out.append(f"run {rnd.randint(10, 40)}")
    elif r < 0.60:
        out.append("press C 3")
        out.append(f"run {rnd.randint(10, 60)}")
    else:
        d = rnd.choice(["U", "D", "L", "R", "UL", "UR", "DL", "DR"])
        out.append(f"press {d} {rnd.randint(5, 50)}")
        out.append(f"run {rnd.randint(1, 20)}")
print("\n".join(out))
