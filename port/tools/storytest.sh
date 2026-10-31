#!/bin/sh
# Gameplay-level regression gate (#32): replay the story routes (tools/py/story_routes.py) CLOSED-LOOP against the
# native port, once per --hle spec, and compare the per-step checkpoints (room, inventory, story bytes $FF2A00..+29).
# Closed loop = tools/py/story.py decides every next input from RAM it reads back from the live port (session.py
# with SCOOBY_BACKEND=port:SPEC), so C replacements that take ~0 emulated time cannot desync it.
#   port/tools/storytest.sh [ROUTE...]     (run on a VM after building build/port; default: the six short routes)
#   port/tools/storytest.sh long           the long walkthroughs: w0_full (Blake's Hotel, power-on to credits) and w1_full (Ha Ha Carnival, power-on to credits); ~10 min each per --hle spec
# PASS per route = checkpoints identical for --hle none and --hle all.  The end room is also compared with the
# harness-verified header of tools/scripts/story/ROUTE.txt (reported as a note).
# On a none/all mismatch, STORYTEST_BISECT=1 bisects with --hle all,-FUNC and names the replacement whose removal restores agreement.
# STORYTEST_WS=1 (#26, widescreen is presentation only): instead of none vs all, run each route twice with
# --hle ${STORYTEST_HLE:-all} and the native renderer, --options widescreen=4:3 and widescreen=16:9 (STORYTEST_WS_OPT),
# and require identical checkpoints INCLUDING a sha1 of the whole work RAM, and an identical per-frame hash of
# RAM + VRAM + CRAM + VSRAM + VDP registers + Z80 RAM at every frame (SCOOBY_STATE_LOG).
# STORYTEST_HIFPS=1 (#27, high frame rate is presentation only): the same with --options high_fps=off and
# high_fps=${STORYTEST_HIFPS_OPT:-144} (both widescreen=${STORYTEST_HIFPS_WS:-16:9}, so the wide view is
# interpolated too; every frame's in-betweens are rendered), plus an identical hash of every whole frame
# shown (SCOOBY_FRAME_HASH_LOG: the picture at k x T is the 60 Hz picture).
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
routes=${*:-"w0_cafe_items w0_mine_wheels w0_office w0_kitchen w1_front_lot w1_boathouse"}
[ "$routes" = "long" ] && routes="w0_full w1_full"
out=runs/storytest; rm -rf "$out"; mkdir -p "$out/gen"
export STORY_PRESS_MIN=5 STORY_OUT="$out/gen" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
fail=0

run_one() { # ROUTE SPEC TAG [PORT_OPTIONS]
	d="$out/$1.$3"
	mkdir -p "$d"
	SCOOBY_PORT_OPTS="${4:-}" SCOOBY_STATE_LOG="${4:+$out/$1.$3.state}" SCOOBY_FRAME_HASH_LOG="${4:+$out/$1.$3.fhash}" \
	SCOOBY_BACKEND="port:$2" python3 tools/py/session.py serve "$d" >"$d.log" 2>&1 &
	sp=$!
	n=0; while [ ! -S "$d/sock" ] && [ $n -lt 100 ]; do sleep 0.2; n=$((n+1)); done
	# write regenerated routes under runs/, never over the committed tools/scripts/story/*.txt (mousetest reads those)
	STORY_ROUTE_DIR="$d/routes" timeout ${STORYTEST_TIMEOUT:-3600} python3 tools/py/story_routes.py "$d" "$1" --cp "$out/$1.$3.cp.json" >"$out/$1.$3.out" 2>&1
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

cmp_ws() { # full checkpoint dicts (incl. ramsha) and the per-frame state logs
	python3 - "$1" "$2" <<'P'
import json, sys
a, b = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))
bad = [(i, x, y) for i, (x, y) in enumerate(zip(a, b)) if x != y]
for i, x, y in bad[:3]:
    print("  step", i, x["label"], "\n    4:3:", x, "\n    wide:", y)
if len(a) != len(b):
    print("  checkpoint counts differ", len(a), len(b)); bad.append(0)
sys.exit(1 if bad else 0)
P
}

