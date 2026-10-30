#!/bin/sh
# script_play.sh ROUTE... (VM, worktree root, build/port built): play-test for #19.
# Plays each route with `--hle all` and `--hle none` and compares what the scripts decide:
# room, inventory list and the 29 story bytes at the end, plus a screenshot (cmpshot).
# Sprite/animation timing may differ (C code takes no CPU time); story state must not.
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
o=runs/script_play; mkdir -p "$o"
st=0
for r in "$@"; do
	n=$(basename "$r" .txt)
	for m in all none; do
		d="$o/$n.$m"; rm -rf "$d"; mkdir -p "$d"
		{ cat "$r"; echo "ram end"; echo "shot end"; } > "$d/route.txt"
		build/port/scoobyport --hle $m --script "$d/route.txt" --outdir "$d" --frames 0 > "$d/log" 2>&1
	done
	res=$(.venv/bin/python -I - "$o/$n.all/end.ram" "$o/$n.none/end.ram" <<'EOF'
import sys, struct
def st(p):
    r = open(p, 'rb').read()
    w = lambda a: struct.unpack_from('>H', r, a - 0xFF0000)[0]
    inv, a = [], 0xFF0A1E
    while w(a) < 0x8000 and len(inv) < 40:
        inv.append(w(a)); a += 2
    return w(0xFF06AC), inv, r[0x2A00:0x2A00 + 29].hex()
a, b = st(sys.argv[1]), st(sys.argv[2])
print(('SAME' if a == b else 'DIFF') + ' room %d inv %s story %s' % a + ('' if a == b else ' | none: room %d inv %s story %s' % b))
EOF
)
	shot=$(.venv/bin/python -I tools/py/cmpshot.py "$o/$n.all/end.ppm" "$o/$n.none/end.ppm" 2>&1 | tail -1)
	echo "$n: $res; screenshot: $shot"
	case "$res" in DIFF*) st=1;; esac
done
exit $st
