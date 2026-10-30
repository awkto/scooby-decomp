#!/bin/bash
# wait.sh LOG [secs] : wait (default 100 s) for END/FAILED in runs/LOG, print the summary lines
cd "$(dirname "$0")/../.."
t=${2:-100}
timeout $t bash -c "until grep -q -E '^END|FAILED' runs/$1; do sleep 3; done"
grep -E "^==|Error|FAILED|Traceback|END|^!!" runs/$1 | cut -c1-300
