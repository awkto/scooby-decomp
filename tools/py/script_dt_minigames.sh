#!/bin/sh
# script_dt_minigames.sh (VM, worktree root, build/port built): difftest of the third #19
# pass (docs/subsystems/script.md, docs/subsystems/minigames.md): ending credits, fullscreen
# picture, Crane Game claw, Bumper Car duel, plus the walk_obj Scooby tail and Shaggy's leg
# setups. Captures every call (up to 1100) on the two long routes in one pass each
# (script_dt_multi.sh; states are kept in runs/states/<ADDR>_<route>, RECAPTURE=1 redoes them).
# The duel's setup and per-frame range have dead registers at their exit: --ignore-regs.
export ROUTES="tools/scripts/story/w0_full.txt tools/scripts/story/w1_part.txt"
export N=${N:-1100}
M=tools/py/script_dt_multi.sh
$M "$@" 
