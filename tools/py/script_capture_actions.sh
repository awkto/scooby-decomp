#!/bin/sh
# script_capture_actions.sh FUNC ADDR N FRAME... (VM, worktree root, after scripttrace.sh):
# capture N states of ADDR starting one frame before each FRAME of port/scripts/verbs.txt
# (frames where a verb case matched, from the dispatch trace) and difftest FUNC on them.
f=$1; addr=$2; n=$3; shift 3
fr=""; for x in "$@"; do fr="$fr $((x - 1))"; done
d=runs/states/act_${addr}
rm -rf "$d"; mkdir -p "$d"
.venv/bin/python -I tools/py/capture_at.py port/scripts/verbs.txt "$addr" "$n" "$d/s" $fr > "$d/route.txt"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy build/port/scoobyport --hle none --script "$d/route.txt" --frames 1 > "$d/log" 2>&1
tot=0; bad=0
for s in "$d"/s_*; do
	r=$(build/port/difftest run "$f" --states "$s" 2>&1 | grep -E 'states equal|FAIL' | head -1)
	case "$r" in *FAIL*) bad=1; echo "$s: $r"; build/port/difftest run "$f" --states "$s" 2>&1 | head -30;; esac
	echo "$s: $r"
done
