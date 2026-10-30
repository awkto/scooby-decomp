#!/usr/bin/env python3
"""objrooms.py MYST  -- initial room of every object (ROM init records), grouped by room: `room: tid:name ...`"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import objects as O

m = int(sys.argv[1]) if len(sys.argv) > 1 else 0
h = O.HDRS[m]
start, end = O.L(h + 4), O.L(h + 8)
names = {o["tid"]: o["name"] for o in O.load(m)}
a = start
i = 0
rooms = {}
while a < end:
    fl = O.ROM[a + 0xC]
    rec = 18 if fl & 2 else 14
    room = O.W(a + 6)
    rooms.setdefault(room, []).append(f"{i + 3}:{names.get(i + 3, '?')}")
    a += rec
    i += 1
for r in sorted(rooms):
    print(r, " | ".join(rooms[r]))
