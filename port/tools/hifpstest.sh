#!/bin/sh
# High frame rate test (#27, docs/subsystems/hifps.md): interpolated in-between pictures are
# presentation only.
#   port/tools/hifpstest.sh            (VM, after building build/port; ~3 min)
# Routes: the Lobby walk (port/scripts/hifps/lobby_walk.txt: title -> Lobby, walk right then
# left; the camera scrolls on the way back) and a Boardwalk walk (warp tools/scripts/rooms/w1_r24.txt,
# then port/scripts/hifps/boardwalk_tail.txt). Each runs with high_fps off and at every rate of
# HIFPSTEST_RATES (default "120 144 240"), in every view of HIFPSTEST_VIEWS (default "4:3 16:9").
# Checks per route, view and rate:
#   - machine state (RAM, VRAM, CRAM, VSRAM, VDP registers, Z80 RAM) identical to the run with
#     high_fps off at every frame (SCOOBY_STATE_LOG);
#   - every whole frame (the picture shown at k x T) identical to the 60 Hz run's
#     (SCOOBY_FRAME_HASH_LOG: a hash of each presented frame);
#   - the frame-mode check: 0 interpolated frames whose no-step render differs from the raster frame;
#   - matched actor sprite pieces on gameplay frames >= HIFPSTEST_MIN_MATCH % (default 95).
# Reported: the counters (frames interpolated / still / held by reason), render time per
# in-between (p50 / p99 / max). In-between pictures of a short stretch of each walk are written
# to runs/hifpstest/ROUTE.VIEW/RATE/s<frame>_<j>.png (s<k>_00 = the whole frame k, s<k>_<j> =
# picture j of n on the way from k to k+1), in presentation order.
# Then the present clock: the Lobby walk played in real time (--realtime-script, no audio, no
# vsync: the VM has no display) with SCOOBY_PRESENT_LOG; spacing histogram of the presents
# against T / n, and the whole frames' due times (k x T).
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
out=runs/hifpstest
rm -rf "$out"; mkdir -p "$out/routes"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SCOOBY_RENDERER=native
P=build/port/scoobyport
rates=${HIFPSTEST_RATES:-"120 144 240"}
views=${HIFPSTEST_VIEWS:-"4:3 16:9"}
min_match=${HIFPSTEST_MIN_MATCH:-95}
fail=0

cp port/scripts/hifps/lobby_walk.txt "$out/routes/lobby.txt"
{ grep -v '^save ' tools/scripts/rooms/w1_r24.txt; cat port/scripts/hifps/boardwalk_tail.txt; } > "$out/routes/boardwalk.txt"
# dumped stretches (emulated frame numbers, ~3 s each) at HIFPSTEST_DUMP_RATE (default 240): the
# Lobby walk back to the left (Shaggy 0.68 px a frame; the camera follows from frame ~3035), the
# Boardwalk walk to the right with the camera following (1.33 px a frame)
window_lobby=${HIFPSTEST_WIN_LOBBY:-2930-3110}
window_boardwalk=${HIFPSTEST_WIN_BOARDWALK:-5520-5700}
dump_rate=${HIFPSTEST_DUMP_RATE:-240}

stat() { sed -n "$2" "$1" | head -1; }

