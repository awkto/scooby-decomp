#!/bin/bash
# portsnip.sh SPEC SNIPPET : run a snippet on a fresh port session (--hle SPEC), print its output
cd "$(dirname "$0")/../.."
d=runs/ps_$1; rm -rf $d; mkdir -p $d
export STORY_PRESS_MIN=5 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
SCOOBY_BACKEND="port:$1" setsid nohup python3 tools/py/session.py serve $d > $d.log 2>&1 < /dev/null &
n=0; while [ ! -S "$d/sock" ] && [ $n -lt 100 ]; do sleep 0.2; n=$((n+1)); done
SDIR=ps_$1 timeout 300 python3 tools/dev/run.py $2 2>&1 | grep -v "^  plan"
python3 tools/py/session.py stop $d >/dev/null 2>&1
