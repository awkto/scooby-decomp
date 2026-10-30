#!/bin/bash
# wait2.sh LOG PATTERN [secs]: wait for PATTERN in runs/LOG
cd "$(dirname "$0")/../.."
timeout ${3:-100} bash -c "until grep -q -E '$2' runs/$1; do sleep 3; done"
tail -4 runs/$1 | cut -c1-300
