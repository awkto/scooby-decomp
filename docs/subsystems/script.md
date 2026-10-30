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
  `sub_007A2A` font recolour (text).

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
| $001C0C | sub_001C0C | TALK dialog menu (choices, nested dialogs) | 68k (pad/bar-slide waits; open) |
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
| 10 | $004D80 | say | | 68k: pushes D7/font base around the text box (`sub_00A130`, blocks) |
| 11 | $004F0E | goto_room (+ $4F34, $4F4A) | ranges around palette_read_cram / room_reload | C |
| 12 / 14 | $004F50 / $002E7E | set_hotspot / set_hotspot2 | func | C |
| 13 | $002446 | on_event | func | C |
| 15 | $0048D8 | wait (+ $48E8) | ranges | C |
| 16 | $002AD0 | freeze | func | C |
| 17 | $004FBE | wait_anim (+ $4FF2) | ranges | C |
| 18 | $004EE4 | goto_room_cut (+ $4F08) | ranges around room_load | C |
| 19 | $002B0E | unfreeze_wait (+ $2B44) | ranges | C |
| 1A | $002626 | pal_cycle | func | C |
| 1C / 1D | $00255E / $002510 | test_moving / test_anim_done | func | written, **no states** (never executed on any route; registered but untested) |
| 1E | $00268E | face | | 68k |
| 1F | $004F70 | wait_stop | | 68k |
| 20 / 21 | $0048F8 / $004944 | stop_path / path | | 68k: no executions on any route |

### Sub-op handlers (`script_subop_table` $002EB4)

| sub | address | name | status |
| --- | --- | --- | --- |
| 01 | $00453E | end | C (SUBOP_BIT) |
| 0D / 0E | $004482 / $00448C | camera lock on / off | C |
| 1A / 1B | $004274 / $004298 | pal_flash on / off | C |
| 1C | $004862 (+ $4874) | bar_show | C (ranges) |
| 1D | $00482E (+ $4844, $4850) | bar_hide | C (ranges; the wait-for-idle spin $4836 stays 68k) |
| 38 / 39 | $002FD6 / $002FE0 | skippable off / on | C |
| 3A / 3B | $002FC2 / $002FCC | $FF09EB bit 5 on / off | C |
| others | | see docs/script.md section 6 | 68k: blocking (fades, pans, mini-games, credits) or never executed on a route |

## Difftest results

(see the end of this file)

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
