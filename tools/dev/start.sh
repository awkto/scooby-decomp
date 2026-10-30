#!/bin/bash
# start.sh [DIR] : start a persistent exploration session (harness backend) in runs/DIR (default dev) on the VM
cd "$(dirname "$0")/../.."
d=${1:-dev}
pkill -f "session.py serve runs/$d\$" ; pkill -f "runs/$d/ctl"; sleep 1
rm -rf runs/$d; mkdir -p runs/$d
setsid nohup python3 tools/py/session.py serve runs/$d > runs/$d.log 2>&1 < /dev/null &
sleep 2; ls runs/$d
