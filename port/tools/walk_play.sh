#!/bin/sh
# Play-test for the walking port: the same random walk (Lobby + Hallway/Cafe via exits,
# with A/B/C taps) with all C replacements and with none; prints Shaggy/Scooby/room at the end
# and the screenshots.  Run on a VM after building build/port.
#   port/tools/walk_play.sh [SEED] [SEGMENTS]
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
seed=${1:-11}; n=${2:-2500}
mkdir -p runs/walkplay
.venv/bin/python -I tools/py/walkexplore.py port/scripts/lobby.txt "$seed" "$n" 0.02 |
	grep -v '^peek' > runs/walkplay/route.txt
echo 'ram end' >> runs/walkplay/route.txt
for h in all none; do
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy build/port/scoobyport --hle $h \
		--script runs/walkplay/route.txt --outdir runs/walkplay/$h --frames 1 >/dev/null 2>&1
	.venv/bin/python -I -c "
import struct,sys
r=open('runs/walkplay/$h/end.ram','rb').read()
w=lambda a: struct.unpack('>H', r[a-0xFF0000:a-0xFF0000+2])[0]
print('$h: shaggy', w(0xFF04DC), w(0xFF04F4), 'scooby', w(0xFF04E0), w(0xFF04F8), 'room', w(0xFF06AC), 'camera', w(0xFF06CA), w(0xFF06CE))"
done
