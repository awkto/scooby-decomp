#!/usr/bin/env python3
"""difftest --poke variants that point a captured script-handler state at OTHER ops of the
same kind (#19), so the handler's rare paths get tested without a route that reaches them.

  script_pokes.py OP [MAX]      prints `--poke SPEC` arguments, one variant per line

A captured entry state of handler OP (A5 = some op of that type, Z flag = mode) is replayed
with A5 moved to up to MAX (default 40) other ops of type OP from both mysteries, picked to
cover distinct operand patterns (condition modes, set modes, verbs, ...). Extra RAM/register
pokes per op type put the VM into the state that makes the interesting path run:
  01 case_verb    script_flags = 2 (verb pass), D7 = the case's verb, target2 = its "with"
                  object or 0, verb_state bit 0 clear/set
  02 talk_choice  script_flags = 4 (collect), talk count 0..5
  13 on_event     script_flags = $10 / 0
other ops: A5 only (the state's mode decides scan/execute).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import scriptdis as S  # noqa: E402


def ops_of(rom, op):
    out = []
    for wi in (0, 1):
        w = S.World(rom, wi)
        for label, hdr, b0, b1 in S.script_list(w):
            d = S.Dis(w)
            d.block(b0, b1, 0)
            for a, (o, n) in d.starts.items():
                if o == op:
                    out.append(a)
    return sorted(set(out))


def main(argv):
    op = int(argv[0], 16)
    mx = int(argv[1]) if len(argv) > 1 else 40
    rom = open(S.ROM_PATH, 'rb').read()
    u16 = lambda a: struct.unpack_from('>H', rom, a)[0]
    addrs = ops_of(rom, op)
    # one op per distinct "shape" first, then fill up evenly
    key = {
        0x01: lambda a: (u16(a + 2), u16(a + 4) != 0),
        0x03: lambda a: u16(a + 0xE),
        0x04: lambda a: u16(a + 0xE),
        0x08: lambda a: u16(a + 0xA),
        0x0B: lambda a: u16(a + 2) >> 15,
        0x05: lambda a: (u16(a + 2) >> 15, u16(a + 4) in (0, 1)),
    }.get(op, lambda a: 0)
    seen, pick = set(), []
    for a in addrs:
        k = key(a)
        if k not in seen:
            seen.add(k)
            pick.append(a)
    rest = [a for a in addrs if a not in pick]
    step = max(1, len(rest) // max(1, mx - len(pick)))
    pick += rest[::step]
    pick = pick[:mx]
    for i, a in enumerate(pick):
        spec = 'A5=0x%X' % a
        if op == 0x01:
            spec += ',B:FF0AC9=2,D7=%d,W:FF06B6=%d,B:FF09DE=%d' % (
                u16(a + 2), u16(a + 4) if i % 3 else 0, (i >> 1) & 1)
        elif op == 0x02:
            spec += ',B:FF0AC9=4,W:FF080E=%d' % (i % 6)
        elif op == 0x13:
            spec += ',B:FF0AC9=%d' % (0x10 if i % 2 == 0 else 0x90)
        print('--poke %s' % spec)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
