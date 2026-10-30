#!/bin/sh
# Interrupt-delivery regression test (GitLab #30). difftest can't see this: it holds IRQs.
# Every gameplay frame must run vblank_main unless the game itself masks interrupts
# (room loads run with SR=$2700). With C replacements a VBlank used to be lost when 68k code
# run nested inside C wrote the VDP while far ahead of the raster (hle.c, nest_begin()).
#   port/tools/irqtest.sh          (on a VM, after building build/port)
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
fail=0
for r in pointer_play.txt inventory_play.txt mouse/look.txt mouse/bar.txt mouse/walk.txt; do
	for h in all none; do
		line=$(build/port/frameprof --hle "$h" --script port/scripts/lobby.txt --script "port/scripts/$r" 2>/dev/null | grep 'lost in')
		n=$(echo "$line" | sed -n 's/.*lost in \([0-9]*\) of \([0-9]*\).*/\1/p')
		tot=$(echo "$line" | sed -n 's/.*lost in \([0-9]*\) of \([0-9]*\).*/\2/p')
		if [ -n "$n" ] && [ "$n" = 0 ] && [ "${tot:-0}" -gt 100 ]; then
			echo "PASS $r --hle $h: vblank_main ran in all $tot gameplay frames"
		else
			echo "FAIL $r --hle $h: ${line:-frameprof failed}"
			fail=1
		fi
	done
done
exit $fail
