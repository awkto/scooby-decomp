#!/usr/bin/env python3
"""route_marks.py ROUTE STEP > out: insert `shot fNNNNN` and `peek` lines (room, script
flags, vblank flags) every STEP frames of a route, for comparing two --hle runs (#19 debugging)."""
import sys
route, step = sys.argv[1], int(sys.argv[2])
t, nxt = 0, step
for line in open(route):
    p = line.split()
    n = int(p[-1]) if p and p[0] in ('run', 'press') and len(p) >= 2 else 0
    while n and t + n >= nxt:
        k = nxt - t
        if k:
            print(' '.join(p[:-1] + [str(k)]))
        print('shot f%05d\npeek room FF06AC\npeek vbf FF09DC 1\npeek sf FF0AC9 1' % nxt)
        t += k; n -= k; p = p[:-1] + [str(n)]; nxt += step
    if p and (n or p[0] not in ('run', 'press')):
        print(' '.join(p) if p[0] in ('run', 'press') else line.rstrip('\n'))
    t += n
