#!/bin/sh
# script_dt_pokes.sh (VM, worktree root, build/port built, states captured by script_dt.sh):
# re-run the A5-driven handlers on their captured states with A5 moved to up to 40 other ops
# of the same kind (tools/py/script_pokes.py), covering condition/set modes, verb matches,
# talk-choice collection etc. that the routes rarely reach. --max 25 states per directory.
run() {   # FUNC OP
	f=$1; op=$2
	addr=$(build/port/difftest list | awk -v f="$f" '$2 == f {print $1}' | head -1 | tr -d '$')
	pk=$(.venv/bin/python -I tools/py/script_pokes.py "$op" 40 | tr '\n' ' ')
	for d in runs/states/$addr runs/states/${addr}_*; do
		[ -d "$d" ] || continue
		echo "$f [$d] $(build/port/difftest run "$f" --states "$d" --max 25 $pk 2>&1 | grep -E 'states equal|FAIL|differ|entry:|: 68k=|did not' | head -6)"
	done
}
run script_op_case_verb 01
run script_op_talk_choice 02
run script_op_if 03
run script_op_if_else 04
run script_op_set 08
run script_op_on_event 13
run script_op_set_state 0B
run script_op_move_obj 05
run script_op_anim_wait 06
run script_op_set_verb 09
run script_op_freeze 16
run script_op_unfreeze_wait 19
run script_op_wait_anim 17
run script_op_pal_cycle 1A
run script_op_sound 0D
run script_op_goto_room_cut 18
run script_op_goto_room 11
run script_op_wait 15
