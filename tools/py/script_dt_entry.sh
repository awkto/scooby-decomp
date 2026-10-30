#!/bin/sh
# script_dt_entry.sh SRC FUNC:KIND:CODE... (VM, worktree root, build/port built; states of
# SRC captured, e.g. by script_dt_multi.sh): "synthetic dispatch" difftest (#19) of script
# handlers that no route reaches. States captured at handler SRC (another op or sub-op
# handler, entered by the same dispatcher `jsr (a0)`) are replayed with the PC at FUNC
# (difftest --entry) and the registers the dispatcher would have set for a real op of
# FUNC's kind (tools/py/script_pokes.py --dispatch OP / --sub SUB: A5 = that op, A0 = the
# handler, D0 for sub-ops), up to 40 ops x 25 states per state directory.
#   KIND = op | sub, CODE = hex op / sub-op number.
# Example: script_dt_entry.sh 00448C script_sys_09e8_1_on:sub:0D script_op_path:op:21
# EXTRAS="POKE;POKE...": also run every variant with each of these RAM/register pokes added
# (to reach both sides of a test the source states never take).
src=$1; shift
for spec in "$@"; do
	f=${spec%%:*}; rest=${spec#*:}; kind=${rest%%:*}; code=${rest#*:}
	if [ "$kind" = sub ]; then mode=--sub; else mode=--dispatch; fi
	pk=$(.venv/bin/python -I tools/py/script_pokes.py $mode "$code" 40 | tr '\n' ' ')
	# EXTRAS="B:FF09E0=0xFB;B:FF09E0=0xF7": every variant once more with each extra poke
	if [ -n "$EXTRAS" ]; then
		base=$pk; pk=$base
		for x in $(echo "$EXTRAS" | tr ';' ' '); do
			pk="$pk $(echo "$base" | sed "s/--poke \([^ ]*\)/--poke \1,$x/g")"
		done
	fi
	[ -n "$pk" ] || { echo "TOTAL $f: no $kind $code in the script data"; continue; }
	ok=0; all=0
	for d in runs/states/${src}_*; do
		[ -n "$(ls "$d" 2>/dev/null)" ] || continue
		out=$(build/port/difftest run "$f" --entry --states "$d" --max 25 $pk 2>&1)
		line=$(echo "$out" | grep -E 'states equal' | head -1)
		echo "$out" | grep -E 'FAIL|differ|entry:|: 68k=|did not|error' | head -10 | sed 's/^/      /'
		p=$(echo "$line" | sed -n 's/.*: \([0-9]*\)\/\([0-9]*\) states equal.*/\1 \2/p')
		[ -n "$p" ] && { ok=$((ok + ${p% *})); all=$((all + ${p#* })); }
	done
	echo "TOTAL $f (synthetic from \$$src): $ok/$all"
done
