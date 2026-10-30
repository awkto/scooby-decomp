#!/usr/bin/env python3
"""--poke variants for difftest of the password code (GitLab #21, port/tools/dt-menus).

The menu tours only run each password routine a handful of times; this makes more states by patching work RAM:
  show   room ($FF06AC), the 29 state bytes ($FF2A00) and the stale byte $FF003D          (password_show)
  check  the password text at $FF08BC: valid passwords of random states, and the same with one character
         changed (bad checksum), plus the stale symbols $FF0070/71                      (password_check)
  raw    the 32 raw bytes at $FF001E                                                    (password_scramble, _unscramble,
         password_accept)
Usage: menus_pokes.py KIND [COUNT] -> prints  --poke "..."  arguments (one per variant).
"""
import os, random, sys
sys.path.insert(0, os.path.dirname(__file__))
import password as pw

kind = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 100
r = random.Random(21)


def longs(addr, data):
    data = bytes(data) + bytes((-len(data)) % 4)
    return [f"L:{addr + 4 * i:X}=0x{int.from_bytes(data[4 * i:4 * i + 4], 'big'):X}" for i in range(len(data) // 4)]


def state():
    k = r.randrange(4)
    if k == 0: return [r.randrange(256) for _ in range(29)]
    if k == 1: return [0] * 29
    if k == 2: return [255] * 29
    return [r.choice((0, 255, r.randrange(256))) for _ in range(29)]


out = []
for i in range(n):
    room = r.randrange(32)
    s = state()
    if kind == "show":
        items = [f"W:FF06AC={room}", f"B:FF003D={r.randrange(256)}"] + longs(0xFF2A00, s[:28]) + [f"B:FF2A1C={s[28]}"]
    elif kind == "check":
        p = pw.encode(room, s)
        if i % 3 == 0:
            k = r.randrange(50)
            p = p[:k] + r.choice([c for c in pw.ALPHA if c != p[k]]) + p[k + 1:]
        buf = bytearray(64)
        for row in range(2):
            for k in range(25):
                buf[31 * row + k + k // 5] = ord(p[25 * row + k])
            for g in range(5):
                buf[31 * row + g * 6 + 5] = 0x20
        items = longs(0xFF08BC, buf[:64]) + [f"B:FF0070={r.randrange(32)}", f"B:FF0071={r.randrange(32)}"]
    elif kind == "raw":
        raw = bytes([r.randrange(256) for _ in range(32)])
        items = longs(0xFF001E, raw)
    else:
        sys.exit("kind?")
    out.append('--poke "' + ",".join(items) + '"')
print(" ".join(out))
