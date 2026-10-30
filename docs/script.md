# The adventure script interpreter (GitLab #19)

Everything the player can do with an object, every cutscene, every room-entry effect and every
timed object animation is a **script**: big-endian word byte code stored in ROM, run by a small
interpreter at `$0023DC`-`$005000`. This page describes the VM, the data format, where scripts are
attached, and every opcode. Tools:

* `tools/py/scriptdis.py`: disassembles all 8061 script ops of both mysteries into
  `build/scripts/w{0,1}_{start,rooms,objects}.txt` (ROM-derived, gitignored) and checks emulator
  traces against the disassembly (`--check`, section 9).
* C port and per-handler status: `docs/subsystems/script.md`, `port/src/game/script.c`.

Disassembly excerpt (the Bellhop in the Lobby, mystery 0; quoted dialogue elided as `<text>`):

```
146BD0  case TALK:
146BD8      walk Shaggy -> spawn 6 speed 1 anim 0
146BE2      face Shaggy dir 0
146BE8      say "<text>"  [colour 6, pal 0, talkanim]
146BF0      anim_wait #6"Bellhop" anim 9
146BF6      say "<text>"  [colour 14, pal 1]
146BFE  talk_choice "<text>" reply "<text>" pose 11:
146C0C      talk_choice "<text>" reply "<text>" pose 10:
146C1A          anim_wait #6"Bellhop" anim 11
146C20          say "<text>"  [colour 14, pal 1]
146C28          if #172"Doll".room == 1:
...
146C84                  set_state #172"Doll" = $00A3
146C8A                  move_obj #172"Doll" -> room16"The Lobby"
...
146CD4                  set F01.0 = 1
```

## 1. VM state

