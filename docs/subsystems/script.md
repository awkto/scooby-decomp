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
* **Blocking handlers**: left in 68k for now, or split into ranges in front of their
  blocking calls (`run_object_events` is done this way).
* Callees of other subsystems go through `hle_call68k`: `sound_play`/`sound_stop` (sound),
  `vdp_dma_cram` (VDP), `verb_label_redraw` (#17).

In a native build without the CPU, the ranges collapse into one loop
`while (pc < end) { handler[op](mode); if (stop) break; }` and each blocking handler is a
coroutine that yields where the 68k code calls `sub_00B0B8` / `wait_vblank` / the walk or
room-load functions.

## Functions

Status: **C** = replaced and difftested; **68k** = not replaced (reason given).
"route" = all calls on the capture route when fewer than 200.

### Dispatch and drivers

| address | name | what | status |
| --- | --- | --- | --- |
| $0023DC | script_run | scan-mode loop head (range, exits $23F2 jsr / $2404 rts) | see table below |
| $0023F4 | script_run_next | after a handler: stop flag or loop (range) | |
| $002406 | script_run_frame | execute-mode loop head incl. the Start-skip test (range, exits $2430 / $2444) | |
| $002432 | script_run_frame_next | after a handler (range) | |
| $002320 | script_run_nested | TALK choice collection: flags = 4, count = 0, saves A5/A6/cb, script_run | 68k (calls script_run, stack juggling) |
| $0019B0 | action_execute | verb on object: collect TALK choices, scan for `case`, run the body, TALK menu, failure text | 68k (blocks; large) |
| $001C0C | sub_001C0C | TALK dialog menu | 68k (blocks: pad loops, bar slide) |
| $00221A / $002278 | room enter / after-fade script | | 68k (call script_run_frame) |
| $00AFDA | run_object_events | event scripts of pending objects | object walk in C: ranges $AFEA / $B05E / $B09A |

### Opcode handlers (`script_op_table` $002354)

| op | address | name | blocks | status |
| --- | --- | --- | --- | --- |
| 00 | $00250C | nop | | C |
| 01 | $00246A | case_verb | | C |
| 02 | $0025AC | talk_choice | | C |
| 03 | $002732 | if (+ condition $002796) | | C |
| 04 | $00274A | if_else | calls the loop | 68k |
| 05 | $0028A0 | move_obj | yes | 68k |
| 06 | $002B5A | anim_wait | yes | 68k |
| 07 | $002BF8 | show_obj | inventory_refresh | 68k |
| 08 | $002C56 | set | | C |
| 09 | $002D5E | set_verb | | C |
| 0A | $002DC0 | set_icon | inventory_refresh | 68k |
| 0B | $002DF2 | set_state | spin (bit 15) | 68k |
| 0C | $002E9E | sys | some sub-ops | 68k |
| 0D | $0048B6 | sound | | C |
| 0E / 1B | $0049BA | walk | yes | 68k |
| 0F | $004C20 | walk_obj | yes | 68k |
| 10 | $004D80 | say | yes | 68k |
| 11 | $004F0E | goto_room | yes | 68k |
| 12 / 14 | $004F50 / $002E7E | set_hotspot | | C |
| 13 | $002446 | on_event | | C |
| 15 | $0048D8 | wait | yes | 68k |
| 16 | $002AD0 | freeze | | C |
| 17 | $004FBE | wait_anim | yes | 68k |
| 18 | $004EE4 | goto_room_cut | yes | 68k |
| 19 | $002B0E | unfreeze_wait | yes | 68k |
| 1A | $002626 | pal_cycle | | C |
| 1C / 1D | $00255E / $002510 | test_moving / test_anim_done | | C |
| 1E | $00268E | face | yes (bit 15) | 68k |
| 1F | $004F70 | wait_stop | yes | 68k |
| 20 | $0048F8 | stop_path | | 68k (no calls on any route) |
| 21 | $004944 | path | | 68k (no calls on any route) |

### Sub-op handlers (`script_subop_table` $002EB4)

| sub | address | name | status |
| --- | --- | --- | --- |
| 01 | $00453E | end | C |
| 0D / 0E | $004482 / $00448C | camera lock on/off | C |
| 1A / 1B | $004274 / $004298 | pal_flash on/off | C |
| 38 / 39 | $002FD6 / $002FE0 | skippable off/on | C |
| 3A / 3B | $002FC2 / $002FCC | $FF09EB bit 5 on/off | C |
| others | | see docs/script.md section 6 | 68k (blocking, or never called on a route) |

## Difftest results

(filled in below as functions pass)

## Trace validation of the disassembly

`tools/py/scripttrace.sh OUT ROUTE...` (VM) plays harness routes with `trace` on and keeps the
dispatcher's opcode reads; `scriptdis.py --check` compares them with the static disassembly.
Routes: `port/scripts/verbs.txt` (Lobby, random verbs on every object), `port/scripts/capture.txt`,
all 12 `tools/scripts/natural/*.txt` routes (both mysteries, 9 rooms each... from the real start).
Result: **939 distinct op addresses, 657,899 dispatches, 0 mismatches** (every executed op
starts exactly where the disassembly says an op starts). Executed opcodes: 01 02 03 04 05 06 08
09 0B 0C 0D 0E 0F 10 11 13 14 15 16 17 18 19 1A 1B; sub-ops 02 03 04 0E 0F 1A 1B 1C 1D 39 3A 3B.
