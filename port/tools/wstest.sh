#!/bin/sh
# Widescreen test (#26, docs/subsystems/widescreen.md): every room of both mysteries drawn wide.
#   port/tools/wstest.sh [ROOM...]      (VM, after building build/port; ROOM = w0_r02 ...; default all 51)
# For each warp script tools/scripts/rooms/ROOM.txt: warp there, then walk left and right with
# screenshots, once with --options widescreen=${WSTEST_OPT:-16:9} and once 4:3. Checks per room:
#   - the machine state (RAM, VRAM, CRAM, VSRAM, VDP registers, Z80 RAM) is the same at every frame
#     in both runs (SCOOBY_STATE_LOG): widescreen is presentation only;
#   - counters of the wide run for the room: lost sprites, unneeded bars, wide lines whose 4:3
#     part differs from the 4:3 picture: all must be 0, and the room must be shown wide. Wrapped
#     (hidden) sprite pieces, frames with bars (rooms narrower than the view) and the frames shown
#     4:3 by rule (e.g. planes-not-map: the 4:3 window of the planes is not the room map, which
#     the extra columns are drawn from) are reported.
# Screenshots: runs/wstest/ROOM/*.png (the 4:3 run's are not kept).
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
out=runs/wstest
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SCOOBY_RENDERER=native
opt=${WSTEST_OPT:-16:9}

one() { # ROOM
	r=$1; d="$out/$r"; mkdir -p "$d/wide" "$d/43"
	{ grep -v '^save ' "tools/scripts/rooms/$r.txt"
	  cat <<'EOF'
run 60
shot a_arrive
press L 150
run 20
shot b_left
press R 320
run 20
shot c_right
press R 200
run 20
shot d_right2
press L 120
run 20
shot e_back
EOF
	} > "$d/script.txt"
	SCOOBY_STATE_LOG="$d/wide.state" nice build/port/scoobyport --options "widescreen=$opt,ws_stats=1" \
		--script "$d/script.txt" --outdir "$d/wide" --frames 0 >"$d/wide.log" 2>&1
	SCOOBY_STATE_LOG="$d/43.state" nice build/port/scoobyport --options widescreen=4:3 \
		--script "$d/script.txt" --outdir "$d/43" --frames 0 >"$d/43.log" 2>&1
	for f in "$d"/wide/*.ppm; do convert "$f" "$d/$(basename "${f%.ppm}").png"; done
	rm -rf "$d/43" "$d/wide"
}
if [ "${1:-}" = "--one" ]; then           # worker (4 in parallel)
	one "$2"
	exit 0
fi
rooms=${*:-$(ls tools/scripts/rooms/ | sed 's/\.txt$//')}
rm -rf "$out"; mkdir -p "$out"
for r in $rooms; do echo "$r"; done | xargs -P 4 -n 1 sh "$0" --one

fail=0
printf '%-7s %-22s %5s %5s %5s %7s %5s %6s %6s %-5s %s\n' room name wide frames bars wrapped lost unneed centre state "4:3 frames in the room (rule=frames)"
for r in $rooms; do
	d="$out/$r"
	w=${r%%_*}; n=${r#*_r}
	name=$(head -1 "tools/scripts/rooms/$r.txt" | sed "s/.*room [0-9]* '\(.*\)' of.*/\1/; s/.*room [0-9]* \"\(.*\)\" of.*/\1/")
	line=$(grep "^ws-room $w r$n:" "$d/wide.log")
	st=same
	cmp -s "$d/wide.state" "$d/43.state" || st=DIFF
	[ -s "$d/wide.state" ] || st=MISSING
	get() { echo "$line" | sed -n "s/.* $1 \([0-9]*\).*/\1/p"; }
	centre=$(sed -n 's/^ws: centre: \([0-9]*\) of.*/\1/p' "$d/wide.log")
	frames=$(get frames); wide=$(get wide); mm=$(get mismatch); wr=$(get wrapped); lost=$(get lost)
	bars=$(get bars); un=$(get unneeded)
	why=$(echo "$line" | sed 's/.*;//')
	printf '%-7s %-22.22s %5s %5s %5s %7s %5s %6s %6s %-5s %s\n' "$r" "$name" "${wide:--}" "${frames:--}" "${bars:--}" \
		"${wr:--}" "${lost:--}" "${un:--}" "${centre:--}" "$st" "$why"
	if [ -z "$line" ] || [ "$st" != same ] || [ "${lost:-1}" != 0 ] || [ "${un:-1}" != 0 ] || \
	   [ "${centre:-1}" != 0 ] || [ "${wide:-0}" = 0 ]; then
		fail=1
	fi
done
# mouse clicks in the margins of the wide view walk to that world point (port/scripts/ws/margin.txt)
d="$out/margin"; mkdir -p "$d"
cat port/scripts/lobby.txt port/scripts/ws/margin.txt > "$d/script.txt"
build/port/scoobyport --options "widescreen=$opt" --script "$d/script.txt" --outdir "$d" --frames 0 >"$d/log" 2>&1
pk() { sed -n "s/^[0-9]* $1 \([0-9a-f]*\)/\1/p" "$d/peek.log" | head -1; }
sx1=$((0x$(pk sx1))); sy1=$((0x$(pk sy1))); sx2=$((0x$(pk sx2)))
if [ "$opt" = 16:9 ]; then
	if [ $sx1 = 109 ] && [ $sy1 = 134 ] && [ $sx2 -gt 330 ]; then
		echo "PASS margin clicks: left margin -> Shaggy at ($sx1,$sy1) (world point of the click), right margin -> x $sx2"
	else
		echo "FAIL margin clicks: left -> ($sx1,$sy1), want (109,134); right -> x $sx2, want > 330"; fail=1
	fi
fi
for f in "$d"/*.ppm; do convert "$f" "${f%.ppm}.png"; rm -f "$f"; done
[ $fail = 0 ] && echo "PASS wstest: all rooms wide, state identical to 4:3, counters 0" || echo "FAIL wstest"
exit $fail
