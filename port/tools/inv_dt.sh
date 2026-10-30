#!/bin/sh
# Difftest the inventory code (verbs.c, the inventory ranges of mainloop.c; GitLab #17) on a route
# that really has an inventory: the story route w0_cafe_items (Lobby -> Cafe, three items picked up
# through the game's own UI: inventory_refresh / arrows_update run for every TAKE), then
# inv_items.txt (pokes 6 more carried objects: 9 items = 3 rows) and inv_play2.txt (slides to the
# inventory, pages down/up with the arrow slots, hovers every slot, Use Can Opener on Chili).
#   port/tools/inv_dt.sh [NAME ADDR]...   capture and compare (default: every function below)
#   DT_RERUN=1 port/tools/inv_dt.sh       re-run on the states already in runs/states_inv
# States go to runs/states_inv/<ADDR>/.  Run on a VM.  DT_TAIL=n shows n lines per function.
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
[ -f build/port/CMakeCache.txt ] || cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/port -j"$(nproc)" --target difftest >/dev/null || exit 1
D=build/port/difftest
SCRIPTS="--script tools/scripts/story/w0_cafe_items.txt --script port/scripts/inv_items.txt --script port/scripts/inv_play2.txt"
PAIRS="inventory_build_page 005D1C inventory_upload 005CB6 inventory_refresh_build 005D02
 inventory_arrow_up_set 005C64 inventory_arrow_down_set 005C7E inventory_arrows_update 005B92
 inventory_arrows_down 005BA2 main_loop_pass 000D3E main_loop_after_inv_back 001178
 main_loop_after_inv_fwd 0011B4 main_loop_after_click 00122E"
[ $# -gt 0 ] && PAIRS="$*"
N=${DT_INV_N:-400}
mkdir -p runs/dt-inv
echo $PAIRS | xargs -n 2 | xargs -P "${DT_JOBS:-4}" -L 1 sh -c '
	name=$0; addr=$1
	d=runs/states_inv/$addr
	if [ -z "$DT_RERUN" ] && [ ! -d "$d" ]; then
		'"$D"' capture $addr '"$N"' --out $d '"$SCRIPTS"' > runs/dt-inv/$name.cap.log 2>&1
	fi
	'"$D"' run $name --states $d --keep-going $DT_EXTRA > runs/dt-inv/$name.log 2>&1'
# main_loop_pass again, on the passes that take the arrow exits ($00116C $001174 $0011A8 $0011B0):
# rare among all passes, so port/scripts/inv_arrows.txt switches the capture on while it clicks them
a=runs/states_inv/000D3E_arrows
if [ -z "$DT_RERUN" ] && [ ! -d $a ]; then
	mkdir -p $a runs/inv_arrows
	cat tools/scripts/story/w0_cafe_items.txt port/scripts/inv_items.txt port/scripts/inv_arrows.txt > runs/inv_arrows_route.txt
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy build/port/scoobyport --hle none --script runs/inv_arrows_route.txt \
		--outdir runs/inv_arrows --frames 0 > runs/dt-inv/arrows.cap.log 2>&1
fi
$D run main_loop_pass --states $a --keep-going -v > runs/dt-inv/main_loop_pass_arrows.log 2>&1
echo "main_loop_pass, arrow passes: $(tail -1 runs/dt-inv/main_loop_pass_arrows.log | cut -c1-60)"
grep -o 'exit \$[0-9A-F]*' runs/dt-inv/main_loop_pass_arrows.log | sort | uniq -c | tr '\n' ' '; echo
tail -1 runs/dt-inv/main_loop_pass_arrows.log | grep -q "states equal" || rc=1
rc=${rc:-0}
set -- $PAIRS
while [ $# -ge 2 ]; do
	tail -${DT_TAIL:-1} runs/dt-inv/$1.log
	grep -q "states equal" runs/dt-inv/$1.log || rc=1
	shift 2
done
exit $rc
