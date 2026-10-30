#!/usr/bin/env python3
"""blk.py MYST TID... [-v]  -- print the disassembled script of objects (from build/scripts/w{M}_objects.txt, run scriptdis.py first),
without the animation/walk/sound noise (-v keeps everything). TID 'r' = room scripts, 's' = start scripts."""
import os, re, sys

args = sys.argv[1:]
verbose = "-v" in args
args = [a for a in args if a != "-v"]
m = args.pop(0)
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "scripts")
NOISE = re.compile(r"^[0-9A-F]{6} +(sound|sound_stop|wait|wait_stop|wait_anim|walk|anim_wait|face|freeze|unfreeze_wait|set_state|stop_path|set_hotspot|pal_cycle|path|show_obj)\b")
for t in args:
    f = {"r": "rooms", "s": "start"}.get(t, "objects")
    txt = open(os.path.join(root, f"w{m}_{f}.txt")).read()
    if f != "objects":
        print(txt)
        continue
    for blk in re.split(r"(?m)^(?=; ---- )", txt):
        if blk.startswith(f"; ---- object #{t} "):
            for ln in blk.split("\n"):
                if ln.strip() and (verbose or not NOISE.match(ln)):
                    print(ln[:230])
