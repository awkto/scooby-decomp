#!/usr/bin/env python3
"""Fill scripts/story/points_w{M}.json: warp into each room (password script), scan the crosshair hit points of all objects.
    collect_points.py SESSION_DIR MYST      (needs `session.py serve SESSION_DIR`; run on the VM)"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from story import *

d, myst = sys.argv[1], int(sys.argv[2])
g = Game(d)
for rm in range(2, 23 if myst == 0 else 32):
    path = f"tools/scripts/rooms/w{myst}_r{rm:02d}.txt"
    if not os.path.exists(path):
        continue
    g.send("load _boot0")
    g.run_script(path, record=False)
    g.send("run 90")
    g.leave_pointer() if False else None
    objs = g.scan_objects()
    print(rm, len(objs), flush=True)
g.save_points()
