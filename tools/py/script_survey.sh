#!/bin/sh
# script_survey.sh OUTDIR [ROUTE...] (VM, worktree root, build/port built): which script
# handlers (#19) does each route reach? Plays every route with frameprof (pure 68k) watching
# every op handler (script_op_table $2354), every sub-op handler (script_subop_table $2EB4)
# and the extra entries in $EXTRA ("ADDR=name ..."), 4 routes in parallel. Writes
# OUTDIR/<route>.txt (frameprof output) and OUTDIR/hits.txt: "$ADDR name hits route" for
# every watched address a route reached.
o=$1; shift
mkdir -p "$o"
routes=${*:-"port/scripts/capture.txt port/scripts/verbs.txt port/scripts/script_talk.txt $(ls tools/scripts/natural/*.txt tools/scripts/story/*.txt)"}
.venv/bin/python -I - "${EXTRA:-}" > "$o/watch.args" <<'P'
import struct, sys
rom = open('rom/baserom.md', 'rb').read()
seen = {}
for base, n, tag in ((0x2354, 34, 'op'), (0x2EB4, 62, 'sub')):
    for i in range(n):
        a = struct.unpack_from('>I', rom, base + 4 * i)[0]
        seen.setdefault(a, '%s%02X' % (tag, i))
for x in sys.argv[1].split():
    a, _, nm = x.partition('=')
    seen.setdefault(int(a, 16), nm or a)
print(' '.join('--watch 0x%X=%s' % (a, nm) for a, nm in sorted(seen.items())))
P
export o
for r in $routes; do echo "$r"; done |
	xargs -P 4 -I{} sh -c 'n=$(basename "{}" .txt); build/port/frameprof --script "{}" $(cat "$o/watch.args") > "$o/$n.txt" 2>&1'
: > "$o/hits.txt"
for f in "$o"/*.txt; do
	n=$(basename "$f" .txt)
	case $n in hits) continue;; esac
	grep -E '^\$[0-9A-F]{6} ' "$f" | while read -r a nm first last hits; do
		[ "$hits" -gt 0 ] 2>/dev/null && echo "$a $nm $hits $n" >> "$o/hits.txt"
	done
done
sort "$o/hits.txt" -o "$o/hits.txt"
