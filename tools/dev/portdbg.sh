#!/bin/bash
# portdbg.sh ROUTE SPEC LOG : run ROUTE closed-loop on the port with --hle SPEC and STORY_DEBUG=1 (planner trace), detached, log runs/LOG
cd "$(dirname "$0")/../.."
d=runs/pd_$2; rm -rf $d; mkdir -p $d
export STORY_PRESS_MIN=5 STORY_OUT=$d STORY_DEBUG=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
SCOOBY_BACKEND="port:$2" setsid nohup python3 tools/py/session.py serve $d > $d.log 2>&1 < /dev/null &
n=0; while [ ! -S "$d/sock" ] && [ $n -lt 100 ]; do sleep 0.2; n=$((n+1)); done
setsid nohup python3 tools/py/story_routes.py $d $1 > runs/$3 2>&1 < /dev/null &
echo started