if [ -n "${STORYTEST_HIFPS:-}" ]; then
	export SCOOBY_RENDERER=native SCOOBY_HIFPS_STATS=1
	ws=${STORYTEST_HIFPS_WS:-16:9}
	for r in $routes; do
		run_one "$r" "${STORYTEST_HLE:-all}" hz60 "widescreen=$ws,high_fps=off"
		run_one "$r" "${STORYTEST_HLE:-all}" hifps "widescreen=$ws,high_fps=${STORYTEST_HIFPS_OPT:-144}"
		if [ ! -s "$out/$r.hz60.cp.json" ] || [ ! -s "$out/$r.hifps.cp.json" ]; then
			echo "FAIL $r: route did not complete (see $out/$r.*.out)"; fail=1; continue; fi
		if ! cmp_ws "$out/$r.hz60.cp.json" "$out/$r.hifps.cp.json"; then
			echo "FAIL $r: checkpoints (incl. RAM sha1) differ between 60 Hz and high_fps"; fail=1; continue; fi
		f1=$(wc -l < "$out/$r.hz60.state")
		if ! cmp -s "$out/$r.hz60.state" "$out/$r.hifps.state"; then
			echo "FAIL $r: machine state differs between 60 Hz and high_fps at frame" \
			     "$(cmp "$out/$r.hz60.state" "$out/$r.hifps.state" | head -1)"; fail=1; continue; fi
		if ! cmp -s "$out/$r.hz60.fhash" "$out/$r.hifps.fhash" || [ ! -s "$out/$r.hifps.fhash" ]; then
			echo "FAIL $r: a whole frame differs between 60 Hz and high_fps:" \
			     "$(cmp "$out/$r.hz60.fhash" "$out/$r.hifps.fhash" | head -1)"; fail=1; continue; fi
		n=$(python3 -c "import json;print(len(json.load(open('$out/$r.hz60.cp.json'))))")
		e="$out/$r.hifps/harness.err"
		m=$(sed -n 's/^hifps: sprites: \(.*\)/\1/p' "$e" | tail -1)
		fm=$(sed -n 's/^hifps: frame-mode check: \(.*\)/\1/p' "$e" | tail -1)
		ip=$(sed -n 's/^hifps: frames interp *\([0-9]*\).*/\1/p' "$e" | tail -1)
		case "$fm" in *", 0 differ"*) ;; *) echo "FAIL $r: frame-mode check: $fm"; fail=1; continue ;; esac
		echo "PASS $r: 60 Hz == high_fps over $n checkpoints (RAM sha1), $f1 frames (machine state hash, whole-frame hash); ${ip:-0} frames interpolated; sprites: $m"
	done
	exit $fail
fi

if [ -n "${STORYTEST_WS:-}" ]; then
	export SCOOBY_RENDERER=native SCOOBY_WS_STATS=1
	for r in $routes; do
		run_one "$r" "${STORYTEST_HLE:-all}" ws43 "widescreen=4:3"
		run_one "$r" "${STORYTEST_HLE:-all}" wide "widescreen=${STORYTEST_WS_OPT:-16:9}"
		if [ ! -s "$out/$r.ws43.cp.json" ] || [ ! -s "$out/$r.wide.cp.json" ]; then
			echo "FAIL $r: route did not complete (see $out/$r.*.out)"; fail=1; continue; fi
		if ! cmp_ws "$out/$r.ws43.cp.json" "$out/$r.wide.cp.json"; then
			echo "FAIL $r: checkpoints (incl. RAM sha1) differ between 4:3 and widescreen"; fail=1; continue; fi
		f1=$(wc -l < "$out/$r.ws43.state"); f2=$(wc -l < "$out/$r.wide.state")
		if ! cmp -s "$out/$r.ws43.state" "$out/$r.wide.state"; then
			echo "FAIL $r: machine state differs between 4:3 and widescreen at frame" \
			     "$(cmp "$out/$r.ws43.state" "$out/$r.wide.state" | head -1)"; fail=1; continue; fi
		n=$(python3 -c "import json;print(len(json.load(open('$out/$r.ws43.cp.json'))))")
		wide=$(grep -h "^ws: width" "$out/$r.wide/harness.err" | tail -1)
		echo "PASS $r: 4:3 == widescreen over $n checkpoints (RAM sha1) and $f1 frames (machine state hash); $wide"
	done
	exit $fail
fi

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
