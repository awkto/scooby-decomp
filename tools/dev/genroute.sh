#!/bin/bash
# genroute.sh ROUTE [DIR]: fresh harness session runs/DIR, run story_routes.py ROUTE in strict mode (writes tools/scripts/story/ROUTE.txt on the VM)
cd "$(dirname "$0")/../.."
d=${2:-dev}
tools/dev/start.sh $d >/dev/null
STORY_ROUTE_DIR=runs/routes setsid nohup python3 tools/py/story_routes.py runs/$d "$1" --cp "runs/$1.cp.json" > "runs/$1.log" 2>&1 < /dev/null &
sleep 1; echo started
