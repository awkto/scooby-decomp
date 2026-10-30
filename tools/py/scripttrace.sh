#!/bin/bash
# scripttrace.sh OUTDIR ROUTE...   (run on a VM from the worktree root; needs build/harness)
# Plays each harness ROUTE with `trace` on and keeps only the script-dispatch reads
# (R lines at pc $0023E2 script_run / $002420 script_run_frame) in OUTDIR/<route>.disp,
# then checks them against the static disassembly: tools/py/scriptdis.py --check.
o=$1; shift
mkdir -p "$o"
for r in "$@"; do
	n=$(basename "$r" .txt)
	d="$o/$n"; rm -rf "$d"; mkdir -p "$d"
	mkfifo "$d/disp.trace"
	grep -E '^R [0-9]+ 00(23e2|2420) ' < "$d/disp.trace" > "$o/$n.disp" &
	{ echo 'trace disp'; cat "$r"; echo 'untrace'; } > "$d/route.txt"
	tools/harness/h.sh "$d/route.txt" "$d" > "$d/harness.log" 2>&1
	wait
	echo "$n: $(wc -l < "$o/$n.disp") dispatches"
done
.venv/bin/python -I tools/py/scriptdis.py --check "$o"/*.disp
