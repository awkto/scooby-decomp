#!/bin/sh
# script_dt_all.sh OUTDIR FUNC... (VM): build once, then run tools/py/script_dt.sh on the
# functions in 4 parallel jobs (round robin); logs OUTDIR/j<k>.log, merged into OUTDIR/all.log.
o=$1; shift
mkdir -p "$o"
cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build/port -j4 2>&1 | grep -E 'error|warning:'
: "${ROUTES:=tools/scripts/natural/natural_w1_r30.txt tools/scripts/natural/natural_w0_r13.txt port/scripts/script_talk.txt port/scripts/verbs.txt}"
export ROUTES
i=0; l0=; l1=; l2=; l3=
for f in "$@"; do
	case $((i % 4)) in 0) l0="$l0 $f";; 1) l1="$l1 $f";; 2) l2="$l2 $f";; 3) l3="$l3 $f";; esac
	i=$((i + 1))
done
tools/py/script_dt.sh $l0 > "$o/j0.log" 2>&1 &
tools/py/script_dt.sh $l1 > "$o/j1.log" 2>&1 &
tools/py/script_dt.sh $l2 > "$o/j2.log" 2>&1 &
tools/py/script_dt.sh $l3 > "$o/j3.log" 2>&1 &
wait
cat "$o"/j?.log > "$o/all.log"
cat "$o/all.log"
