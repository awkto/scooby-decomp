#!/bin/bash
# waitg.sh PATTERN [secs]: wait for PATTERN in runs/gates.log, then show its tail
cd "$(dirname "$0")/../.."
timeout ${2:-100} bash -c "until grep -q -E '$1' runs/gates.log; do sleep 4; done"
tail -9 runs/gates.log