[ -n "${HIFPSTEST_PRESENT_ONLY:-}" ] && routes="" || routes="lobby boardwalk"
for r in $routes; do
	eval win=\$window_$r
	for v in $views; do
		tag=$r.$(echo "$v" | tr ':' 'x')
		d="$out/$tag"; mkdir -p "$d/off"
		SCOOBY_STATE_LOG="$d/off.state" SCOOBY_FRAME_HASH_LOG="$d/off.hash" nice $P --options "widescreen=$v" \
			--script "$out/routes/$r.txt" --outdir "$d/off" --frames 0 >"$d/off.log" 2>&1
		for h in $rates; do
			mkdir -p "$d/$h"
			dump=; [ "$h" = "$dump_rate" ] && dump="$d/$h,$win"
			SCOOBY_STATE_LOG="$d/$h.state" SCOOBY_FRAME_HASH_LOG="$d/$h.hash" SCOOBY_HIFPS_DUMP="$dump" \
				nice $P --options "widescreen=$v,high_fps=$h" --script "$out/routes/$r.txt" --outdir "$d/$h" \
				--frames 0 >"$d/$h.log" 2>&1
			ls "$d/$h" | grep '^s.*\.ppm$' | xargs -r -P 4 -I{} sh -c 'convert "$1/$2" "$1/${2%.ppm}.png" && rm -f "$1/$2"' _ "$d/$h" {}
			st=same; cmp -s "$d/off.state" "$d/$h.state" || st=DIFF; [ -s "$d/$h.state" ] || st=MISSING
			fh=same; cmp -s "$d/off.hash" "$d/$h.hash" || fh=DIFF; [ -s "$d/$h.hash" ] || fh=MISSING
			frames=$(wc -l < "$d/$h.hash")
			m=$(stat "$d/$h.log" 's/.*matched (\([0-9.]*\)%).*/\1/p')
			fm=$(stat "$d/$h.log" 's/^hifps: frame-mode check: \([0-9]*\) frames, \([0-9]*\) differ.*/\1 \2/p')
			rt=$(stat "$d/$h.log" 's/^hifps: render per in-between: \(.*\)/\1/p')
			ip=$(stat "$d/$h.log" 's/^hifps: frames interp *\([0-9]*\).*/\1/p')
			ok=PASS
			[ "$st" = same ] && [ "$fh" = same ] || ok=FAIL
			[ "${fm#* }" = 0 ] || ok=FAIL
			awk -v m="${m:-0}" -v t="$min_match" 'BEGIN { exit !(m + 0 >= t + 0) }' || ok=FAIL
			[ $ok = PASS ] || fail=1
			echo "$ok $tag $h Hz: state $st, whole frames $fh ($frames), matched ${m:-?}%, interpolated ${ip:-0} frames, frame-mode check ${fm:-?} (checked differing), render $rt"
		done
		grep "^hifps: frames\|^hifps: sprites\|^hifps: scroll" "$d/$(echo $rates | awk '{print $NF}').log" | sed 's/^/    /'
	done
done

# ---- present clock: real-time play, spacing of the presents
d="$out/present"; mkdir -p "$d"
grep -v '^press R 200$\|^press L 200$\|^run 10$' port/scripts/hifps/lobby_walk.txt > "$d/to_lobby.txt"
echo "save hifps_lobby" >> "$d/to_lobby.txt"
$P --script "$d/to_lobby.txt" --outdir "$d" --frames 0 >"$d/to_lobby.log" 2>&1
printf 'load hifps_lobby\npress R 200\npress L 200\nrun 30\n' > "$d/walk.txt"
for h in $rates; do
	SCOOBY_PRESENT_LOG="$d/present.$h.csv" timeout 120 $P --realtime-script --mute --no-vsync \
		--options "high_fps=$h" --script "$d/walk.txt" --outdir "$d" --frames 0 >"$d/present.$h.log" 2>&1
	python3 - "$d/present.$h.csv" "$h" <<'P' || fail=1
import sys, statistics
rows = [l.split() for l in open(sys.argv[1])]
rate = int(sys.argv[2])
T = 1e6 / 59.922743404                      # us
n = round(rate * T / 1e6)
step = T / n
t = [int(r[0]) for r in rows[30:]]          # before SDL_RenderPresent; skip the start-up
d = [b - a for a, b in zip(t, t[1:])]
late = [int(r[0]) - int(r[2]) for r in rows[30:] if int(r[2])]
whole = [int(r[2]) for r in rows[30:] if r[3] == 'W' and int(r[2])]
wd = [b - a for a, b in zip(whole, whole[1:])]
within = sum(abs(x - step) <= 1000 for x in d) / len(d) * 100
hist = {}
for x in d:
    k = round((x - step) / 500) * 0.5      # ms off the ideal spacing, 0.5 ms buckets
    hist[k] = hist.get(k, 0) + 1
kinds = {k: sum(r[3] == k for r in rows) for k in "WIH"}
ok = within >= 95 and abs(statistics.mean(wd) - T) < 50
print("%s present %d Hz: n = %d, %d presents (%d whole, %d interpolated, %d held), spacing mean %.3f ms (ideal %.3f), "
      "sd %.3f ms, %.1f%% within 1 ms; late vs due: median %.2f ms, p99 %.2f ms; whole frames every %.3f ms (T %.3f)"
      % ("PASS" if ok else "FAIL", rate, n, len(rows), kinds['W'], kinds['I'], kinds['H'], statistics.mean(d) / 1000,
         step / 1000, statistics.pstdev(d) / 1000, within, statistics.median(late) / 1000,
         sorted(late)[int(len(late) * 0.99)] / 1000, statistics.mean(wd) / 1000, T / 1000))
print("    spacing - ideal (ms): " + "  ".join("%+.1f:%d" % (k, hist[k]) for k in sorted(hist)))
sys.exit(0 if ok else 1)
P
done
[ $fail = 0 ] && echo "PASS hifpstest" || echo "FAIL hifpstest"
exit $fail
