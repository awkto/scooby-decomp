#!/bin/bash
# ver.sh ROUTE... : replay flat routes in the batch harness (verify.py), log runs/ver_ROUTE.log (detached friendly)
cd "$(dirname "$0")/../.."
for r in "$@"; do python3 tools/py/verify.py tools/scripts/story/$r.txt runs/ver_$r > runs/ver_$r.log 2>&1; cat runs/ver_$r.log; done
