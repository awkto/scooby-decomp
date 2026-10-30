#!/bin/sh
# Gameplay-level regression gate (#32): replay the story routes (tools/py/story_routes.py) CLOSED-LOOP against the
# native port, once per --hle spec, and compare the per-step checkpoints (room, inventory, story bytes $FF2A00..+29).
# Closed loop = tools/py/story.py decides every next input from RAM it reads back from the live port (session.py
# with SCOOBY_BACKEND=port:SPEC), so C replacements that take ~0 emulated time cannot desync it.
#   port/tools/storytest.sh [ROUTE...]     (run on a VM after building build/port; default: the six short routes)
#   port/tools/storytest.sh long           the long walkthroughs: w0_full (Blake's Hotel, power-on to credits) and w1_part (Carnival up to the Dart + Ball); ~10 min each per --hle spec
# PASS per route = checkpoints identical for --hle none and --hle all.  The end room is also compared with the
# harness-verified header of tools/scripts/story/ROUTE.txt (reported as a note).
# On a none/all mismatch, STORYTEST_BISECT=1 bisects with --hle all,-FUNC and names the replacement whose removal restores agreement.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
routes=${*:-"w0_cafe_items w0_mine_wheels w0_office w0_kitchen w1_front_lot w1_boathouse"}
[ "$routes" = "long" ] && routes="w0_full w1_part"
out=runs/storytest; rm -rf "$out"; mkdir -p "$out/gen"
export STORY_PRESS_MIN=5 STORY_OUT="$out/gen" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
fail=0

run_one() { # ROUTE SPEC TAG
	d="$out/$1.$3"
	mkdir -p "$d"
	SCOOBY_BACKEND="port:$2" python3 tools/py/session.py serve "$d" >"$d.log" 2>&1 &
	sp=$!
	n=0; while [ ! -S "$d/sock" ] && [ $n -lt 100 ]; do sleep 0.2; n=$((n+1)); done
	timeout ${STORYTEST_TIMEOUT:-3600} python3 tools/py/story_routes.py "$d" "$1" --cp "$out/$1.$3.cp.json" >"$out/$1.$3.out" 2>&1
	python3 tools/py/session.py stop "$d" >/dev/null 2>&1
	kill $sp 2>/dev/null; wait $sp 2>/dev/null
}
cmp_cp() {
	python3 - "$1" "$2" <<'P'
import json, sys
a, b = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))
key = lambda x: (x["room"], x["inv"], x["story"])
bad = [(i, x, y) for i, (x, y) in enumerate(zip(a, b)) if key(x) != key(y)]
for i, x, y in bad[:3]:
    print("  step", i, x["label"], "\n    a:", key(x), "\n    b:", key(y))
if len(a) != len(b):
    print("  checkpoint counts differ", len(a), len(b)); bad.append(0)
sys.exit(1 if bad else 0)
P
}

for r in $routes; do
	run_one "$r" none none; run_one "$r" "${STORYTEST_HLE:-all}" all
	if [ ! -s "$out/$r.none.cp.json" ] || [ ! -s "$out/$r.all.cp.json" ]; then
		echo "FAIL $r: route did not complete (see $out/$r.*.out)"; fail=1; continue; fi
	if ! cmp_cp "$out/$r.none.cp.json" "$out/$r.all.cp.json"; then
		echo "FAIL $r: --hle none and --hle all diverge"; fail=1
		[ -n "${STORYTEST_BISECT:-}" ] && python3 port/tools/hlebisect.py "$r" 2>&1 | sed "s/^/  /"
		continue
	fi
	n=$(python3 -c "import json;print(len(json.load(open('$out/$r.none.cp.json'))))")
	end=$(python3 -c "import json;c=json.load(open('$out/$r.none.cp.json'))[-1];print(c['room'])")
	exp=$(grep '^# end state' "tools/scripts/story/$r.txt" | sed 's/.* room=\([0-9]*\).*/\1/')
	echo "PASS $r: none == all over $n checkpoints, end room $end"
	[ "$end" = "$exp" ] || echo "  note: end room differs from harness route header (room $exp)"
done
exit $fail
