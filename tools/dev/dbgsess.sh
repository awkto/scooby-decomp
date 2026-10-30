#!/bin/bash
# dbgsess.sh : fresh session runs/dbg seeded with the savestates/marks of runs/dev
cd "$(dirname "$0")/../.."
tools/dev/start.sh dbg > /dev/null
cp runs/dev/c1*.state runs/dev/c1*.route runs/dev/c2*.state runs/dev/c2*.route runs/dbg/ 2>/dev/null
ls runs/dbg | head -3
