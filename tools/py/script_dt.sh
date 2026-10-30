#!/bin/sh
# script_dt.sh FUNC... (VM, worktree root; build/port must be built): difftest script
# handlers (#19). First the default capture route (port/tools/dt semantics: >= 200 states
# spread over port/scripts/capture.txt); if that gives fewer than 200 states, also every
# call (--every 1, up to N=200) on each route in $ROUTES, captured into
# runs/states/<ADDR>_<route>/. Prints the result lines per (function, route).
for f in "$@"; do
	r=$(build/port/difftest check "$f" 2>&1)
	echo "$r" | grep -E 'states equal|FAIL|differ|entry:|: 68k=|did not|error' | head -8
	echo "$r" | grep -q ' 200/200 states equal' && continue
	addr=$(build/port/difftest list | awk -v f="$f" '$2 == f {print $1}' | head -1 | tr -d '$')
	for rt in $ROUTES; do
		d=runs/states/${addr}_$(basename "$rt" .txt)
		[ -d "$d" ] || build/port/difftest capture "0x$addr" "${N:-200}" --every 1 --script "$rt" --out "$d" > /dev/null 2>&1
		echo "  [$rt] $(build/port/difftest run "$f" --states "$d" 2>&1 | grep -E 'states equal|FAIL|differ|entry:|: 68k=|did not|no states' | head -6)"
	done
done
