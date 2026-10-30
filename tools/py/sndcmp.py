#!/usr/bin/env python3
"""Parse SCOOBY_SNDLOG mailbox logs into Z80 messages and compare two of them.
usage: sndcmp.py A.log B.log   (exit 1 if the message sequences differ)
The log has one "frame slot value" line per mailbox ring byte written by the 68k; the stream of
values is split into messages: $FF cmd args, where the arg count depends on cmd."""
import sys
from collections import Counter

NARGS = {0x0B: 12, 0x10: 1, 0x12: 1, 0x0C: 0, 0x0D: 0, 0x16: 0}
NAMES = {0x0B: 'init', 0x10: 'play', 0x12: 'stop', 0x0C: 'pause', 0x0D: 'resume', 0x16: 'stop_all'}


def parse(path):
    vals = [int(l.split()[2], 16) for l in open(path) if l.strip()]
    vals = vals[64:] if vals[:64] == [0] * 64 else vals   # skip the zero fill of Z80 RAM by the driver upload
    msgs, i = [], 0
    while i < len(vals):
        if vals[i] != 0xFF:
            msgs.append(('stray', vals[i]))
            i += 1
            continue
        cmd = vals[i + 1]
        n = NARGS.get(cmd, 0)
        msgs.append((NAMES.get(cmd, '%02X' % cmd), tuple(vals[i + 2:i + 2 + n])))
        i += 2 + n
    return msgs


def structural(path, rom='rom/baserom.md'):
    """Sound Test spam route: input edges are timing-sensitive (a 3-frame press is sometimes seen
    twice), so message sequences legitimately differ between modes.  Check what must hold anyway:
    no stray bytes, exactly one init, every play after the first is directly preceded by stop_all
    (Sound Test: stop_all then play), every id is a Sound Test id."""
    m = parse(path)
    ids = set(open(rom, 'rb').read()[0x1F7492:0x1F7492 + 87]) | {0x2F}
    bad = []
    if sum(1 for x in m if x[0] == 'init') != 1: bad.append('init count')
    if any(x[0] == 'stray' for x in m): bad.append('stray bytes')
    for k, x in enumerate(m):
        if x[0] == 'play' and x[1][0] not in ids: bad.append('bad id %02X at %d' % (x[1][0], k))
        if x[0] == 'play' and k > 3 and m[k - 1][0] != 'stop_all': bad.append('play without stop_all at %d' % k)
    print('%s: %d messages (%s), structural: %s' % (path, len(m), dict(Counter(x[0] for x in m)), bad or 'OK'))
    return not bad


if __name__ == '__main__' and sys.argv[1] == '--structural':
    sys.exit(0 if all([structural(p) for p in sys.argv[2:]]) else 1)
if __name__ == '__main__':
    a, b = parse(sys.argv[1]), parse(sys.argv[2])
    print('%s: %d messages; %s: %d messages' % (sys.argv[1], len(a), sys.argv[2], len(b)))
    for k, m in enumerate(a):
        if k >= len(b) or b[k] != m:
            print('first difference at message %d: %s vs %s' % (k, m, b[k] if k < len(b) else None))
            sys.exit(1)
    if len(a) != len(b):
        print('length differs')
        sys.exit(1)
    print('IDENTICAL:', dict(Counter(m[0] for m in a)))
