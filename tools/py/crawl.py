#!/usr/bin/env python3
"""Crawl every room of a mystery from its password-warp state: for each object under the crosshair,
try the verbs and log what the game says / which story bytes or inventory change.
    crawl.py SESSION_DIR MYST [ROOMS...]  > runs/crawl_wM.txt     (needs `session.py serve SESSION_DIR`)
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from story import *

d, myst = sys.argv[1], int(sys.argv[2])
rooms = [int(x) for x in sys.argv[3:]] or range(2, 23 if myst == 0 else 32)
g = Game(d)
g.send("save _boot0")
for rm in rooms:
    path = f"tools/scripts/rooms/w{myst}_r{rm:02d}.txt"
    if not os.path.exists(path):
        continue
    g.send("load _boot0")
    g.route = []
    open(g.rpath, "w").write("")
    g.run_script(path)
    g.run(60)
    st = g.state()
    print(f"=== room {rm} ({st['room']}) pos=({st['x']},{st['y']}) story={st['story']}", flush=True)
    for t, n, rec in g.room_objects():
        print(f"  obj {t:3d} {n:22s} rec={[hex(x) for x in rec][:6]}")
    g.explore_room(wait=10, verbs=("Look", "Take", "Open", "Push", "Pull", "Talk", "Shut", "Eat"))
