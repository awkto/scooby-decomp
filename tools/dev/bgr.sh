#!/bin/bash
# bgr.sh SNIPPET LOG [DIR]: run a snippet detached against the EXISTING session runs/DIR (default dev), log to runs/LOG
cd "$(dirname "$0")/../.."
d=${3:-dev}
SDIR=$d setsid nohup python3 tools/dev/run.py "$1" > "runs/$2" 2>&1 < /dev/null &
sleep 1; echo started
