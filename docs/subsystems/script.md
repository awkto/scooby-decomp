# Script interpreter: functions and port status (GitLab #19)

The byte code, data format and every opcode are described in `docs/script.md`. This page
lists the functions of the subsystem, how they are split for the C port, and their difftest
status. C: `port/src/game/script.c` (dispatch, data ops, drivers, TALK), `script_walk.c`
(walk ops), `script_ops.c` (actor/inventory ops, sub-ops, the rest of `say`), `walkto.c`
(the walk-to routines `$005002-$005B54`, taken over from #16).

## How the port is split

Most execute-mode handlers wait for VBlank-driven state, so the interpreter cannot be one C
function while the 68k CPU is still emulated (docs/porting.md, "Waiting for VBlank"):

* **Dispatch loops** (`script_run`, `script_run_frame`): `HLE_RANGE`s that do the end test
  and the table lookup and stop in front of the `jsr (a0)` to the handler. The 68k makes the
  call; the instruction after it is the next range (stop test, loop).
* **Non-blocking handlers**: `HLE_FUNC`s at the handler address (the dispatcher's `jsr`
  lands in C).
* **Blocking handlers**: `HLE_RANGE`s from the handler entry to the blocking call
  (`jsr sub_00B0B8` = one frame of actor work, `wait_vblank`, `room_load`, ...), plus a range
  after the call that loops back to the call or finishes the op.
* **Pushes across blocking calls** (second pass): a range exit may declare a stack delta
  (`HLE_EXIT_SP(addr, delta)`, docs/porting.md "Ranges that push or pop"). The walk ops push
  D7, `shaggy_walk_to` the VBLANK_FLAGS byte, the TALK code 8/16/52/60 bytes and 10 per
  TALK level, `say` D7/facing/font base, `fade_out` VBLANK_FLAGS: all of it is C now. The
  first pass had to leave every push/pop instruction in 68k.
* Callees of other subsystems go through `hle_call68k`: `sound_play`/`sound_stop` (sound),
  `vdp_dma_cram`/`vdp_dma_vram` (VDP), `verb_label_redraw`/`str_copy` (#17/util),
  `font_tiles_load` $007A2A font recolour (text, now C: room.c).

In a native build without the CPU, the ranges collapse into one loop
`while (pc < end) { handler[op](mode); if (stop) break; }` and each blocking handler becomes a
coroutine that yields where the 68k code calls `sub_00B0B8` / `wait_vblank` / the walk or
room-load functions.

## Functions and status

**C** = replaced (HLE_FUNC or ranges) and difftested; **68k** = not replaced, with the reason.
Results: `n/n` on the default capture route (port/scripts/capture.txt, >= 200 spread states
when the route has that many calls), `+ routes` = every call (up to 200) on
`natural_w1_r30`, `natural_w0_r13`, `script_talk`, `verbs`, `+ pokes` = the same states
replayed with A5 moved to up to 40 other ops of the same kind (`tools/py/script_pokes.py`).
See "Difftest results" for the numbers.

### Dispatch and drivers

| address | name (C) | what | status |
| --- | --- | --- | --- |
| $0023DC / $0023F4 | script_run, script_run_next | scan-mode loop: end test + handler fetch (exit at `jsr (a0)` $23F2), stop test after it | C |
| $002406 / $002432 | script_run_frame, script_run_frame_next | execute-mode loop incl. the Start-skip test | C |
| $002320 / $002348 | script_run_nested, _done | TALK choice collection (flags 4, count 0), A5/A6/callback pushed around `bsr script_run` | C (2nd pass, SP -12 / +12) |
| $0019B0 | action_execute, _verb_pass ($1A16), _run_case ($1A46), _after ($1A84), _after_talk ($1A9C), _done ($1BA0) | verb on object: header, TALK collection, `case` scan, body, failure text "I can't ...", then_talk | C (6 ranges; pushes, script calls and `sub_00A130` text stay 68k) |
| $001C0C | talk_menu_begin ($1C10), _save ($1C4A), _slide ($1C9E), _collect ($1CC4) + _collect_done ($1CD8), _level ($1CDC), _draw ($1D22), _input ($1DB8), _pick ($1E5A) | TALK dialog menu: bar slide-out, collect and list unused choices, Up/Down, pick | C (collect: 2nd pass, movem.l d0-a6 = SP -60 / +60) |
| $001E7A-$0020D0 | talk_choice_say ($1E7A), _reply ($1ED4), _body ($1FC2), _after ($1FE4), talk_leave ($2084); helpers talk_bar_blank $0022F2, talk_gesture $0020DA | the picked choice: Shaggy says it (text_wait), the partner's reply in its colour and pose (text_show), the body (script_run_frame), next / previous TALK level or leave, bar slide-in and restore | C (2nd pass). The tail `bsr wait_vblank / inventory_refresh / inventory_arrows_update / rts` is only blocking calls (68k) |
| $00221A / $002278 | room_enter_script ($2226 + $2266), room_after_fade_script ($2284 + $22C4) | room scripts | C (ranges around the pushes) |
| $00AFDA | run_object_events_setup ($AFEA), _matched ($B05E), _next ($B09A) | the object walk of the event scan; scan/run calls stay 68k | C |

### Opcode handlers (`script_op_table` $002354)

| op | address | name | kind | status |
| --- | --- | --- | --- | --- |
| 00 | $00250C | nop | | 68k: occurs nowhere in the script data |
| 01 | $00246A | case_verb | func | C |
| 02 | $0025AC | talk_choice | func | C |
| 03 | $002732 | if (+ condition $002796) | func | C |
| 04 | $00274A | if_else | range (condition; the nested run stays 68k) | C |
| 05 | $0028A0 | move_obj (+ $29EA, $2A2C) | ranges | C |
| 06 | $002B5A | anim_wait (+ $2B92, $2BE2) | ranges | C |
| 07 | $002BF8 | show_obj (+ $2C50) | ranges around inventory_refresh | C (2nd pass) |
| 08 | $002C56 | set | func (toggle mode with the original's A3 bug -> hle_call68k_orig; unused by the data) | C |
| 09 | $002D5E | set_verb | func | C |
| 0A | $002DC0 | set_icon (+ $2DEC) | ranges around wait_vblank + inventory_refresh | C (2nd pass) |
| 0B | $002DF2 | set_state | range (exit at the redraw spin $2E6E) | C |
| 0C | $002E9E | sys (+ $2EAE) | ranges around `jsr (a0)` | C |
| 0D | $0048B6 | sound | func | C |
| 0E / 1B | $0049BA | walk_spawn / walk_xy (+ $4A34, $4AB2, $4A4C, $4ACA, $4B0E) | ranges, D7 pushed (SP -2 / +2) | C (2nd pass) |
| -- | $004B24 | actor_walk_to (+ $4BA0, $4C14) | ranges | C (2nd pass) |
| 0F | $004C20 | walk_obj (+ $4D1C, $4D78; helper sprite_frame_hotspot $00197E) | ranges, D7 pushed | C (2nd pass); the Scooby tail $4D48 has no states (not registered) |
| 10 | $004D80 | say | ranges (text.c, #20) + script_ops.c | C: text.c's 6 ranges, plus (2nd pass) the pushes/pops $4D84 $4DBE $4E1C $4E5C $4E80 $4EDC and the pose waits $4DDE $4DEE $4EA8 $4EB8 |
| 11 | $004F0E | goto_room (+ $4F34, $4F4A) | ranges around palette_read_cram / room_reload | C |
| 12 / 14 | $004F50 / $002E7E | set_hotspot / set_hotspot2 | func | 14 C; 12 same code, not registered: op $12 occurs nowhere in the data |
| 13 | $002446 | on_event | func | C |
| 15 | $0048D8 | wait (+ $48E8) | ranges | C |
| 16 | $002AD0 | freeze | func | C |
| 17 | $004FBE | wait_anim (+ $4FF2) | ranges | C |
| 18 | $004EE4 | goto_room_cut (+ $4F08) | ranges around room_load | C |
| 19 | $002B0E | unfreeze_wait (+ $2B44) | ranges | C |
| 1A | $002626 | pal_cycle | func | C |
| 1C / 1D | $00255E / $002510 | test_moving / test_anim_done | func | 1C C (2nd pass: Blake intro); 1D not registered: occurs nowhere in the data |
| 1E | $00268E | face (+ $26DA, $271C) | ranges | C (2nd pass); $271C synthetic states only |
| 1F | $004F70 | wait_stop (+ $4FA4) | ranges | C (2nd pass, Blake intro) |
| 20 / 21 | $0048F8 / $004944 | stop_path / path | func | C (2nd pass, synthetic states only) |

### Walk-to routines (from #16, `walkto.c`)

| address | name | what | status |
| --- | --- | --- | --- |
| $005002 | shaggy_walk_to (+ $55F4 turned, $561E moving, $5648 standing) | plan (straight or over the 2 room waypoints), turn, walk, stand; VBLANK_FLAGS byte pushed (SP -2 / +2) | C |
| $0056B0 / $005AD8 | scooby_walk_to / scooby_walk_direct (+ $5B34 turned) | same planner for Scooby; returns after the turn, the walk runs on | C |
| $00566A | scooby_walk_wait (+ $566E, $5678, $56A2) | scooby_walk_to then wait for the arrival and the stand pose; the entry is the `bsr` (68k) | C |
| $005378 / $005364 / $0059B0 / $0052C0 / $0052F4 | walk_line_clear, shaggy_/scooby_line_clear, waypoint0_/waypoint1_line_clear | straight-line collision test (per-step cost model) | C |
| $00548E / $0059DC / $00547A / $0059C8 | shaggy_/scooby_set_dir, _dir_to | walk direction (8.8) and facing | C |
| $00532A / $005976 | shaggy_/scooby_waypoints_reachable | | C |
| $00519C, $005852, $00589A, $0058E4, $00592C | leg setup (waypoint -> target) | | C; Shaggy's $0051E4/$00522E/$005276 have no states (not registered; the planner range runs the same C) |

### Sub-op handlers (`script_subop_table` $002EB4)

| sub | address | name | status |
| --- | --- | --- | --- |
| 00 / 08 | $002FAC / $004534 | nop / closeup_off | written, not registered: occur nowhere in the data |
| 01 | $00453E | end | C |
| 02 | $004548 | room_fade_in | 68k: `bsr room_fade_in / rts`, nothing to port |
| 03 | $00454E (+ $4560) | fade_out | C (2nd pass, VBLANK_FLAGS byte pushed) |
| 04 | $004570 | wait_button (test after the two calls) | C (range; the entry is two `bsr`s) |
| 05 / 0C | $0045A4 | fullscreen_picture / bar_hide_once | 68k: long picture viewer with VRAM save/restore, no states |
| 06 | $00481A | bar_show_if_hidden | C (2nd pass, synthetic) |
| 07 | $0044C2 | closeup_picture (after the 68k spin at $44BA) | C (2nd pass, synthetic) |
| 09 / 0A / 0B / 36 | | scooby_away / back / pose_on / pose_off | C (2nd pass, synthetic) |
| 0D / 0E | $004482 / $00448C | camera lock on / off | C (0D: synthetic) |
| 0F / 10 | $0043C0 (+ $441C) / $004428 (+ $4474) | camera_pan_right / left | C (2nd pass; right: Blake intro + capture, left: synthetic) |
| 11 / 12 | $0043AC / $0043B6 | $FF09E8 bit 2 | C (synthetic) |
| 13 / 14 | $004374 / $004390 | sfx_random_a / b | C (2nd pass, story routes) |
| 15 / 16 | $004330 / $00436A | follow_mode_on (after its `bsr`) / off | C (synthetic) |
| 17 / 18 / 19 | $0042A2 / $0042E0 / $004322 | overlay_a_on / overlay_b_on / overlay_off | C (synthetic) |
| 1A / 1B | $004274 / $004298 | pal_flash on / off | C |
| 1C | $004862 (+ $4874) | bar_show | C (ranges) |
| 1D | $00482E (+ $4844, $4850) | bar_hide | C (ranges; the wait-for-idle spin $4836 stays 68k) |
| 1E | $0041B6 | ending_credits | 68k: the end sequence (LZSS, scroll, fade, soft reset), no states |
| 1F/20, 23/24, 29/2A, 2D/2E, 30/31 | | $FF09E9 bits 0, 1, 3, 6, 7 | C (synthetic) |
| 21 | $00415E | wait_dpad_choice (after its `bsr`) | C (synthetic) |
| 22 | $004120 (+ $414C) | shaggy_anim_by_x | C (synthetic) |
| 25 | $004100 | wait_shaggy_path (after its `bsr`) | C (synthetic) |
| 26 | $0040E0 | test_shaggy_path | C (synthetic) |
| 27 / 32 | $003F58 / $003142 | minigame_throw / minigame_tug | 68k: interactive mini-games (100 / 450 instructions of input loops), no states |
| 28 / 2F | $003F4E / $003E96 | talk_end / then_talk | C (synthetic) |
| 2B / 2C | $003ED2 (+ $3F00) / $003F16 | shaggy_alt_sprite on / off | C (synthetic) |
| 33/34, 3A/3B, 3C/3D | | $FF09EB bits 0, 5, 6 | C (3A/3B route states, others synthetic) |
| 35 / 37 | $00301E / $0030A6 (5 ranges each) | shaggy_pose_45 / 47 | 35 C (synthetic; the sprite-DMA spin stays 68k); 37 written, not registered: occurs nowhere in the data |
| 38 / 39 | $002FD6 / $002FE0 | skippable off / on | C (38: both intros) |

## Difftest results (first pass)

Run with `tools/py/script_dt_all.sh` (on the VM, after merging main). **No failing state.** Per function: the default capture route, then every call (`--every 1`, up to 200) on extra routes where the default gave fewer than 200 (routes without calls omitted). `pokes` = total states replayed with A5 moved to other ops of the same kind (`script_dt_pokes.sh`, up to 25 states x 40 variants per state directory).

| address | function | states (route n/n) | pokes |
| --- | --- | --- | --- |
| $0019B0 | action_execute | capture 5/5; natural_w1_r30 11/11; natural_w0_r13 4/4; script_talk 2/2; verbs 58/58 |  |
| $001A16 | action_execute_verb_pass | capture 3/3; natural_w1_r30 10/10; natural_w0_r13 3/3; script_talk 1/1; verbs 56/56 |  |
| $001A46 | action_execute_run_case | capture 3/3; natural_w1_r30 10/10; natural_w0_r13 3/3; script_talk 1/1; verbs 56/56 |  |
| $001A84 | action_execute_after | capture 3/3; natural_w1_r30 9/9; natural_w0_r13 2/2; script_talk 1/1; verbs 56/56 |  |
| $001A9C | action_execute_after_talk | script_talk 1/1 |  |
| $001BA0 | action_execute_done | capture 3/3; natural_w1_r30 9/9; natural_w0_r13 2/2; script_talk 1/1; verbs 56/56 |  |
| $001C10 | talk_menu_begin | script_talk 1/1 |  |
| $001C4A | talk_menu_save | script_talk 1/1 |  |
| $001C9E | talk_menu_slide | script_talk 200/200 |  |
| $001CDC | talk_menu_level | script_talk 9/9 |  |
| $001D22 | talk_menu_draw | script_talk 6/6 |  |
| $001DB8 | talk_menu_input | script_talk 200/200 |  |
| $001E5A | talk_menu_pick | script_talk 200/200 |  |
| $002226 | room_enter_script | capture 6/6; natural_w1_r30 10/10; natural_w0_r13 5/5; script_talk 2/2; verbs 3/3 |  |
| $002266 | room_enter_script_done | capture 6/6; natural_w1_r30 10/10; natural_w0_r13 5/5; script_talk 2/2; verbs 3/3 |  |
| $002284 | room_after_fade_script | capture 6/6; natural_w1_r30 10/10; natural_w0_r13 5/5; script_talk 2/2; verbs 3/3 |  |
| $0022C4 | room_after_fade_script_done | capture 6/6; natural_w1_r30 10/10; natural_w0_r13 5/5; script_talk 2/2; verbs 3/3 |  |
| $0023DC | script_run | capture 200/200 |  |
| $0023F4 | script_run_next | capture 200/200 |  |
| $002406 | script_run_frame | capture 200/200 |  |
| $002432 | script_run_frame_next | capture 200/200 |  |
| $002446 | script_op_on_event | capture 200/200 | 625 |
| $00246A | script_op_case_verb | capture 200/200 | 2000 |
| $0025AC | script_op_talk_choice | capture 200/200 | 2000 |
| $002626 | script_op_pal_cycle | natural_w1_r30 2/2; natural_w0_r13 1/1 | 78 |
| $002732 | script_op_if | capture 200/200 | 1000 |
| $00274A | script_op_if_else | capture 200/200 | 1000 |
| $0028A0 | script_op_move_obj | capture 13/13; natural_w1_r30 7/7; natural_w0_r13 9/9; script_talk 6/6; verbs 6/6 | 1640 |
| $0029EA | script_op_move_obj_wait | capture 2/2; natural_w0_r13 31/31 |  |
| $002A2C | script_op_move_obj_static | natural_w1_r30 2/2; natural_w1_r27 1/1; w0_cafe_items 6/6; w0_kitchen 3/3 |  |
| $002AD0 | script_op_freeze | capture 8/8; natural_w1_r30 12/12; natural_w0_r13 7/7; script_talk 4/4; verbs 4/4 | 1400 |
| $002B0E | script_op_unfreeze_wait | capture 4/4; natural_w1_r30 8/8; natural_w0_r13 5/5; script_talk 2/2; verbs 2/2 | 840 |
| $002B44 | script_op_unfreeze_wait_loop | capture 4/4; natural_w1_r30 8/8; natural_w0_r13 5/5; script_talk 2/2; verbs 2/2 |  |
| $002B5A | script_op_anim_wait | natural_w0_r17 2/2; natural_w0_r13 8/8; script_talk 5/5; w0_cafe_items 7/7; w0_kitchen 4/4 | 1040 |
| $002B92 | script_op_anim_wait_started | natural_w0_r17 200/200; natural_w0_r13 128/128; w0_cafe_items 200/200; w0_kitchen 200/200 |  |
| $002BE2 | script_op_anim_wait_looped | natural_w0_r13 200/200; script_talk 32/32 |  |
| $002C56 | script_op_set | capture 200/200 | 1000 |
| $002D5E | script_op_set_verb | natural_w0_r17 2/2; w0_cafe_items 1/1 | 120 |
| $002DF2 | script_op_set_state | capture 167/167; natural_w1_r30 15/15; natural_w0_r13 16/16; script_talk 3/3; verbs 3/3 | 2480 |
| $002E7E | script_op_set_hotspot2 | capture 3/3; natural_w0_r13 2/2; script_talk 1/1; verbs 1/1 |  |
| $002E9E | script_op_sys | capture 28/28; natural_w1_r30 32/32; natural_w0_r13 16/16; script_talk 14/14; verbs 12/12 |  |
| $002EAE | script_op_sys_done | capture 28/28; natural_w1_r30 31/31; natural_w0_r13 16/16; script_talk 14/14; verbs 12/12 |  |
| $002FC2 | script_sys_09eb_5_on | capture 2/2; natural_w1_r30 1/1; natural_w0_r13 1/1; script_talk 1/1; verbs 1/1 |  |
| $002FCC | script_sys_09eb_5_off | capture 2/2; natural_w1_r30 1/1; natural_w0_r13 1/1; script_talk 1/1; verbs 1/1 |  |
| $002FE0 | script_sys_skippable_on | capture 2/2; natural_w1_r30 1/1; natural_w0_r13 1/1; script_talk 1/1; verbs 1/1 |  |
| $004274 | script_sys_pal_flash_on | capture 2/2; natural_w1_r30 1/1; natural_w0_r13 1/1; script_talk 1/1; verbs 1/1 |  |
| $004298 | script_sys_pal_flash_off | capture 3/3; natural_w1_r30 1/1; natural_w0_r13 1/1; script_talk 1/1; verbs 1/1 |  |
| $00448C | script_sys_09e8_1_off | capture 4/4; natural_w0_r13 2/2; script_talk 2/2; verbs 2/2 |  |
| $00453E | script_sys_end | script_talk 2/2 |  |
| $004570 | script_sys_wait_button_test | natural_w1_r27 107/107 |  |
| $00482E | script_sys_bar_hide | capture 4/4; natural_w1_r30 6/6; natural_w0_r13 2/2; script_talk 2/2; verbs 2/2 |  |
| $004844 | script_sys_bar_hide_slide | capture 4/4; natural_w1_r30 6/6; natural_w0_r13 2/2; script_talk 2/2; verbs 2/2 |  |
| $004850 | script_sys_bar_hide_loop | capture 200/200 |  |
| $004862 | script_sys_bar_show | capture 4/4; natural_w1_r30 5/5; natural_w0_r13 2/2; script_talk 2/2; verbs 2/2 |  |
| $004874 | script_sys_bar_show_loop | capture 200/200 |  |
| $0048B6 | script_op_sound | capture 10/10; natural_w1_r30 11/11; natural_w0_r13 4/4; script_talk 4/4; verbs 4/4 | 1320 |
| $0048D8 | script_op_wait | capture 3/3; natural_w1_r30 16/16; natural_w0_r13 12/12; script_talk 1/1; verbs 1/1 | 1320 |
| $0048E8 | script_op_wait_loop | capture 200/200 |  |
| $004EE4 | script_op_goto_room_cut | capture 5/5; natural_w1_r30 9/9; natural_w0_r13 4/4; script_talk 2/2; verbs 2/2 | 880 |
| $004F08 | script_op_goto_room_cut_done | capture 5/5; natural_w1_r30 9/9; natural_w0_r13 4/4; script_talk 2/2; verbs 2/2 |  |
| $004F0E | script_op_goto_room | natural_w1_r30 1/1; natural_w0_r13 1/1; verbs 1/1 | 120 |
| $004F34 | script_op_goto_room_load | natural_w1_r30 1/1; natural_w0_r13 1/1; verbs 1/1 |  |
| $004F4A | script_op_goto_room_done | natural_w1_r30 1/1; natural_w0_r13 1/1; verbs 1/1 |  |
| $004FBE | script_op_wait_anim | natural_w0_r13 5/5 | 200 |
| $004FF2 | script_op_wait_anim_loop | natural_w0_r13 200/200 |  |
| $00AFEA | run_object_events_setup | capture 200/200 |  |
| $00B05E | run_object_events_matched | capture 200/200 |  |
| $00B09A | run_object_events_next | capture 200/200 |  |

First pass: `pal_cycle`'s DMA path was changed to a tail jump after its poke run; it was
re-run on the route states only.

## Difftest results, second pass

`tools/py/script_dt_pass2.sh` (VM) reproduces all of it. **No failing state** except one
invalid poke variant (below). Three kinds of states:

* **route**: every call (up to 200 per route) on all routes: `capture`, `verbs`,
  `script_talk`, the new `port/scripts/script/intro_w0.txt` / `intro_w1.txt` (title -> the
  mystery's intro cutscene played to the end, *not* skipped: it runs face, wait_stop,
  test_moving, camera_pan_right, the walk/put-and-animate paths...), the 12 natural routes
  and the 6 story routes (`difftest capture-multi`: one pass per route for all addresses).
* **pokes**: the route states replayed with A5 moved to up to 40 other ops of the same kind
  (`script_pokes.py`, 25 states per directory).
* **synthetic**: handlers that no route reaches, run on states captured at another handler
  entered through the same call (difftest `--entry`, `script_dt_entry.sh`): sub-op entries
  from sub-op $0E states (A5/A0/D0 poked to real ops of the target sub-op), op handlers from
  op $08 states, "after `bsr actor_animate`" pieces of sub-ops from camera_pan_right's
  ($441C), plus RAM pokes (`EXTRAS`) for the other side of their tests. They prove the C
  equals the 68k on plausible inputs; they are not states the game produced.

| function(s) | route | pokes / synthetic |
| --- | --- | --- |
| script_op_walk_spawn ($49BA) | 65/65 | 2600/2600 (op $0E) + 2600/2600 (op $1B) |
| walk_spawn_anim0/anim1 ($4A34/$4AB2), _done0/_done1/_done ($4A4C/$4ACA/$4B0E) | 1/1, 1/1, 9/9, 3/3, 65/65 | |
| actor_walk_to ($4B24), _anim ($4BA0), _moving ($4C14) | 49/49, 34/34, 600/600 | |
| script_op_walk_obj ($4C20), _shaggy ($4D1C), _done ($4D78); sprite_frame_hotspot ($197E) | 17/17 each | 644/645 (op $0F, see below) |
| walk_line_clear / shaggy_ / scooby_ / waypoint0_ / waypoint1_line_clear | 548, 33, 455, 35, 25 (all equal) | |
| shaggy_/scooby_set_dir, shaggy_/scooby_dir_to | 24, 382, 21, 329 (all equal) | |
| leg setup: shaggy_leg0_from_wp0, scooby_leg0_wp0/leg0_wp1/leg1_wp0/leg1_wp1 | 3, 25, 11, 5, 12 (all equal) | |
| shaggy_/scooby_waypoints_reachable | 6/6, 63/63 | |
| shaggy_walk_to ($5002), _turned, _moving, _standing | 26/26, 297/297, 1628/1628, 142/142 | |
| scooby_walk_to ($56B0), scooby_walk_direct ($5AD8), scooby_walk_turned | 412/412, 293/293, 1563/1563 | |
| scooby_walk_wait_mode/_moving/_standing | 3/3, 407/407, 29/29 | |
| script_op_face ($268E), _wait_shaggy ($26DA), _wait_scooby ($271C) | 5/5, 400/400, - | 200/200 (op $1E); _wait_scooby 2000/2000 synthetic (from $26DA) |
| script_op_wait_stop ($4F70), _test ($4FA4) | 4/4, 4/4 | 160/160 (op $1F) |
| script_op_test_moving ($255E) | 200/200 | 100/100 (op $1C) |
| script_op_show_obj ($2BF8), _done | 2/2, 2/2 | 18/18 (op $07) |
| script_op_set_icon ($2DC0), _done | 1/1, 1/1 | 4/4 (op $0A) |
| script_op_path ($4944), script_op_stop_path ($48F8) | - | 8280/8280, 2070/2070 synthetic (from op $08) |
| script_run_nested ($2320), _done | 136/136, 136/136 | |
| talk_menu_collect ($1CC4), _done ($1CD8) | 9/9, 9/9 | |
| talk_choice_say / _reply / _body / _after, talk_leave | 6/6 each, talk_leave 200/200 | |
| talk_bar_blank ($22F2), talk_gesture ($20DA) | 14/14, 105/105 | |
| say: push_d7, push_facing, pose_started, pose_done, push_font, pop_font, pose_back, back_started, back_done, done | 161, 5, 78, 600, 161, 159, 5, 5, 600, 159 (all equal) | |
| sys fade_out ($454E), _done ($4560) | 48/48, 48/48 | |
| sys camera_pan_right ($43C0), _wait ($441C) | 3/3, 400/400 | |
| sys sfx_random_a / _b | 13/13, 1/1 | |
| sys skippable_off ($38), 09e8_1_off ($0E) | 2/2, 29/29 | |
| 26 flag / one-shot sub-ops ($0B $09 $0A $0D $11 $12 $16 $19 $1F $20 $23 $24 $26 $28 $29 $2A $2C $2D $2E $2F $30 $31 $33 $34 $36 $3C $3D) | - | 29 to 232 each, all equal (synthetic) |
| overlay_a_on, overlay_b_on, camera_pan_left, shaggy_anim_by_x, shaggy_alt_sprite_on, shaggy_pose_45, closeup_picture, bar_show_if_hidden | - | 29, 29, 58, 29, 29, 29, 29, 812 (synthetic) |
| after-call pieces: wait_shaggy_path, shaggy_anim_by_x_wait, alt_sprite_on_wait, wait_dpad_choice, follow_mode_on, camera_pan_left_wait, shaggy_pose_45 _go/_place/_wait/_done | - | 200, 100, 100, 200, 300, 200, 50/50/200/50 (synthetic, from $441C) |

The one failing variant: walk_obj state `004C20_w1_front_lot/00000` (Ha Ha Carnival) poked to
the Blake's Hotel op at $149518 ("walk to #64 Shed"). Object 64 does not exist in that
mystery, its hotspot index is 0, and the hotspot-table read goes through a garbage pointer
into unmapped cartridge space (`$87EE3C`), where the 68k reads open bus (the last prefetched
word). C cannot reproduce open-bus values; the real data never does this (cross-mystery
pokes are an artifact of `script_pokes.py`).

**Not registered** (`-DSCRIPT_UNTESTED`), all because nothing in the script data uses them
(`build/scripts/op_starts.txt`): ops $00, $12, $1D; sub-ops $00, $08, $37 (shaggy_pose_47).
Also without states: the Scooby tail of walk_obj ($4D48) and Shaggy's leg setups $51E4,
$522E, $5276 (the planner range runs the same C for them).

**Still 68k** in the subsystem, with reasons:
* sub-ops $05/$0C fullscreen_picture, $1E ending_credits, $27 minigame_throw, $32
  minigame_tug: long interactive or end-of-game sequences with many waits and no states
  (no route reaches them; they would need a late-game route plus their own range chains).
* sub-op $02 room_fade_in (`bsr room_fade_in / rts`), $04's entry (two `bsr`s), the entries
  of $15/$21/$25 (`bsr actor_animate` first), and the 68k VBlank spins (`$4836` bar idle,
  `$44BA`/`$3026` sprite DMA idle): nothing but calls / wait primitives.
* the TALK tail after `talk_leave` (`bsr wait_vblank / inventory_refresh /
  inventory_arrows_update / rts`) and action_execute's own pushes (first pass).

## Play test

`tools/py/script_play.sh` compares room, inventory and the 29 story bytes at the end of a route between two `--hle` specs:

* `all` vs `all` minus every script.c function: the Lobby route and the TALK route end identically (only sprite pixels differ). `port/tools/hosttest.sh`: all PASS; Lobby HLE-on vs harness 98.91 % identical.
* The #29 story routes and the natural routes are open-loop and frame-exact (poked pointer positions, timed presses). They already desynchronise with the other subsystems' C code alone (`all,-script.c` vs `none`), and also with only the script.c functions on (`none,+script.c` vs `none`): warps reach the right room and the first actions work (a wheel is taken in w0_mine_wheels, the Front Lot sign is taken), then later presses miss. This looks like timing (ranges take no CPU time), not a script decision: every function involved is equal under difftest. A closed-loop play test (#29's `story.py` driving the port) would settle it.
* Found and fixed by the play test: a DMA started from a nested `hle_call68k` in live play stalls the CPU clock for seconds (bar_show restoring the verb bar). The sandbox finishes DMA at once, so difftest cannot see it. DMA calls stay 68k (`bar_show_loop` exits before them, `pal_cycle` tail-jumps to its `bsr vdp_dma_cram`).

## Trace validation of the disassembly

`tools/py/scripttrace.sh OUT ROUTE...` (VM) plays harness routes with `trace` on and keeps the
dispatcher's opcode reads; `scriptdis.py --check` compares them with the static disassembly.
Routes: `port/scripts/verbs.txt` (Lobby, random verbs on every object), `port/scripts/capture.txt`,
all 12 `tools/scripts/natural/*.txt` routes (both mysteries, from the real start).
Result: **939 distinct op addresses, 657,899 dispatches, 0 mismatches**: every executed op
starts exactly where the disassembly says an op starts. Executed opcodes: 01 02 03 04 05 06 08
09 0B 0C 0D 0E 0F 10 11 13 14 15 16 17 18 19 1A 1B; sub-ops 02 03 04 0E 0F 1A 1B 1C 1D 39 3A 3B.

## Tools

| tool | what |
| --- | --- |
| `tools/py/scriptdis.py` | disassembler (`build/scripts/`), `--range`, `--check` |
| `tools/py/scripttrace.sh` | dispatch trace of routes + check |
| `tools/py/script_dt.sh`, `script_dt_all.sh` | difftest on capture.txt, then every call on extra routes (4 parallel jobs) |
| `tools/py/script_pokes.py`, `script_dt_pokes.sh` | `--poke` variants: A5 moved to other ops of the same kind |
| `tools/py/capture_at.py` | insert `capture` commands into a route at given frames |
| `tools/py/script_play.sh` | play-test: routes with `--hle all` vs `--hle none`, compare room / inventory / story bytes |
| `port/scripts/script_talk.txt` | Lobby + a poked TALK to the Bellhop (dialog menu, choices) |
| `poke ADDR W VAL` | new port-script command (harness syntax), used by the TALK route and #29's story routes |
| `tools/py/script_survey.sh` | which op / sub-op handlers (and extra addresses) each route reaches, with hit counts (frameprof watches) |
| `tools/py/script_dt_multi.sh` | `difftest capture-multi` over all routes (one pass per route), then difftest; `DTOPTS` for pokes |
| `tools/py/script_pokes.py --dispatch OP` / `--sub SUB` | pokes for synthetic dispatch (A5 + A0, + D0 for sub-ops) |
| `tools/py/script_dt_entry.sh` | synthetic-dispatch difftest (`--entry`) of handlers no route reaches; `EXTRAS` adds RAM pokes |
| `tools/py/script_dt_pass2.sh` | all second-pass difftests (routes, pokes, synthetic) |
| `port/scripts/script/intro_w0.txt`, `intro_w1.txt` | title -> the mystery's intro cutscene, not skipped (face, wait_stop, test_moving, camera pan, put-and-animate) |
