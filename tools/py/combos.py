#!/usr/bin/env python3
"""List every (verb, second-object) handler pair found in the object scripts:
    combos.py MYST     -> "target-object: USE/GIVE item" lines (obj2 = hit-test id of the second object)
Heuristic: scans each script for the match-header `0001 <verb 1..10> <obj2> <len>` (script_op_01)."""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import objects as O

VN = {1: "Pull", 2: "Take", 3: "Open", 4: "Shut", 5: "Use", 6: "Give", 7: "Push", 8: "Talk", 9: "Eat", 10: "Look"}
myst = int(sys.argv[1])
objs = O.load(myst)
names = {o["tid"]: o["name"] for o in objs}
for o in objs:
    a, n = o["addr"], o["size"]
    seen = []
    for off in range(6, n, 2):
        if O.W(a + off) == 1 and 1 <= O.W(a + off + 2) <= 10:
            v, o2, ln = O.W(a + off + 2), O.W(a + off + 4), O.W(a + off + 6)
            if 0 < ln <= n and (o2 == 0 or o2 in names):
                seen.append((v, o2))
    for v, o2 in dict.fromkeys(seen):
        if o2:
            print(f"{o['tid']:3d} {o['name']:22s} {VN[v]:5s} with {o2:3d} {names.get(o2,'?')}")
