#!/bin/bash
# gates.sh : rebuild the port and run every gate (hosttest, irqtest, mousetest, storytest short routes, storytest long), one log
cd "$(dirname "$0")/../.."
exec > runs/gates.log 2>&1
(cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release >/dev/null && nice cmake --build build/port -j4 2>&1 | tail -2)
for g in hosttest irqtest mousetest; do echo "=== $g"; nice port/tools/$g.sh 2>&1 | tail -6; done
echo "=== storytest short (all)"; STORYTEST_HLE=all nice port/tools/storytest.sh 2>&1 | tail -12
echo "=== storytest long (all)"; STORYTEST_HLE=all STORYTEST_TIMEOUT=3000 nice port/tools/storytest.sh long 2>&1 | tail -6
echo "=== done"
