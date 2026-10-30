#!/bin/bash
# st2.sh LOG ROUTE... : detached storytest (HLE spec from $STORYTEST_HLE, default all) on the current build, log runs/LOG
cd "$(dirname "$0")/../.."
l=$1; shift
STORYTEST_HLE=${STORYTEST_HLE:-all} STORYTEST_TIMEOUT=3000 setsid nohup nice port/tools/storytest.sh "$@" > "runs/$l" 2>&1 < /dev/null &
sleep 1; echo started
