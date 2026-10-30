#!/bin/bash
# gates2.sh : storytest short + long with --hle all on the current build (after a driver change); log runs/gates2.log
cd "$(dirname "$0")/../.."
exec > runs/gates2.log 2>&1
echo "=== storytest short (all)"; STORYTEST_HLE=all nice port/tools/storytest.sh 2>&1 | tail -12
echo "=== storytest long (all)"; STORYTEST_HLE=all STORYTEST_TIMEOUT=3000 nice port/tools/storytest.sh long 2>&1 | tail -6
echo "=== done"
