#!/bin/sh
# script_dt_pass2.sh [OUT] (VM, worktree root): the difftest runs of the second #19 pass
# (docs/subsystems/script.md "Difftest results, second pass"). Builds, then:
#   1. every call on all routes (script_dt_multi.sh) for the functions with route states;
#   2. A5-retarget pokes (script_pokes.py) for the op handlers;
#   3. synthetic dispatch (script_dt_entry.sh, difftest --entry) for handlers no route
#      reaches: sub-op entries from sub-op $0E states, op handlers from op $08 states,
#      "after bsr actor_animate" pieces of sub-ops from camera_pan_right's.
# Logs to OUT (default runs/pass2); prints the TOTAL lines.
o=${1:-runs/pass2}
mkdir -p "$o"
cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build/port -j4 2>&1 | grep -E 'error|warning:'
M=tools/py/script_dt_multi.sh
E=tools/py/script_dt_entry.sh
pokes() { .venv/bin/python -I tools/py/script_pokes.py "$1" 40 | tr '\n' ' '; }

# 1. route states
$M script_op_walk_spawn script_op_walk_spawn_anim0 script_op_walk_spawn_anim1 \
   script_op_walk_spawn_done0 script_op_walk_spawn_done1 script_op_walk_spawn_done \
   actor_walk_to actor_walk_to_anim actor_walk_to_moving sprite_frame_hotspot \
   script_op_walk_obj script_op_walk_obj_shaggy script_op_walk_obj_done \
   walk_line_clear shaggy_line_clear scooby_line_clear waypoint0_line_clear waypoint1_line_clear \
   shaggy_set_dir scooby_set_dir shaggy_dir_to scooby_dir_to shaggy_leg0_from_wp0 \
   scooby_leg0_from_wp0 scooby_leg0_from_wp1 scooby_leg1_from_wp0 scooby_leg1_from_wp1 \
   shaggy_waypoints_reachable scooby_waypoints_reachable shaggy_walk_to shaggy_walk_turned \
   shaggy_walk_moving shaggy_walk_standing scooby_walk_to scooby_walk_direct scooby_walk_turned \
   scooby_walk_wait_mode scooby_walk_wait_moving scooby_walk_wait_standing \
   script_op_face script_op_face_wait_shaggy script_op_wait_stop script_op_wait_stop_test \
   script_op_test_moving script_sys_skippable_off script_op_show_obj script_op_show_obj_done \
   script_op_set_icon script_op_set_icon_done script_run_nested script_run_nested_done \
   talk_menu_collect talk_menu_collect_done talk_bar_blank_fn talk_gesture_fn talk_choice_say \
   talk_choice_reply talk_choice_body talk_choice_after talk_leave \
   script_op_say_push_d7 script_op_say_push_facing script_op_say_pose_started \
   script_op_say_pose_done script_op_say_push_font script_op_say_pop_font \
   script_op_say_pose_back script_op_say_back_started script_op_say_back_done script_op_say_done \
   script_sys_fade_out script_sys_fade_out_done script_sys_camera_pan_right \
   script_sys_camera_pan_right_wait script_sys_sfx_random_a script_sys_sfx_random_b \
   script_sys_09e8_1_off script_op_set > "$o/routes.log" 2>&1
grep TOTAL "$o/routes.log"

# 2. A5-retarget pokes on the route states
for x in "script_op_walk_spawn 0E" "script_op_walk_spawn 1B" "script_op_walk_obj 0F" \
         "script_op_face 1E" "script_op_wait_stop 1F" "script_op_show_obj 07" \
         "script_op_set_icon 0A" "script_op_test_moving 1C"; do
	set -- $x
	DTOPTS="--max 25 $(pokes $2)" $M "$1" | grep TOTAL | sed "s/\$/ (pokes: op $2)/"
done > "$o/pokes.log" 2>&1
cat "$o/pokes.log"

# 3. synthetic dispatch
{
$E 00448C script_sys_09eb_6_on:sub:3C script_sys_09eb_6_off:sub:3D \
   script_sys_scooby_pose_off:sub:36 script_sys_09eb_0_on:sub:33 script_sys_09eb_0_off:sub:34 \
   script_sys_no_idle_anim_on:sub:30 script_sys_no_idle_anim_off:sub:31 \
   script_sys_09e9_6_on:sub:2D script_sys_09e9_6_off:sub:2E script_sys_09e9_3_on:sub:29 \
   script_sys_09e9_3_off:sub:2A script_sys_talk_end:sub:28 script_sys_09e9_1_on:sub:23 \
   script_sys_09e9_1_off:sub:24 script_sys_09e9_0_on:sub:1F script_sys_09e9_0_off:sub:20 \
   script_sys_overlay_off:sub:19 script_sys_follow_mode_off:sub:16 script_sys_09e8_2_on:sub:11 \
   script_sys_09e8_2_off:sub:12 script_sys_scooby_away:sub:09 script_sys_scooby_back:sub:0A \
   script_sys_scooby_pose_on:sub:0B script_sys_then_talk:sub:2F \
   script_sys_shaggy_alt_sprite_off:sub:2C script_sys_test_shaggy_path:sub:26 \
   script_sys_09e8_1_on:sub:0D script_sys_overlay_a_on:sub:17 script_sys_overlay_b_on:sub:18 \
   script_sys_camera_pan_left:sub:10 script_sys_shaggy_anim_by_x:sub:22 \
   script_sys_shaggy_alt_sprite_on:sub:2B script_sys_shaggy_pose_45:sub:35 \
   script_sys_shaggy_pose_47:sub:37 script_sys_closeup_picture:sub:07
EXTRAS="B:FF2A00=2" $E 00448C script_sys_bar_show_if_hidden:sub:06
$E 002C56 script_op_path:op:21 script_op_stop_path:op:20
$E 0026DA script_op_face_wait_scooby:op:1E
EXTRAS="B:FF0AC0=0" $E 00441C script_sys_wait_shaggy_path:sub:25
EXTRAS="B:FF0ABD=1" $E 00441C script_sys_shaggy_anim_by_x_wait:sub:22 \
   script_sys_shaggy_alt_sprite_on_wait:sub:2B
EXTRAS="B:FF09E0=0xFB;B:FF09E0=0xF7;B:FF09E0=0xFD" $E 00441C script_sys_wait_dpad_choice:sub:21
EXTRAS="B:FF0ABC=1" $E 00441C script_sys_follow_mode_on:sub:15
EXTRAS="B:FF09E9=0" $E 00441C script_sys_camera_pan_left_wait:sub:10
$E 00441C script_sys_shaggy_pose_45_go:sub:35 script_sys_shaggy_pose_45_place:sub:35 \
   script_sys_shaggy_pose_47_go:sub:37 script_sys_shaggy_pose_47_place:sub:37 \
   script_sys_shaggy_pose_45_done:sub:35 script_sys_shaggy_pose_47_done:sub:37
EXTRAS="B:FF0ABC=1;B:FF0ABC=1,B:FF09DE=0x80;B:FF0ABC=1,B:FF0AC0=0" $E 00441C \
   script_sys_shaggy_pose_45_wait:sub:35 script_sys_shaggy_pose_47_wait:sub:37
} > "$o/entry.log" 2>&1
grep -E 'TOTAL|FAIL' "$o/entry.log"
