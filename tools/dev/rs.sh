#!/bin/bash
# rs.sh SNIP [DIR] : run snippet SNIP against the live session runs/DIR (default dev), foreground, 100 s cap
cd "$(dirname "$0")/../.."
SDIR=${2:-dev} timeout 600 python3 tools/dev/run.py "$1" 2>&1 | cut -c1-600
