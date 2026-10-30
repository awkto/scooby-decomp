#!/bin/sh
# Run menu tours as one session and make a montage of their screenshots (a quick visual check):
#   port/scripts/menus/tour.sh [-h HLE-spec] N...      e.g.  tour.sh 1 2 3     output: runs/tour/all.png
# The tours continue one another (see gen.py), so give them in order.
h=none
[ "$1" = -h ] && { h=$2; shift 2; }
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
rm -rf runs/tour; mkdir -p runs/tour
for n in "$@"; do cat port/scripts/menus/tour$n.txt; done > runs/tour/chain.txt
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 1200 build/port/scoobyport --hle "$h" \
	--script runs/tour/chain.txt --outdir runs/tour --frames 0 >runs/tour/log 2>&1
tail -2 runs/tour/log
cd runs/tour || exit 1
for f in *.ppm; do convert "$f" -resize 50% "${f%.ppm}.png"; done
files=""; for n in "$@"; do files="$files $(grep '^shot' ../../port/scripts/menus/tour$n.txt | sed 's/shot \(.*\)/\1.png/' | tr '\n' ' ')"; done
montage $files -tile 4x -geometry +2+2 all.png
