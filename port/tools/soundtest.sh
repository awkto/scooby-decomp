#!/bin/sh
# Audible-behaviour gate for the sound glue (#22): run the Sound Test route, a spam route and an
# in-game pause route with --hle all and --hle none, log every 68k write into the Z80 mailbox ring
# (SCOOBY_SNDLOG) and compare the message sequences (tools/py/sndcmp.py).  Needs build/port.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
mkdir -p runs/sndtest
cmake --build build/port -j"$(nproc)" --target scoobyport >/dev/null
rc=0
off=-sound_cmd_play,-sound_cmd_stop,-sound_cmd_pause,-sound_cmd_unpause,-sound_cmd_stop_all,-sound_play_wrapper,-sound_stop_wrapper,-z80_load_driver,-z80_reset_driver,-sound_init_wrapper
for r in sound_test sound_spam sound_pause; do
  for h in all none nosound; do
    case $h in nosound) spec=all,$off;; *) spec=$h;; esac
    SCOOBY_SNDLOG=runs/sndtest/$r.$h.log SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
      timeout 600 build/port/scoobyport --hle "$spec" --script port/scripts/$r.txt --outdir runs/sndtest \
      --screenshot runs/sndtest/$r.$h.ppm >runs/sndtest/$r.$h.out 2>&1 || true
  done
  echo "== $r"
  if [ $r = sound_spam ]; then
    # rapid B/D presses: the Sound Test sees some edges twice depending on frame phase, so only structure is checked
    python3 tools/py/sndcmp.py --structural runs/sndtest/$r.all.log runs/sndtest/$r.none.log runs/sndtest/$r.nosound.log || rc=1
    continue
  fi
  python3 tools/py/sndcmp.py runs/sndtest/$r.all.log runs/sndtest/$r.none.log || rc=1
  # same HLE set minus the sound functions: isolates this subsystem from timing desync caused by others
  python3 tools/py/sndcmp.py runs/sndtest/$r.all.log runs/sndtest/$r.nosound.log || rc=1
done
exit $rc
