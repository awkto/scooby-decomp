#!/bin/sh
# Game-save gate (#28): save slots at 8 points of the full walkthroughs, load each, compare with the hand-typed password.
# See slottest.py.   port/tools/slottest.sh [ROUTE...]   (on a VM; needs build/port)
cd "$(dirname "$0")/../.." && exec python3 port/tools/slottest.py "$@"
