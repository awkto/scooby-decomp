#!/bin/sh
# script_dt_multi.sh FUNC... (VM, worktree root, build/port built): difftest many functions
# on every call (up to $N, default 200) on each route of $ROUTES, capturing all of them in
# ONE pass per route (difftest capture-multi) into runs/states/<ADDR>_<route>/ (kept if it
# exists; RECAPTURE=1 redoes it). Routes run 4 in parallel. Prints, per function, one result
# line per route that has states, and a TOTAL line (states equal / tested).
: "${ROUTES:=port/scripts/capture.txt port/scripts/verbs.txt port/scripts/script_talk.txt port/scripts/script/intro_w0.txt port/scripts/script/intro_w1.txt $(ls tools/scripts/natural/*.txt tools/scripts/story/*.txt)}"
: "${N:=200}"
addrs=""
for f in "$@"; do
	a=$(build/port/difftest list | awk -v f="$f" '$2 == f {print $1}' | head -1 | tr -d '$')
	[ -n "$a" ] || { echo "unknown function $f" >&2; continue; }
	addrs="$addrs $a"
done
for r in $ROUTES; do echo "$r"; done | xargs -P 4 -I{} sh -c '
	r={}; t=$(basename "$r" .txt); need=""
	for a in '"$addrs"'; do
		[ -d "runs/states/${a}_$t" ] && [ -z "$RECAPTURE" ] || need="$need $a"
	done
	[ -z "$need" ] || build/port/difftest capture-multi '"$N"' "$t" $need --script "$r" > /dev/null 2>&1'
for f in "$@"; do
	a=$(build/port/difftest list | awk -v f="$f" '$2 == f {print $1}' | head -1 | tr -d '$')
	ok=0; all=0
	for r in $ROUTES; do
		t=$(basename "$r" .txt); d=runs/states/${a}_$t
		[ -n "$(ls "$d" 2>/dev/null)" ] || continue
		out=$(build/port/difftest run "$f" --states "$d" ${DTOPTS:-} 2>&1)
		line=$(echo "$out" | grep -E 'states equal' | head -1)
		echo "  $f [$t] $line"
		echo "$out" | grep -E 'FAIL|differ|entry:|: 68k=|did not|error|  [A-Z].*68k=' | head -12 | sed 's/^/      /'
		p=$(echo "$line" | sed -n 's/.*: \([0-9]*\)\/\([0-9]*\) states equal.*/\1 \2/p')
		[ -n "$p" ] && { ok=$((ok + ${p% *})); all=$((all + ${p#* })); }
	done
	echo "TOTAL $f \$$a: $ok/$all"
done
