#!/usr/bin/env python3
"""Insert difftest `capture` commands into a harness/port route at given frames.

  capture_at.py ROUTE ADDR N OUTPREFIX FRAME... > new_route.txt

At each FRAME (counted like the harness: every `run N` / `press B N` advances N frames)
the route gets `capture ADDR N OUTPREFIX_<k> 1`, i.e. the next N calls of ADDR are saved to
their own directory (a capture command replaces the previous one). Used for script handlers
whose interesting path happens on a few known frames (e.g. a verb `case` matching: frames
from the script dispatch trace, tools/py/scripttrace.sh).
"""
import sys


def main(argv):
    route, addr, n, prefix = argv[0], argv[1], int(argv[2]), argv[3]
    frames = sorted(int(f) for f in argv[4:])
    out, t, k = [], 0, 0
    for line in open(route):
        parts = line.split()
        cnt = None
        if parts and parts[0] == 'run' and len(parts) == 2:
            cnt = int(parts[1])
        elif parts and parts[0] == 'press' and len(parts) == 3:
            cnt = int(parts[2])
        while cnt is not None and k < len(frames) and t <= frames[k] < t + cnt:
            first = frames[k] - t
            if first:
                out.append(' '.join(parts[:-1] + [str(first)]))
            out.append('capture %s %d %s_%d 1' % (addr, n, prefix, k))
            t += first
            cnt -= first
            parts = parts[:-1] + [str(cnt)]
            k += 1
        if cnt is None:
            out.append(line.rstrip('\n'))
        else:
            if cnt:
                out.append(' '.join(parts[:-1] + [str(cnt)]))
            t += cnt
    print('\n'.join(out))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
