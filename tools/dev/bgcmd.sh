#!/bin/bash
# bgcmd.sh SCRIPT : run a script detached (own session), return immediately
cd "$(dirname "$0")/../.."
setsid nohup "$@" > /dev/null 2>&1 < /dev/null &
echo started