| what | where | meaning |
| --- | --- | --- |
| script pc | **A5** | address of the current op (always even, in ROM) |
| script end | **A6** | the run stops when A5 >= A6 |
| verb | **D7** | verb being matched (`case` ops), set by `action_execute` from `ram_action_verb` |
| mode | **D0** / Z flag at handler entry | `script_run` passes D0 = 0 (Z set) = **scan mode**; `script_run_frame` passes D0 = 1 (Z clear) = **execute mode**. Every handler starts with `beq`/`bne` on the Z flag that `moveq` left. |
| `ram_script_flags` `$FF0AC9` | byte | bit 0 **stop** (a handler asks the loop to return; a `case`/`on_event` match or `sys end`); bit 1 match `case` ops (verb pass); bit 2 collect `talk_choice` ops; bit 4 match `on_event`; bit 5 a matching `case ... with X` waits for the second object; bit 6 a verb script ran (no "I can't ..." message); bit 7 walk-on script already ran for the current hot spot (main loop) |
| `ram_script_flags2` `$FF0ACA` | byte | bit 0 Start was pressed (VBlank sets it while Start is held); bit 1 cutscene is skippable (`sys skippable_on`): with bits 0+1 set `script_run_frame` returns at once and text boxes close; bit 2 end the TALK menu (`sys talk_end`); bit 3 run TALK after this action (`sys then_talk`, target from `$FF06DA`) |
| `ram_script_cb` `$FF0878` | long | the loop to use for nested blocks (`if_else` calls it): `script_run` ($23DC) or `script_run_frame` ($2406) |
| `ram_vblank_flags` bit 7 | `$FF09DC` | "a script is running": the player cannot walk or open the pointer |
| `$FF080E` | word | number of collected `talk_choice` ops (max 5) |
| `$FF0824`/`$FF082E`/`$FF0838`/`$FF084C`/`$FF0860` | 5 words/words/longs/longs/longs | per choice: pose, body length, choice text, reply text, body address |
| `$FF06B0` | word | object script header word 2: high byte = pronoun bits for the "I can't ..." message, low byte = TALK text colour/palette of the object |
| `ram_game_state` `$FF2A00` | bytes | **story flags** (meanings found so far: docs/walkthrough.md, #29), `Fxx.b` in the disassembly = bit b of byte `$FF2A00+xx`. The first 29 bytes are what the password stores (docs/passwords.md). Byte 0 is also used by the engine: bit 1 verb bar hidden, bits 2/3 results of `test_moving`/`test_anim_done`, bit 4 = `shaggy_walk_to` walks straight lines. |

There is no script stack: nesting uses the 68k stack (`if_else` pushes A5/A6 and calls the loop
recursively). The old name `ram_script_stack` for `$FF0328` was wrong: it is the inventory
name-table buffer (now `ram_inv_nametable`), also used by `minigame_tug`.

## 2. Dispatch loops

```
script_run        $0023DC   while (a5 < a6) { h = op_table[*(u16*)a5]; d0 = 0; h();   // scan mode
                                              if (script_flags & 1) break; }
script_run_frame  $002406   if ((flags2 & 3) == 3) return;                           // Start skips
                            while (a5 < a6) { h = op_table[*(u16*)a5]; d0 = 1; h();   // execute mode
                                              if (script_flags & 1) break; }
script_run_nested $002320   script_flags = 4; $FF080E = 0; push a5,a6,cb; cb = script_run;
                            script_run(); pop    -- collects the talk_choice ops of a script
```

* The opcode word indexes `script_op_table` `$002354` (34 longs; slot `$1B` reuses the handler of
  `$0E`). The handler advances A5 past its operands itself. A handler that does nothing in the
  current mode still skips its operands, so a scan walks the whole script.
* Op `$0C` (`sys`) indexes `script_subop_table` `$002EB4` (62 longs; slot `$0C` reuses `$05`).
* The dispatch reads the opcode with `move.w (a5),d0` at `$0023E2` (`script_run`) and `$002420`
  (`script_run_frame`). That ROM read is what `scriptdis.py --check` uses to trace execution.
* Handlers must preserve A5 semantics only; D0-D7/A0-A4 are scratch (except D7 = verb, which the
  walk handlers save and restore).

### Who runs scripts

| caller | script | mode |
| --- | --- | --- |
| `game_start` `$000CD0` (after `mystery_load`) | start-up script 1 then 2 (header +$30), with `VBF_SCRIPT` set | execute |
| `sub_00221A` (end of `room_load` `$002000`) | room **enter** script (room entry +$0C) | execute |
| `sub_002278` (end of `room_fade_in` `$002024`) | room **after-fade** script (room entry +$10) | execute |
| `action_execute` `$0019B0` (main loop, `VBF_SCRIPT` = a verb was clicked; and the walk-on check at `$001376` with verb 11) | object script of `ram_target_obj`: TALK collection, then verb `case` scan, then the case body | scan, then execute |
| `sub_001C0C` `$001C0C` (TALK only) | the dialog menu: shows the collected `talk_choice` texts, runs the chosen body, recurses into nested choices | execute |
| `run_object_events` `$00AFDA` (main loop, every pass; also while text is shown and in `sys wait_button`) | `on_event` block of every object in the current room (or carried) whose record +$18 bit 2 is set (set by the VBlank object timers `sub_007C04`) | scan (`flags = $10`), then execute |

### `action_execute` in detail

1. A5 = object script body (header: `[len][word → $FF06B0][word]`). **Quirk:** A6 = header +
   len, 6 bytes short of the real end (header + 6 + len, which `run_object_events` uses), so ops
   starting in the last 6 bytes are never scanned here. No `case`/`talk_choice` can start there
   (they are 8/14 bytes long), so it has no effect on the data.
2. `script_run_nested`: collect `talk_choice` ops at the top level (count `$FF080E`).
3. `ram_script_flags = 2`, D7 = verb, `script_run`: scans for `case <verb>` (ops inside taken `if`
   blocks are scanned too, so a case can be conditional). `case X with Y` (USE/GIVE) matches only
   when `ram_target2_obj` = Y; if no second object was chosen yet it sets flag bit 5 and
   `ram_verb_state` `$FF09DE` bit 0 ("waiting for the second object"). USE clears and GIVE sets
   `$FF09E8` bit 0 (the "with"/"to" word of the status line).
4. On a match (flag bit 0, A5 = the case op): A6 = A5 + len, A5 += 8, flag bit 6, cb =
   `script_run_frame`, run the body.
5. TALK (verb 8): if choices were collected, `sub_001C0C` runs the menu (it blocks).
6. No match and not walk-on: the default failure text `verb_fail_msgs` `$02FEA0` with `\1`
   replaced by "that"/"him"/"her" from the pronoun bits.
7. `sys then_talk` (flags2 bit 3) re-enters `action_execute` with verb TALK.

## 3. Where scripts are stored

All offsets come from the mystery header (`$031AF2` → `$0FCA0C` Blake's Hotel, `$0FCA40` Ha Ha
Carnival; see docs/formats.md). Fields used by scripts:

| header | meaning |
| --- | --- |
| +$04 | object init records (14 bytes, +4 if byte 12 bit 1: x, y), up to +$08. Count: 181 (mystery 0), 155 (mystery 1) |
| +$08 | room table, 0x14 bytes per room (room 2 first); +$0C = enter script, +$10 = after-fade script (offsets from +$2C) |
| +$1C, +$24 | hot-spot rectangles (`set_hotspot`, `walk_obj` target positions) |
| +$20 | **text base**: NUL-terminated ASCII; room names (entry +$00), object names, `say` and `talk_choice` texts are long offsets from here |
| +$28 | **object table**: 8 bytes per object (object n is entry n-3): long script offset (from +$2C), long name offset (from +$20) |
| +$2C | **script base** |
| +$30 | start-up script: `[start room][music][len] body1 [len2] body2` |

Script headers: object `[len][pronoun/colour][word, $FFFF]` + body; room `[len][word]` + body;
`len` = body length in bytes. The scripts tile `$1467AC-$15059A` (mystery 0) and
`$1B3D38-$1BB94A` (mystery 1) exactly: no gaps, no overlaps, no unreachable code
(`scriptdis.py` checks every block ends exactly at its declared end). Several objects share one
script (dummy objects point at the same empty script).

## 4. Operand conventions

* All operands are words unless noted; texts are longs (offset from header+$20).
* **who** (actor/object reference): 1 = Shaggy, 2 = Scooby, n >= 3 = object record n-3
  (`ram_objects` `$FF1200` + 26*(n-3)); the same numbering as `ram_target_obj`. Bit 15 on the who
  word of `move_obj`, `set_state`, `walk*`, `face` changes blocking (see the table).
* **room**: room number as in `$FF06AC`; 1 = the inventory, 0 = nowhere.
* **flag**: (byte, bit) = bit `bit & 7` of `$FF2A00 + byte` (`btst`/`bset` on memory are byte ops).
* **object field**: (field offset, who): word at `ram_objects + 26*(who-3) + (field & ~1)`.
  Special case in conditions: (6, 1) = "Shaggy's room" = `$FF06AC`.
* **condition** (ops `$03`/`$04`, 16 bytes): `[op][skip][else_len][a][b][c][d][mode]`:
  lhs = mode bit 0 ? flag(a, b) : bit 1 ? field(a, b) : immediate a;
  rhs = mode bit 2 ? flag(c, d) : bit 3 ? field(c, d) : immediate c;
  compare: bit 4 `==`, bit 5 `>`, bit 6 `<` (signed), none `!=`.
* Object record fields (26 bytes; see also docs/subsystems/mainloop.md): +$00 state/graphic
  word (`set_state`), +$02 hot-spot number (`set_hotspot`), +$04 inventory icon (`set_icon`),
  +$06 room, +$08 facing/offset used by `walk_obj`, +$0A timer (VBlank), +$0C..+$11 script
  variables (`var`; object 0 +$0C is the result of the mini-games and `wait_dpad_choice`),
  +$12/+$14 actor x/y, +$16.. default verb bytes, +$18 flags (bit 0 redraw/animate, 1 actor,
  2 event pending, 3 depth sorting), +$19 actor slot − 2 ($FF none).

## 5. Opcodes

Length = bytes the op occupies (A5 advance). "Scan" = what it does in `script_run` mode; every
op not listed as acting in scan mode only skips. **B** = may block (waits for VBlank-driven
state through `sub_00B0B8` "run one frame of actors", `wait_vblank`, or a callee that does).

| op | name | length | operands | behaviour | B |
| --- | --- | --- | --- | --- | --- |
| 00 | nop | 2 | | | |
| 01 | case_verb | len | verb, with (0 or who), len; body at +8 | scan with flag bit 1: verb == D7 (and second object) → stop with A5 here. Execute: skip the block | |
| 02 | talk_choice | len | text.l, reply.l, len, pose; body at +$E | scan with flag bit 2: record the choice (max 5). Otherwise skip | |
| 03 | if | skip | skip, -, cond (4 words + mode) ; body at +$10 | both modes: true → enter the body (A5+16), false → A5 += skip | |
| 04 | if_else | skip+else | skip, else_len, cond; then-body at +$10, else-body at +skip | both modes: true → run the then-body with the loop in `ram_script_cb` (recursive), then skip the else-body; if the nested run stopped (match/end), pop and return stopped. False → A5 += skip (falls into the else-body) | B |
| 05 | move_obj | 6 | who (bit 15 = no inventory refresh/no wait), room | puts the object into a room. Actors (+$18 bit 1) leaving the current room free their sprite slot; arriving in the current room claims a free actor slot 2..5, loads the sprite (`dat_032670` lookup by hot-spot id), and waits for its first animation loop. Static objects get +$18 bit 0 (redraw); into/out of room 1 updates `ram_inv_list` and the inventory page | B |
| 06 | anim_wait | 6 | who, anim | Shaggy: `$FF0488`, Scooby `$FF048A`, object actor: `$FF048C+2*slot`; sets the animation-start bit in `$FF0AB5` and waits until the animation looped (`ram_actors_anim_looped`). An object without an actor slot: only stores anim in record +$00 | B |
| 07 | show_obj | 4 | who | object in the current room: +$18 bit 0 (redraw). Carried: refresh the inventory page | B |
| 08 | set | 12 | dst_a, dst_b, src_a, src_b, mode | src = mode bit 2 flag(src) / bit 3 field(src) / bit 7 `rng_next(src_a)` / immediate. mode bit 0: dst = flag(dst); bit 4 → set/clear by src != 0, else toggle (**bug**: the EOR indexes with A3 instead of D3; never used by the data). Otherwise dst = field: bit 4 `=`, 5 `-=`, 6 `|=`, else `+=` | |
| 09 | set_verb | 6 | who, byte verb, byte slot | object default verb byte +$16+slot = verb; repaints the verb label if the object is hovered; deselects the current verb | |
| 0A | set_icon | 6 | who, icon | record +$04 = icon; carried → refresh inventory | B |
| 0B | set_state | 6 | who (bit 15 = wait), state | actors: +$00 = state. Others: +$02 = state if nonzero; in the current room +$00 = state (0 → +$00 \|= $C000 "hidden") and +$18 bit 0; with bit 15 spin until VBlank's object code cleared bit 0 | B (bit 15) |
| 0C | sys | 4 | subop | `script_subop_table[subop]()`, see section 6 | B (some) |
| 0D | sound | 4 | id (bit 15 = stop) | `sound_play(id)` / `sound_stop(id & $7FFF)` | |
| 0E | walk (spawn) | 10 | who (bit 15 = no wait), speed (<0 = teleport), anim (<0 = keep), spawn | target = `ram_room_spawns[spawn]` (cells × 8). Shaggy: teleport (set x, y, anim and wait loop) or `shaggy_walk_to` `$5002`; Scooby: teleport or `sub_00566A`; object actor: `sub_004B24` (sets target, step = speed × base, waits until arrived unless bit 15) | B |
| 0F | walk_obj | 12 | who, target who, anim, dx, speed | walk next to another actor/object: target position + dx (static objects: hot-spot rectangle +8/+10); Shaggy/Scooby then face the object's +$08 direction | B |
| 10 | say | 8 | attr, text.l | text box: attr bits 0-3 text colour, 4-5 palette (both via `sub_007A2A`), bit 12 Shaggy talking animation, bit 13 turn Shaggy to direction bits 14-15 first and back afterwards. `sub_00A130` prints and waits for a button / timeout | B |
| 11 | goto_room | 8 | room, spawn, facing | fade out, `$FF06AC` = room, `$FF069E` = spawn*4, `room_reload` (load + fade in; runs the new room's enter and after-fade scripts) | B |
| 12 | set_hotspot | 6 | who, id | record +$02 = id (14 is identical) | |
| 13 | on_event | len | len; body at +4 | scan with flag bit 4: stop with A5 here (the event body is then run). Otherwise skip | |
| 14 | set_hotspot2 | 6 | who, id | same code as 12 | |
| 15 | wait | 4 | frames | `$FF08AE` = frames; runs `sub_00B0B8` until it goes negative | B |
| 16 | freeze | 4 | who | sets the actor's bit in `$FF0ABF` (actor frozen: ignored by idle/follow logic) | |
| 17 | wait_anim | 4 | who | waits for the actor's `ram_actors_anim_done` bit | B |
| 18 | goto_room_cut | 8 | room, spawn, facing | like 11 without the fade (`room_load` only) | B |
| 19 | unfreeze_wait | 4 | who | sets `ram_actors_depth_new` bit, waits for the animation loop, clears the `$FF0ABF` bit | B |
| 1A | pal_cycle | 10 | slot 0-4, first, last, speed | palette cycling slot (`$FF087C` table, enable `$FF0ACD`); speed < 0: disable and DMA the colours from `ram_pal_target` | |
| 1B | walk (xy) | 12 | who, speed, anim, x, y | same handler as 0E with an explicit pixel target | B |
| 1C | test_moving | 4 | who | byte 0 bit 2 of `$FF2A00` = actor walking (`ram_actors_moving`) | |
| 1D | test_anim_done | 4 | who | byte 0 bit 3 = `ram_actors_anim_done` bit | |
| 1E | face | 6 | who (bit 15 = wait), dir | Shaggy: turn animation from `dat_0FC740` [old dir][new dir]; Scooby: `scooby_pose_table`; with bit 15 wait for the animation | B (bit 15) |
| 1F | wait_stop | 4 | who | waits until the actor is neither walking nor on a path | B |
| 20 | stop_path | 4 | who | if walking: clear the moving bit; else set `ram_actors_path_once` (path stops at its end) | |
| 21 | path | 8+len | len, who, delay, data[len] | start a scripted path (docs/subsystems/walking.md 5.4): `ram_actor_path_ptr[n]` = data, delay, position 0, `ram_actors_on_path` bit | |

Block ops `01`, `02`, `03`, `04`, `13` measure their length from the op address. The VM has no
jumps: all control flow is these blocks, `sys end` (stop), and Start-skipping.

## 6. Sub-ops (`sys`, op $0C)

Flags without a known meaning are named after their bit (`flag09e8_1_on` etc.).

| sub | name | what | B |
| --- | --- | --- | --- |
| 00 | nop | | |
| 01 | end | `ram_script_flags` bit 0: stop the current script | |
| 02 | room_fade_in | `room_fade_in` (status line, palette fade in, after-fade script) | B |
| 03 | fade_out | `palette_read_cram` with frame work paused (fade to black) | B |
| 04 | wait_button | loop: object events + one frame, until A or B is pressed (edge) | B |
| 05 | fullscreen_picture | hide the verb bar, save VRAM, show a full-screen picture (`dat_02B33A`, maps `$02EF7A`/`$02F97A`) with a cycling palette for 200 frames or until A/B, restore | B |
| 06 | bar_show_if_hidden | if `$FF2A00` bit 1: sub 1C | B |
| 07 | closeup_picture | wait for actors idle, draw the 11×11 close-up `dat_031DFA`/`dat_0320CC` on plane A, `ram_verb_state` bit 6 | B |
| 08 | closeup_off | clear `ram_verb_state` bit 6 | |
| 09 / 0A | scooby_away / scooby_back | `ram_verb_state` bit 7 and `$FF0ABF` bit 1 (Scooby frozen) | |
| 0B / 36 | scooby_pose_on / off | Scooby pose = facing+4, `$FF09EB` bit 2 | |
| 0C | bar_hide_once | same handler as 05: if the bar is visible, set bit 1 and slide it out (sub 1D) | B |
| 0D / 0E | flag09e8_1 on/off | `$FF09E8` bit 1: camera locked | |
| 0F / 10 | camera_pan_right / left | lock the camera and scroll to the room's right/left edge (2 px/frame, 8 with `$FF09E9` bit 4), wait | B |
| 11 / 12 | flag09e8_2 on/off | `$FF09E8` bit 2 | |
| 13 / 14 | sfx_random_a / b | random sound `$25`/`$27` / `$28`/`$2A` (Scooby / Shaggy exclamations; `rng_next` with range 2 gives 0 or 1, so the middle sound is never picked) | |
| 15 / 16 | follow_mode_on / off | wait for Shaggy's animation; `$FF08B0` = first free actor slot; `$FF09E8` bit 3 | B |
| 17 / 18 / 19 | overlay_a_on / overlay_b_on / overlay_off | sprite overlay effects (`dat_0321BE` / `dat_0322BE` to VRAM `$FF00`), `$FF09E8` bits 4/5 | |
| 1A / 1B | pal_flash_on / off | `$FF09E8` bit 7 palette effect (`dat_03247C`) | |
| 1C | bar_show | slide the verb bar back in and redraw it, clear `$FF2A00` bit 1 | B |
| 1D | bar_hide | wait for the bar to be idle, save it, slide it out | B |
| 1E | ending_credits | the end: decompress and scroll the credits, wait, fade, `soft_reset` | B |
| 1F / 20, 23 / 24, 29 / 2A, 2D / 2E, 30 / 31 | `$FF09E9` bits 0, 1, 3, 6, 7 | bit 7 = no random idle animation for Shaggy (`sub_0020DA`), bit 4 = fast camera / alt sprite | |
| 21 | wait_dpad_choice | wait for Left/Right/Down (pad bits 2/3/1): object 0 var = 0/1/2 | B |
| 22 | shaggy_anim_by_x | Shaggy animation $49/$4A by x < $114, wait | B |
| 25 | wait_shaggy_path | wait until Shaggy's scripted path ended | B |
| 26 | test_shaggy_path | object 0 var = Shaggy on a path ? 0 : 1 | |
| 27 | minigame_throw | interactive throw mini-game (D-pad aims, A/B throws); result in object 0 var | B |
| 28 | talk_end | flags2 bit 2: leave the TALK menu | |
| 2B / 2C | shaggy_alt_sprite_on / off | Shaggy sprite set `$0B5D02` (and back to `$032700`) | B (2B) |
| 2F | then_talk | flags2 bit 3, `$FF06DA` = object 0 var: TALK to that object after this action | |
| 32 | minigame_tug | interactive tug/fight mini-game with two meters (`$FF0A90`-`$FF0AA4`), result in object 0 var | B |
| 33 / 34, 3A / 3B, 3C / 3D | `$FF09EB` bits 0, 5, 6 | | |
| 35 / 37 | shaggy_pose_45 / 47 | Shaggy animation $45/$47 at a fixed position, waits for actors | B |
| 38 / 39 | skippable_off / on | flags2 bit 1 (Start skips the cutscene; with `$FF09ED` bit 2 (password game) bit 0 at once) | |

## 7. Text

Texts are plain NUL-terminated ASCII at header+$20 + offset. `say` prints through `sub_00A130`
(text box in the window plane `$D000`, word wrap at 30 columns, waits for a button or a
length-based timeout, `$FF080A`); `talk_choice` texts are drawn by `sub_009FA2` into the verb-bar
area. Text rendering itself (#20): docs/subsystems/text.md.

## 8. Blocking

Most execute-mode ops wait for something the VBlank side advances: an actor animation loop, a
walk, a countdown, a palette fade, a room load. The waits are either `sub_00B0B8` (one frame of
actor/sprite work, then it returns when VBlank has run) inside a loop, `wait_vblank`, or calls to
`shaggy_walk_to` `$5002` / `scooby_walk_to` `$56B0` / `room_load` / `room_fade_in` /
`inventory_refresh` / `palette_read_cram`. `tools/py/blocking.py` flags 18 ops and 22 sub-ops; it
misses one: `set_state` with bit 15 spins on the object's +$18 bit 0 (cleared by VBlank). The
dispatch loops therefore block through their handlers and stay in 68k (see
docs/subsystems/script.md for the split).

## 9. Validation by tracing

The harness `trace` command logs every ROM data read as `R <frame> <pc> <addr> <width>`. The
dispatchers read each opcode at pc `$0023E2`/`$002420`, so those lines are the executed script
pcs. `scriptdis.py --check FILE...` requires each of them to be an op start of the static
disassembly. Results are in docs/subsystems/script.md.
