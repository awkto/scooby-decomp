#!/usr/bin/env python3
"""Emit a harness script of random play from a savestate: fuzzscript.py STATE FRAMES SEED"""
import random, sys
state, frames, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
rng = random.Random(seed)
print(f"load {state}")
moves = ["U", "D", "L", "R", "UL", "UR", "DL", "DR", "A", "B", "C", "A", "B", "C", "S"]
t = 0
while t < frames:
    b = rng.choice(moves)
    n = rng.randint(2, 6) if b in ("A", "B", "C", "S") else rng.randint(10, 90)
    print(f"press {b} {n}")
    w = rng.randint(5, 60)
    print(f"run {w}")
    t += n + w
print("shot end")
