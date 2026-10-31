#!/bin/sh
# Interpreted-instruction profile over every gameplay route with --hle all (GitLab #24).
#   port/tools/ipprof.sh [GATE...]      run on a VM after building build/port; output runs/ipprof/GATE.PID.txt
# story_full = the long closed-loop walkthroughs w0_full and w1_full (power-on to the credits; none + all, ~50 min)
# Gates (default: all of them): story story_full mouse sound menus intro_w0 intro_w1 extra baseline
# (baseline = --hle none runs of intro_w0, intro_w1 and the menu tours, the denominators of the percentages)
# Each program writes its profile at exit (SCOOBY_IPPROF, src/ipprof.c).  The gate scripts also run
# --hle none / partial specs: the aggregator (tools/py/remaining68k.py) keeps only the runs whose spec is
# "all" (or the scoobyport default, which is all).  Then:
#   .venv/bin/python -I tools/py/remaining68k.py runs/ipprof > docs/remaining68k.md
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
out=runs/ipprof
mkdir -p "$out"
gates=${*:-"story story_full mouse sound menus intro_w0 intro_w1 extra baseline"}
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
P=build/port/scoobyport

route() { # GATE SCRIPT [HLE] : one route straight from power-on, --hle all by default
	SCOOBY_IPPROF="$out/$1.%d.txt" timeout 1800 $P --hle "${3:-all}" --script "$2" --outdir "$out/work" --frames 0 \
		>"$out/$1.log" 2>&1 || echo "  (exit $?)"
}

mkdir -p "$out/work"
for g in $gates; do
	echo "== $g"
	case $g in
	story) SCOOBY_IPPROF="$out/story.%d.txt" STORYTEST_HLE=all port/tools/storytest.sh ;;
	story_full) SCOOBY_IPPROF="$out/story_full.%d.txt" STORYTEST_HLE=all port/tools/storytest.sh long ;;
	mouse) SCOOBY_IPPROF="$out/mouse.%d.txt" MOUSETEST_STATE_HLE=all port/tools/mousetest.sh | tail -3 ;;
	sound) SCOOBY_IPPROF="$out/sound.%d.txt" port/tools/soundtest.sh | tail -6 ;;
	menus) SCOOBY_IPPROF="$out/menus.%d.txt" port/scripts/menus/tour.sh -h all 1 2 3 ;;
	intro_w0) route intro_w0 port/scripts/script/intro_w0.txt ;;
	intro_w1) route intro_w1 port/scripts/script/intro_w1.txt ;;
	baseline) route intro_w0 port/scripts/script/intro_w0.txt none; route intro_w1 port/scripts/script/intro_w1.txt none
		SCOOBY_IPPROF="$out/menus.%d.txt" port/scripts/menus/tour.sh -h none 1 2 3 ;;
	extra) # other recorded routes, listed separately in the report (not part of the #24 route set)
		for r in lobby boot_screens verbs inventory_play pointer_play script_talk password_screen capture; do
			route "extra_$r" "port/scripts/$r.txt"
		done ;;
	esac
done
echo "profiles: $(ls "$out"/*.txt | wc -l) files in $out"
build/port/scoobyport --list-hle > "$out/hle-list.txt"
