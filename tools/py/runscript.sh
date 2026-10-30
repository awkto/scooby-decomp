#!/bin/bash
# runscript.sh SCRIPT OUTDIR [STATEFILE...]  (run on the VM from the worktree root)
# Fresh OUTDIR, copies given savestates into it (harness `load NAME` reads OUTDIR/NAME.state),
# runs the harness, prints peek.log and converts every .ppm to .png.
s=$1; o=$2; shift 2
rm -rf "$o"; mkdir -p "$o"
for f in "$@"; do cp "$f" "$o/"; done
tools/harness/h.sh "$s" "$o" 2>&1 | tail -3
[ -f "$o/peek.log" ] && cat "$o/peek.log"
for p in "$o"/*.ppm; do [ -f "$p" ] && convert "$p" "${p%.ppm}.png" && rm "$p"; done
exit 0
