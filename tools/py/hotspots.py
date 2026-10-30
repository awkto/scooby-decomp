#!/usr/bin/env python3
"""hotspots.py MYST [TID...] -- static hotspot rectangle (map cells) of each object, from the ROM.

Object init record +2 = type id = overlay index + 1 (non-actors); overlay table at header+0x24 (offsets from header+0x1C):
u16 x, y, w, h (cells), ... .  The hit test the pointer uses is the same rectangle, so a crosshair at world pixel
(8*x .. 8*(x+w), 8*y .. 8*(y+h)) selects the object (world = screen + camera, $FF06CA/$FF06CE)."""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import objects as O


def records(m):
    h = O.HDRS[m]
    a, end = O.L(h + 4), O.L(h + 8)
    i = 0
    while a < end:
        fl = O.ROM[a + 0xC]
        yield i + 3, a, bool(fl & 2)
        a += 18 if fl & 2 else 14
        i += 1


def hotspot(m, tid):
    """(x0, y0, x1, y1) in world pixels or None"""
    h = O.HDRS[m]
    for t, a, actor in records(m):
        if t != tid:
            continue
        typ = O.W(a + 2)
        if actor or typ == 0:
            return None
        base = O.L(h + 0x1C)
        off = O.L(O.L(h + 0x24) + 4 * (typ - 1)) if False else None
        tab = O.L(h + 0x24)
        ov = base + O.L(tab + 4 * (typ - 1))
        x, y, w, hh = (O.W(ov + 2 * k) for k in range(4))
        return (8 * x, 8 * y, 8 * (x + w), 8 * (y + hh))
    return None


if __name__ == "__main__":
    m = int(sys.argv[1])
    tids = [int(a) for a in sys.argv[2:]] or [t for t, a, ac in records(m)]
    for t in tids:
        print(t, hotspot(m, t))


def rect_for_type(m, typ):
    """world-pixel rectangle of overlay/hotspot `typ` (object record +2) or None"""
    if typ == 0:
        return None
    h = O.HDRS[m]
    ov = O.L(h + 0x1C) + O.L(O.L(h + 0x24) + 4 * (typ - 1))
    x, y, w, hh = (O.W(ov + 2 * k) for k in range(4))
    return (8 * x, 8 * y, 8 * (x + w), 8 * (y + hh))
