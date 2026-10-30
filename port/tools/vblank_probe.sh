#!/bin/sh
# Capture entry states of a function the capture routes never call, by pointing the VBlank hook
# ($FF0000) at it: the interrupt trampoline then calls it once per frame with whatever the game
# had in the registers. Use difftest --poke to give it meaningful inputs.
#   port/tools/vblank_probe.sh ADDR [N]       -> runs/states/<ADDR>/ (N states, default 30)
# Used by port/tools/dt-vdp for the VDP helpers that only the unreachable/rare screens call.
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
a=$1
n=${2:-30}
mkdir -p runs
s=runs/probe_$a.txt
{
	echo "run 1500"
	for i in 1 2 3; do echo "press S 4"; echo "run 150"; done
	echo "run 20"
	echo "poke FF0000 4 $a"
	echo "run $((n + 10))"
} > "$s"
cmake --build build/port -j4 --target difftest >/dev/null
build/port/difftest capture "$a" "$n" --script "$s"
