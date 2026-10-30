#!/bin/sh
# Extra difftest coverage for the walking subsystem (walk.c): random walking (with some
# A/B/C taps) in several rooms reached by the password warp / natural route scripts.
# States go to runs/walkstates/<route>/<ADDR>/, then every walking function is run on them.
#   port/tools/walk_dt.sh [N_PER_ROUTE]          (run on a VM)
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
n=${1:-40}
[ -f build/port/CMakeCache.txt ] || cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/port -j"$(nproc)" --target difftest >/dev/null
py=.venv/bin/python
routes="port/scripts/lobby.txt tools/scripts/rooms/w0_r02.txt tools/scripts/rooms/w0_r04.txt
tools/scripts/rooms/w0_r06.txt tools/scripts/rooms/w0_r09.txt tools/scripts/rooms/w0_r15.txt
tools/scripts/rooms/w0_r20.txt tools/scripts/rooms/w1_r11.txt tools/scripts/rooms/w1_r19.txt
tools/scripts/natural/natural_w0_r17.txt tools/scripts/natural/natural_w1_r27.txt"
funcs="005E4A 006AE8 006B28 006C28 006D24 0073D6 007476 00750A"
mkdir -p runs/walkstates
for r in $routes; do
	name=$(basename "$r" .txt)
	s=runs/walkstates/$name.txt
	$py -I tools/py/walkexplore.py "$r" 7 2500 0.03 > "$s"
	for f in $funcs; do
		d=runs/walkstates/$name/$f
		[ -d "$d" ] && [ "$(ls "$d" | wc -l)" -ge 1 ] && continue
		build/port/difftest capture $f "$n" --out "$d" --script "$s" >/dev/null 2>&1 || true
	done &
done
wait
rc=0
for f in $funcs; do
	for r in $routes; do
		d=runs/walkstates/$(basename "$r" .txt)/$f
		[ -d "$d" ] || continue
		build/port/difftest run "\$$f" --states "$d" | sed "s|^|$(basename "$r" .txt): |" || rc=1
	done
done
exit $rc
