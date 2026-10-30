#!/usr/bin/env python3
"""Emit a harness script that exercises the verb bar / inventory UI: Lobby route, then
cursor-mode fuzz (A/B enter cursor mode, D-pad moves the cursor, B held over the bar selects a
verb, C slides the bar to the inventory).  usage: verbscript.py FRAMES SEED > port/scripts/verbs.txt"""
import random, sys
frames, seed = int(sys.argv[1]), int(sys.argv[2])
rng = random.Random(seed)
print("run 1500")
for _ in range(7):
    print("press S 4\nrun 150")
print("run 120")
t = 0
while t < frames:
    r = rng.random()
    if r < 0.30:                       # walk the cursor to the bar and sweep along it
        d = rng.randint(20, 110); print(f"press D {d}")
        for _ in range(rng.randint(1, 5)):
            b = rng.choice(["L", "R"]); n = rng.randint(1, 30); print(f"press {b} {n}"); print("run 2")
        print("press B " + str(rng.randint(2, 6))); print("run 6"); t += d + 40
    elif r < 0.45:
        print("press C 3"); w = rng.randint(20, 40); print(f"run {w}"); t += 3 + w   # slide bar (16 frames)
    elif r < 0.60:
        print("press A 3"); print("run 8"); t += 11
    elif r < 0.75:
        print("press B 3"); print("run 8"); t += 11
    elif r < 0.90:
        b = rng.choice(["U", "D", "L", "R", "UL", "UR", "DL", "DR"]); n = rng.randint(5, 60)
        print(f"press {b} {n}"); print("run 3"); t += n + 3
    else:
        b = rng.choice(["B", "A"]); print(f"press {b} {rng.randint(10, 30)}"); print("run 4"); t += 30
