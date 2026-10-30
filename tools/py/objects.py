#!/usr/bin/env python3
"""Static object/script dump for the two mysteries (from the ROM only).

    objects.py [MYST] [IDX]      list objects (target id = IDX+3) with name, script size, and the text strings inside the script

Header (docs/formats.md): +0x20 names_base, +0x28 object table (8 bytes per object: u32 script offset,
u32 name offset), +0x2c scripts_base.  Object index i is `ram_objects` slot i, hit-test id i+3.
"""
import os, re, struct, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ROM = open(os.path.join(ROOT, "rom", "baserom.md"), "rb").read()
HDRS = [0xFCA0C, 0xFCA40]


def L(a): return struct.unpack(">I", ROM[a:a + 4])[0]
def W(a): return struct.unpack(">H", ROM[a:a + 2])[0]


def cstr(a):
    e = ROM.index(b"\0", a)
    return ROM[a:e].decode("latin1")


def load(myst):
    h = HDRS[myst]
    names, tab, sb = L(h + 0x20), L(h + 0x28), L(h + 0x2C)
    # object count: table runs until the scripts area (tab entries are contiguous); use the first
    # script offset boundary heuristic: table ends where it meets names (tab < sb)
    objs = []
    i = 0
    while True:
        so, no = L(tab + 8 * i), L(tab + 8 * i + 4)
        if no > 0x10000 or so > 0x100000:
            break
        objs.append((i, so, no))
        i += 1
        if i > 400:
            break
    out = []
    for n, (i, so, no) in enumerate(objs):
        name = cstr(names + no) if names + no < len(ROM) else "?"
        a = sb + so
        size = W(a)
        out.append(dict(idx=i, tid=i + 3, name=name, addr=a, size=size))
    return out


def texts(addr, size):
    data = ROM[addr:addr + size + 6]
    return [m.group().decode("latin1") for m in re.finditer(rb"[\x20-\x7e]{6,}", data)]


if __name__ == "__main__":
    myst = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    for o in load(myst):
        if len(sys.argv) > 2 and o["idx"] != int(sys.argv[2]):
            continue
        print(f"{o['tid']:3d} {o['name']:28s} script {o['addr']:06x} +{o['size']:#x}")
        if len(sys.argv) > 2:
            for t in texts(o["addr"], o["size"]):
                print("      ", t)
