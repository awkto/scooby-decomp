#!/usr/bin/env python3
"""Emit a harness script of random play from a savestate.

    fuzzscript.py STATE FRAMES SEED [--no-pause] [--peek N] [--bias] [--roomsave]

--no-pause   never press Start in-game (Start opens the pause menu; QUIT MYSTERY drops back to the title)
--peek N     log the room number ($FF06AC) every ~N frames to peek.log
--bias       walk-heavy mix (long d-pad runs, rarer A/B/C)
--pointer    pointer-mode preset (crosshair + frequent B clicks)
--roomsave   write a savestate 90 frames after first entering each room (rs_<mystery>_<room>.state)
"""
import argparse
import random


def gen(state, frames, seed, no_pause=True, peek=0, bias=False, roomsave=True, pointer=False):
    rng = random.Random(seed)
    out = [f"load {state}"]
    if roomsave:
        out.append("roomsave rs")
    moves = ["U", "D", "L", "R", "UL", "UR", "DL", "DR", "A", "B", "C", "A", "B", "C"]
    if bias:
        moves = ["U", "D", "L", "R", "UL", "UR", "DL", "DR"] * 3 + ["A", "B", "C"]
    if pointer:
        # pointer mode (press C once): d-pad moves the crosshair, B clicks (verb / object), A selects; C rarely
        moves = ["U", "D", "L", "R", "UL", "UR", "DL", "DR"] * 2 + ["B"] * 5 + ["A"] * 2 + ["C"]
        out.append("press C 6")
        out.append("run 30")
    if not no_pause:
        moves.append("S")
    t = nextpeek = 0
    while t < frames:
        b = rng.choice(moves)
        n = rng.randint(3, 6) if b in ("A", "B", "C", "S") else (rng.randint(3, 25) if pointer else rng.randint(10, 90))
        out.append(f"press {b} {n}")
        w = rng.randint(5, 60)
        out.append(f"run {w}")
        t += n + w
        if peek and t >= nextpeek:
            out.append("peek room FF06AC 2")
            nextpeek = t + peek
    out.append("peek room FF06AC 2")
    out.append("shot end")
    return out


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("state")
    ap.add_argument("frames", type=int)
    ap.add_argument("seed", type=int)
    ap.add_argument("--no-pause", action="store_true")
    ap.add_argument("--peek", type=int, default=0)
    ap.add_argument("--bias", action="store_true")
    ap.add_argument("--roomsave", action="store_true")
    ap.add_argument("--pointer", action="store_true")
    a = ap.parse_args()
    print("\n".join(gen(a.state, a.frames, a.seed, a.no_pause, a.peek, a.bias, a.roomsave, a.pointer)))
