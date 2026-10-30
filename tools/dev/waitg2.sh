#!/bin/bash
cd "$(dirname "$0")/../.."
timeout ${2:-100} bash -c "until grep -q -E '$1' runs/gates2.log; do sleep 4; done"
tail -${3:-9} runs/gates2.log
