#!/bin/sh
# Release tarball smoke test (#28):  port/tools/disttest.sh [TARBALL]     (on a VM, after `make dist`)
# unpacks the tarball into a fresh directory, then checks, headless:
#   1. it runs with the ROM next to the binary (found by its sha1, whatever its file name) for N frames
#   2. a modified ROM (one byte changed) is refused, both next to the binary and with --rom
#   3. no ROM at all: exits with the "ROM not found" message (noninteractive: no picker / drop window)
#   4. --options --json lists options; --options k=v is validated; the ini remembers the ROM after a normal start
#   5. the contents: README, LICENSES/GPGX-LICENSE.txt + SDL2, source/, and romscan over the tarball is clean
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
tb=${1:-$(ls build/dist/scooby-*-linux-x86_64.tar.gz | head -1)}
rom=${ROM:-rom/baserom.md}
work=$(mktemp -d /tmp/disttest.XXXXXX)
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy HOME="$work/home" XDG_CONFIG_HOME= XDG_DATA_HOME=
mkdir -p "$HOME"
fail=0
ok() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fail=1; }

tar -C "$work" -xzf "$tb" || { echo "FAIL cannot unpack $tb"; exit 1; }
d=$(ls -d "$work"/scooby-*/)
for f in scooby README.txt LICENSES/GPGX-LICENSE.txt LICENSES/SDL2-LICENSE.txt LICENSES/README.txt source/port/CMakeLists.txt; do
	[ -e "$d$f" ] && ok "contains $f" || bad "missing $f"
done
[ -e "$d/rom" ] || [ -e "$d/baserom.md" ] && bad "tarball contains a ROM"

cd "$d"
# 1. ROM next to the binary
cp "$root/$rom" ./game.bin
out=$(./scooby --frames 300 --mute --no-throttle 2>&1); rc=$?
echo "$out" | tail -1
[ $rc -eq 0 ] && echo "$out" | grep -q "^300 frames" && ok "runs 300 frames with the ROM next to the binary" || bad "run with ROM beside binary (rc=$rc)"
rm game.bin

# 2. modified ROM
python3 - "$root/$rom" ./game.bin <<'EOF'
import sys
b = bytearray(open(sys.argv[1], "rb").read())
b[0x1234] ^= 1
open(sys.argv[2], "wb").write(b)
EOF
out=$(./scooby --frames 10 --mute 2>&1); rc=$?
[ $rc -ne 0 ] && echo "$out" | grep -q "ROM not found" && ok "modified ROM beside the binary is not accepted" || bad "modified ROM beside binary (rc=$rc): $out"
out=$(./scooby --rom ./game.bin --frames 10 --mute 2>&1); rc=$?
[ $rc -ne 0 ] && echo "$out" | grep -q "refusing to run" && ok "--rom with a modified ROM is refused: $(echo "$out" | head -1 | cut -c1-90)" || bad "--rom modified (rc=$rc): $out"
rm game.bin

# 3. no ROM
out=$(./scooby --frames 10 --mute 2>&1); rc=$?
[ $rc -ne 0 ] && echo "$out" | grep -q "ROM not found" && ok "no ROM: clean error" || bad "no ROM (rc=$rc): $out"

# 4. options and the remembered ROM
./scooby --options --json | python3 -c "import json,sys; o=json.load(sys.stdin); assert len(o) > 10 and not [x for x in o if x['game_changing'] and x['default'] != 'off']" && ok "--options --json parses, no game-changing option defaults on" || bad "--options --json"
./scooby --options scale=99 >/dev/null 2>&1 && bad "--options scale=99 accepted" || ok "--options scale=99 rejected"
cp "$root/$rom" "$work/elsewhere.md"
timeout 8 ./scooby --rom "$work/elsewhere.md" --mute >/dev/null 2>&1
grep -q "^rom = $work/elsewhere.md" "$HOME/.config/scooby-decomp/scooby.ini" && ok "ROM remembered in the ini" || bad "ini not written: $(cat "$HOME/.config/scooby-decomp/scooby.ini" 2>&1 | head -3)"
out=$(./scooby --frames 60 --mute --no-throttle 2>&1); rc=$?
[ $rc -eq 0 ] && echo "$out" | grep -q "^60 frames" && ok "later start without arguments uses the remembered ROM" || bad "remembered ROM start (rc=$rc): $out"
./scooby --options volume=50,filter=crt --frames 5 --mute >/dev/null 2>&1 && ok "--options accepted for a run" || bad "--options run"

# 5. romscan
cd "$root"
if [ -x .venv/bin/python ]; then py=".venv/bin/python -I"; else py="python3 -I"; fi
$py tools/py/romscan.py --rom "$rom" "$tb" | tail -1 | grep -q "CLEAN" && ok "romscan clean over the tarball" || bad "romscan"
rm -rf "$work"
[ $fail -eq 0 ] && echo "disttest: PASS" || echo "disttest: FAIL"
exit $fail
