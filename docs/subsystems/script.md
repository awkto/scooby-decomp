# Script interpreter: functions and port status (GitLab #19)

The byte code, data format and every opcode are described in `docs/script.md`. This page
lists the functions of the subsystem, how they are split for the C port, and their difftest
status. C: `port/src/game/script.c`.

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
  after the call that loops back to the call or finishes the op. Stack pushes (`movem`,
  `move.x ...,-(sp)`) stay 68k because a range must exit with the SP it was entered with.
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
| $002320 | script_run_nested | TALK choice collection (flags 4, count 0, saves A5/A6/cb) | 68k (only stack juggling around script_run) |
| $0019B0 | action_execute, _verb_pass ($1A16), _run_case ($1A46), _after ($1A84), _after_talk ($1A9C), _done ($1BA0) | verb on object: header, TALK collection, `case` scan, body, failure text "I can't ...", then_talk | C (6 ranges; pushes, script calls and `sub_00A130` text stay 68k) |
| $001C0C | talk_menu_begin ($1C10), _save ($1C4A), _slide ($1C9E), _level ($1CDC), _draw ($1D22), _input ($1DB8), _pick ($1E5A) | TALK dialog menu: bar slide-out, list unused choices, Up/Down, pick | C up to the pick; running the picked choice ($1E7A-$20D0: reply text, pose, body, nesting, bar slide-in) stays 68k |
| $00221A / $002278 | room_enter_script ($2226 + $2266), room_after_fade_script ($2284 + $22C4) | room scripts | C (ranges around the pushes) |
| $00AFDA | run_object_events_setup ($AFEA), _matched ($B05E), _next ($B09A) | the object walk of the event scan; scan/run calls stay 68k | C |

### Opcode handlers (`script_op_table` $002354)

| op | address | name | kind | status |
| --- | --- | --- | --- | --- |
| 00 | $00250C | nop | | 68k: never executed by the data, nothing to test |
| 01 | $00246A | case_verb | func | C |
| 02 | $0025AC | talk_choice | func | C |
| 03 | $002732 | if (+ condition $002796) | func | C |
| 04 | $00274A | if_else | range (condition; the nested run stays 68k) | C |
| 05 | $0028A0 | move_obj (+ $29EA, $2A2C) | ranges | C |
| 06 | $002B5A | anim_wait (+ $2B92, $2BE2) | ranges | C |
| 07 | $002BF8 | show_obj | | 68k: no executions on any route (inventory_refresh path) |
| 08 | $002C56 | set | func (toggle mode with the original's A3 bug -> hle_call68k_orig; unused by the data) | C |
| 09 | $002D5E | set_verb | func | C |
| 0A | $002DC0 | set_icon | | 68k: no executions on any route |
| 0B | $002DF2 | set_state | range (exit at the redraw spin $2E6E) | C |
| 0C | $002E9E | sys (+ $2EAE) | ranges around `jsr (a0)` | C |
| 0D | $0048B6 | sound | func | C |
| 0E / 1B | $0049BA | walk | | 68k: pushes D7 first; calls shaggy_walk_to/scooby_walk_to (walking, blocking) |
| 0F | $004C20 | walk_obj | | 68k: same |
| 10 | $004D80 | say | ranges (text.c, #20) | C: 6 ranges `script_op_say*` split at the stack pushes/pops and the waits ($004D80 .. $004EB8), see docs/subsystems/text.md section 8 |
| 11 | $004F0E | goto_room (+ $4F34, $4F4A) | ranges around palette_read_cram / room_reload | C |
| 12 / 14 | $004F50 / $002E7E | set_hotspot / set_hotspot2 | func | 14 C; 12 same code, not registered (never executed) |
| 13 | $002446 | on_event | func | C |
| 15 | $0048D8 | wait (+ $48E8) | ranges | C |
| 16 | $002AD0 | freeze | func | C |
| 17 | $004FBE | wait_anim (+ $4FF2) | ranges | C |
| 18 | $004EE4 | goto_room_cut (+ $4F08) | ranges around room_load | C |
| 19 | $002B0E | unfreeze_wait (+ $2B44) | ranges | C |
| 1A | $002626 | pal_cycle | func | C |
| 1C / 1D | $00255E / $002510 | test_moving / test_anim_done | func | written, not registered: never executed on any route |
| 1E | $00268E | face | | 68k |
| 1F | $004F70 | wait_stop | | 68k |
| 20 / 21 | $0048F8 / $004944 | stop_path / path | | 68k: no executions on any route |

### Sub-op handlers (`script_subop_table` $002EB4)

| sub | address | name | status |
| --- | --- | --- | --- |
| 01 | $00453E | end | C (SUBOP_BIT) |
| 0D / 0E | $004482 / $00448C | camera lock on / off | 0E C; 0D written, not registered (no states) |
| 1A / 1B | $004274 / $004298 | pal_flash on / off | C |
| 04 | $004570 | wait_button (test after the two calls) | C (range) |
| 1C | $004862 (+ $4874) | bar_show | C (ranges) |
| 1D | $00482E (+ $4844, $4850) | bar_hide | C (ranges; the wait-for-idle spin $4836 stays 68k) |
| 38 / 39 | $002FD6 / $002FE0 | skippable off / on | 39 C; 38 written, not registered (no states) |
| 3A / 3B | $002FC2 / $002FCC | $FF09EB bit 5 on / off | C |
| others | | see docs/script.md section 6 | 68k: blocking (fades, pans, mini-games, credits) or never executed on a route |

## Difftest results

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

Not registered (no states on any route; enable with `-DSCRIPT_UNTESTED`): op $12 `set_hotspot` ($004F50, same code as the tested $2E7E), ops $1C/$1D `test_moving`/`test_anim_done`, sub-ops $0D ($004482) and $38 ($002FD6). `pal_cycle`'s DMA path was changed to a tail jump after its poke run; it was re-run on the route states only.

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
