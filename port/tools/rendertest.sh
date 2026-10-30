#!/bin/sh
# Native renderer vs GPGX, pixel for pixel, on the gate routes (GitLab #25).
#   port/tools/rendertest.sh [GROUP...]     (on a VM after building build/port)
# Groups (default: all): mouse menus intros extra story
# Every program run here has SCOOBY_RENDERCMP=N: the native renderer draws each frame alongside
# GPGX from the same VDP state (render/render_emu.c), and the two frames are compared. "Sampled"
# frames are every Nth frame plus the 8 frames after every scene change (the interrupt hooks
# change or a third of the picture changes); every other frame is compared too and reported.
#   RENDERTEST_N=N            sampling period (default 30)
#   RENDERTEST_SPLIT=raster   (default) the bottom of the screen is drawn from the live registers the
#                             68k raster split wrote: every frame must be identical (step 1)
#   RENDERTEST_SPLIT=model    the bottom band is drawn from the band model (the values the C
#                             hblank_verb_bar records, SCOOBY_RENDER_SPLIT=model) while GPGX
#                             shows the real split; only the band's first line may differ, in
#                             frames where the 68k's last write came after that line (render.md)
#   RENDERTEST_HLE="all none" --hle specs for menus/intros/extra (story and mouse run both anyway)
# PASS = no compared frame differs (model: see above). Mismatching frames are dumped as native/gpgx/diff PPMs under
# runs/rendertest/dumps/<pid>/ (first 10 per program).
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
out=runs/rendertest
rm -rf "$out"; mkdir -p "$out/logs" "$out/dumps" "$out/work"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
export SCOOBY_RENDERCMP="${RENDERTEST_N:-30},$root/$out/dumps/%d"
unset SCOOBY_RENDERER
if [ "${RENDERTEST_SPLIT:-raster}" = model ]; then export SCOOBY_RENDER_SPLIT=model; else unset SCOOBY_RENDER_SPLIT; fi
groups=${*:-"mouse menus intros extra story"}   # story last: it regenerates tools/scripts/story/*.txt, which mousetest reads
P=build/port/scoobyport

route() { # NAME SCRIPT HLE
	timeout 1800 $P --hle "$3" --script "$2" --outdir "$out/work" --frames 0 >"$out/logs/$1.$3.log" 2>&1
}

for g in $groups; do
	echo "== $g"
	case $g in
	story) STORYTEST_HLE=all port/tools/storytest.sh | sed 's/^/  /'
		for f in runs/storytest/*/harness.err; do cp "$f" "$out/logs/story.$(basename "$(dirname "$f")").log"; done ;;
	mouse) port/tools/mousetest.sh > "$out/mousetest.txt" 2>&1; grep -v "^PASS" "$out/mousetest.txt" | sed "s/^/  /"
		find runs/mousetest -name log.txt | while read -r f; do
			cp "$f" "$out/logs/mouse.$(echo "$f" | sed 's#runs/mousetest/##; s#/#.#g')"; done ;;
	menus) for h in ${RENDERTEST_HLE:-all none}; do
			port/scripts/menus/tour.sh -h "$h" 1 2 3 >/dev/null 2>&1; cp runs/tour/log "$out/logs/menus.$h.log"; done ;;
	intros) for h in ${RENDERTEST_HLE:-all none}; do
			route intro_w0 port/scripts/script/intro_w0.txt "$h"; route intro_w1 port/scripts/script/intro_w1.txt "$h"; done ;;
	extra) for h in ${RENDERTEST_HLE:-all none}; do
			for r in boot_screens lobby verbs inventory_play pointer_play script_talk password_screen sound_test capture; do
				route "extra_$r" "port/scripts/$r.txt" "$h"; done; done ;;
	esac
done

# every program prints "rendercmp: sampled S frames, D differ; all A frames, B differ" at exit
python3 - "$out/logs" <<'P'
import os, re, sys
d = sys.argv[1]
tot = [0, 0, 0, 0, 0]; bad = []; progs = 0
model = os.environ.get("SCOOBY_RENDER_SPLIT") == "model"
for f in sorted(os.listdir(d)):
    for line in open(os.path.join(d, f), errors="replace"):
        m = re.search(r"rendercmp: sampled (\d+) frames, (\d+) differ; all (\d+) frames, (\d+) differ; split-line-only (\d+)", line)
        if m:
            progs += 1
            v = list(map(int, m.groups()))
            tot = [a + b for a, b in zip(tot, v)]
            if v[3] > (v[4] if model else 0):
                bad.append(f"{f}: {line.strip()}")
print(f"programs: {progs}")
print(f"sampled frames compared: {tot[0]}, differing: {tot[1]}")
print(f"all frames compared:     {tot[2]}, differing: {tot[3]}")
print(f"  of which only the first verb-bar line differs (band model, 68k beam race): {tot[4]}")
for b in bad[:20]:
    print("  " + b)
# raster mode: every frame must be identical. Band model: the only allowed difference is the first
# line of the band in frames where the 68k's final register write came late (render.md).
allowed = tot[4] if model else 0
sys.exit(1 if tot[3] > allowed or not tot[0] else 0)
P
r=$?
[ $r = 0 ] && echo "PASS rendertest: native == gpgx on every compared frame" || echo "FAIL rendertest (dumps: $out/dumps)"
exit $r
