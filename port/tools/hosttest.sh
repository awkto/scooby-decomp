#!/bin/sh
# Headless checks of the SDL host (run on a VM after building build/port):
#   port/tools/hosttest.sh
# 1 wrong-ROM refusal  2 Lobby screenshot == tools/harness screenshot (HLE off) and close with HLE on
# 3 savestate round trip  4 integer-scaled window + overlay  5 real-time pacing with audio (SDL disk driver)
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
out=runs/hosttest
rm -rf "$out"; mkdir -p "$out"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
P=${SCOOBY_PORT:-build/port/scoobyport}   # port/tools/nativewrap.sh: the native runner (#24)
fail=0
ok() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fail=1; }

# 1. wrong ROM
cp rom/baserom.md "$out/bad.md"; printf 'X' | dd of="$out/bad.md" bs=1 seek=4096 conv=notrunc 2>/dev/null
if $P --rom "$out/bad.md" --frames 1 >"$out/bad.log" 2>&1; then bad "wrong ROM accepted"; else
	grep -q "does not match" "$out/bad.log" && ok "wrong ROM refused: $(cat "$out/bad.log")" || bad "wrong ROM: $(cat "$out/bad.log")"; fi

# 2. Lobby vs harness
if [ ! -x build/harness ]; then gcc -O2 -I ext/gpgx/libretro/libretro-common/include -o build/harness tools/harness/harness.c -ldl; fi
mkdir -p "$out/h" "$out/off" "$out/on"
build/harness ext/gpgx/genesis_plus_gx_libretro.so rom/baserom.md port/scripts/lobby.txt "$out/h" >/dev/null 2>&1
$P --hle none --script port/scripts/lobby.txt --outdir "$out/off" --frames 0 >/dev/null 2>&1
$P --hle all --script port/scripts/lobby.txt --outdir "$out/on" --frames 0 >"$out/on.log" 2>&1
r=$(python3 tools/py/cmpshot.py "$out/h/lobby.ppm" "$out/off/lobby.ppm" 2>&1) && ok "Lobby (HLE off) identical to harness: $r" || bad "Lobby HLE off vs harness: $r"
r=$(python3 tools/py/cmpshot.py "$out/h/lobby.ppm" "$out/on/lobby.ppm" --diff "$out/on/diff.png" 2>&1)
echo "INFO Lobby (HLE on) vs harness: $r"; grep '^hle ' "$out/on.log" | sed 's/^/INFO   /'

# 3. savestate round trip
cat port/scripts/lobby.txt > "$out/st.txt"
cat >> "$out/st.txt" <<EOF
save st
press R 40
run 20
shot a
load st
press R 40
run 20
shot b
EOF
$P --hle none --script "$out/st.txt" --outdir "$out" --frames 0 >/dev/null 2>&1
cmp -s "$out/a.ppm" "$out/b.ppm" && ok "savestate round trip (save, play, load, replay -> identical frame)" || bad "savestate round trip"

# 4. window: --scale 3 -> 960x672 window; the 256x224 Lobby is drawn 3x (768x672) letterboxed, with overlay
$P --hle none --script port/scripts/lobby.txt --outdir "$out" --frames 0 --scale 3 --overlay --window-shot "$out/window.ppm" >/dev/null 2>&1
dims=$(head -c 20 "$out/window.ppm" | sed -n 2p)
[ "$dims" = "960 672" ] && ok "window shot $dims" || bad "window shot dims '$dims'"
convert "$out/window.ppm" "$out/window.png" 2>/dev/null

# 5. real-time pacing + audio: 600 frames should take ~10 s and produce ~10 s of 44.1 kHz stereo
SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE="$out/audio.raw" $P --hle all --realtime-script --frames 600 --outdir "$out" >"$out/rt.log" 2>&1 &
pid=$!
wait $pid
echo "INFO realtime run: $(tail -1 "$out/rt.log")"
python3 - "$out/audio.raw" <<'EOF'
import sys, struct
d = open(sys.argv[1], 'rb').read()
n = len(d) // 4
s = struct.unpack('<%dh' % (2 * n), d[:4 * n])
nz = sum(1 for v in s if v)
print(f"INFO audio: {n} stereo frames ({n / 44100:.1f} s at 44.1 kHz), {100 * nz // max(1, len(s))}% non-zero samples, peak {max(map(abs, s)) if s else 0}")
EOF
exit $fail
