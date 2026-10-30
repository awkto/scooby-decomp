#!/usr/bin/env python3
"""Dump the raw words of an object's script:  scriptdump.py MYST TID [TID...]"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import objects as O

myst = int(sys.argv[1])
objs = {o["tid"]: o for o in O.load(myst)}
for t in map(int, sys.argv[2:]):
    o = objs[t]
    a = o["addr"]
    n = o["size"] // 2 + 4
    print(t, o["name"], hex(a), hex(o["size"]))
    print("  " + " ".join("%04x" % O.W(a + 2 * i) for i in range(n)))
