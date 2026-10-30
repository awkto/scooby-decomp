#!/bin/bash
# bg.sh SNIPPET LOG [DIR]: fresh session runs/DIR (default dev), run a snippet detached against it, log to runs/LOG
cd "$(dirname "$0")/../.."
d=${3:-dev}
tools/dev/start.sh $d >/dev/null
SDIR=$d setsid nohup python3 tools/dev/run.py "$1" > "runs/$2" 2>&1 < /dev/null &
sleep 1; echo started
