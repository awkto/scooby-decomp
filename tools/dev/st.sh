#!/bin/bash
# st.sh LOG ARGS... : run port/tools/storytest.sh ARGS detached, log to runs/LOG
cd "$(dirname "$0")/../.."
l=$1; shift
setsid nohup bash -c "nice port/tools/storytest.sh $*" > "runs/$l" 2>&1 < /dev/null &
sleep 1; echo started
