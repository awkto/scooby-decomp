#!/usr/bin/env python3
"""Generate a harness script that walks Shaggy around randomly after a route and logs
Shaggy/Scooby positions every 3 frames (peek.log), for checking the zone-map decoding
against real movement (walkmap.py png --points).

usage: walkexplore.py ROUTE SEED SEGMENTS [BUTTON_PROB] > script.txt
  BUTTON_PROB (default 0): chance per segment of an A/B/C tap (opens the verb menu /
  pointer, swaps verb pages, triggers scripts) - for difftest coverage.
"""
import random
import sys

route, seed, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
bprob = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
random.seed(seed)
out = [l for l in open(route).read().splitlines()
       if not l.startswith(('shot', 'save', 'peek', 'ram', 'vdp', 'roomsave'))]
dirs = ['U', 'D', 'L', 'R', 'UL', 'UR', 'DL', 'DR']
d = random.choice(dirs)
for i in range(n):
    if random.random() < 0.08:
        d = random.choice(dirs)
    if random.random() < bprob:
        out.append(f'press {random.choice("ABC")} 2')
        out.append(f'run {random.randint(1, 30)}')
    out.append(f'press {d} 3')
    out += ['peek sx FF04DC 2', 'peek sy FF04F4 2', 'peek bx FF04E0 2', 'peek by FF04F8 2',
            'peek room FF06AC 2']
out.append('shot end')
print('\n'.join(out))
