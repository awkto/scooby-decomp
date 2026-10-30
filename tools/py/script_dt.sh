#!/bin/sh
# script_dt.sh FUNC... (VM, worktree root): difftest script handlers (#19) on the default capture
# route and, with ROUTES="a.txt b.txt" N=..., on extra routes captured into
# runs/states/<ADDR>_<route>/ (handlers that the capture route calls rarely).
set -e
cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/port -j4 --target difftest >/dev/null
for f in "$@"; do
	build/port/difftest check "$f" 2>&1 | grep -E 'states equal|FAIL|differ|entry:|: 68k=|did not|error' | head -8 || true
	addr=$(build/port/difftest list | awk -v f="$f" '$0 ~ " "f"( |$)" {print $1}' | head -1 | tr -d '$')
	for r in $ROUTES; do
		d=runs/states/${addr}_$(basename "$r" .txt)
		[ -d "$d" ] || build/port/difftest capture "0x$addr" "${N:-200}" --script "$r" --out "$d" > /dev/null 2>&1 || true
		echo "  [$r]"
		build/port/difftest run "$f" --states "$d" 2>&1 | grep -E 'states equal|FAIL|differ|entry:|: 68k=|did not|no states' | head -8 || true
	done
done
