# Verb bar, pointer mode and inventory (issue #17)

Written for mouse support (#23): section 6 says exactly what a mouse has to write or call.
All addresses were checked in `src/scooby.s` and by driving the game in the harness
(`port/scripts/v17_*.txt`, `tools/py/verbscript.py`).

## 1. How the game's UI works (summary)

* The game has **no cursor in normal play**: the D-pad walks Shaggy.
* Pressing **A or B** (press edge, while walking) enters **pointer mode**: a 16x16 crosshair sprite
  appears at Shaggy's screen position, the D-pad moves it (2 px / frame), Shaggy stops.
  RAM flag: `ram_vblank_flags` ($FF09DC) **bit 3**.
* In pointer mode the crosshair is tested every frame against the room's objects (names show in the
  status line at the top) and against the verb bar / inventory when it is below line 168.
* **B** does things (select a verb while held over the bar, click an object with the selected verb).
  **A** leaves pointer mode again.
* **C** does *not* toggle pointer mode: it slides the bottom bar between the **verb view** and the
  **inventory view** (flag `ram_vblank_flags` **bit 4**; works in walking and pointer mode).
  Checked: from walking, `press C` only changes $FF09DC 44 -> 54 and `ram_hscroll_verb` -> $FF00.
  `press A` changes it 44 -> 4C and puts the crosshair at (x=$80, y=$14) (the Lobby spawn).

## 2. Functions

| address | name | what | status |
| --- | --- | --- | --- |
| $01006C | hblank_verb_bar | HBlank hook: busy-waits to line 167, then points plane A/B at the verb-bar plane at VRAM $A000 (reg $92 window, vscroll 0, hscroll = `ram_hscroll_verb`). Spins on VDP status and HV counter. | documented, stays 68k (cannot be difftested: busy-waits on beam position) |
| $005B5E | object_name_ptr | object id -> name string pointer | C, 140/140 |
| $005BDC | verb_label_redraw | rewrites the 4x2 tile label of one verb in colour variant 0/1/2 | C, 200/200 |
| $005D1C | inventory_build_page | builds icon tiles + name table for the inventory page | C, 3/3 (only 3 calls on any route, see section 8) |
| $005CB6 | inventory_upload | DMA icon tiles to VRAM $B000, name table to $AACC..; clears $AD80 | 68k |
| $005CF6 | inventory_refresh | build page, set flag bit5, wait for VBlank to upload | 68k (waits for VBlank) |
| $005C8E | (VBlank) verb_vblank_dma | bit5 of $FF09DC set and beam past line $E0 -> inventory_upload | 68k |
| $005B92 | inventory_arrows_update | sets arrow frames ($FF0804 up, $FF0806 down) | 68k (spin-waits $FF0AB1/2) |
| $005C5A / $005C74 | arrow frame setters | spin until the VBlank consumes the previous value | 68k |
| $005E4A | pointer_update | per-frame (from VBlank_main $00A6B0): walking, entering/leaving pointer mode, pointer movement, bar slide animation, pointer -> bar slot mapping, B-click | 68k, 2800 bytes, shared with Shaggy walking (another subsystem) |
| $00A984 | sprite_build | builds the SAT in `ram_sat_buf` incl. the crosshair | 68k, belongs to the sprite subsystem |
| $0019B0 | action_execute | runs the object's script for the chosen verb | 68k (script subsystem) |
| $00A828 | status_line_update | builds "Verb object [with object2]" in $FF08BC, prints with $00A054 | 68k |
| main loop $000E94-$0013B0 (inside `Reset`) | pointer hit-test of objects and verb bar slots, verb select | 68k, not a separate function |

Honest scope: the pointer-mode state machine lives in $005E4A and the main loop (inside `Reset`),
which are one big routine shared with player movement. I ported the three self-contained
helpers; the rest is documented for the mouse work, not replaced.

## 3. Screen layout and geometry (256x224 screen, pointer coordinates = screen pixels)

Lines 168-223 are the bottom bar: plane A/B tile rows 21-27 of the plane at VRAM $A000 (64 wide).
Row 21 (lines 168-175) is blank. The bar is built once per room from `verb_bar_names` ($030016):
three variants x 6 rows x 64 words, written to VRAM $AA80 (+ tile base `$FF0802`).
The variants differ only in palette: **0 normal, 1 selected (white), 2 default verb of the hovered
object (red)**. `verb_label_redraw` rewrites just one verb's 4x2 block.

Verb view (labels, tile units are 8 px):

| verb | id | label pixel box (x0,y0)-(x1,y1) | VRAM name table word addr of 1st tile |
| --- | --- | --- | --- |
| TAKE | 2 | (16,176)-(47,191) | $AB04 |
| LOOK | 10 | (56,176)-(87,191) | $AB0E |
| OPEN | 3 | (96,176)-(127,191) | $AB18 |
| PUSH | 7 | (136,176)-(167,191) | $AB22 |
| EAT | 9 | (176,176)-(207,191) | $AB2C |
| GIVE | 6 | (16,192)-(47,207) | $AC04 |
| TALK | 8 | (56,192)-(87,207) | $AC0E |
| SHUT | 4 | (96,192)-(127,207) | $AC18 |
| PULL | 1 | (136,192)-(167,207) | $AC22 |
| USE | 5 | (176,192)-(207,207) | $AC2C |

Cells are 40 px wide: column k spans x = 16+40k .. 55+40k (label 32 px + 8 px gap). The table that
maps bar position to verb id is `verb_ids` $02FE3C: `2,10,3,7,9, 6,8,4,1,5`
(`dat_02FE3A` is the same table with a leading 0 for slot 0 = none).
The arrow icon ("C" in a blue arrow) is at x=208-231, y=176-207 (VRAM $AAB6 + 10 words x 7 rows);
it is just decoration: C is the only way to use it. In the inventory view it is a left arrow at
the same place (x 8-39) and an up arrow / down arrow at the right (x 208+, upper row / lower row).

Inventory view: 8 slots, two rows of four, each an icon of 4x3 tiles (32x24 px):
slot column c = 0..3 at x = 48+40c .. 79+40c (VRAM column 38+5c+... in the plane at $AACC:
the bar scrolls 256 px, so inventory columns sit at plane columns 38.. = screen columns 6..).
Icons: item tiles DMA'd to VRAM $B000 (tile $580.. for slot 0, 12 tiles per slot), name table
rows at $AACC + n*$80 (n = 0..5), built by `inventory_build_page` from `ram_inv_list`
($FF0A1E: object indices, negative terminator; icon = object record +4 -> table at header+$18,
$182 bytes per icon: palette word + 384 bytes of tiles).
The page index `ram_inv_page` ($FF06EC) is a **row** index: paging moves one row (4 list entries):
slot (row r, col c) shows list entry 4*page + 4r + c.

Slide animation: pressing C sets `ram_bar_slide` ($FF0ACB) = $10; each frame
`ram_hscroll_verb` ($FF0800) += or -= $10 (bit 4 of $FF09DC says which way), 16 frames. While
sliding the crosshair snap is suspended. Idle value $FF.

## 4. RAM

| address | name | meaning |
| --- | --- | --- |
| $FF09DC bit 3 | ram_vblank_flags | **pointer mode on** |
| $FF09DC bit 4 | | **inventory view** (0 = verb view) |
| $FF09DC bit 7 | | an action is pending: main loop calls action_execute |
| $FF09DC bit 5 | | inventory name table/tiles need uploading |
| $FF06DC / $FF06DE | ram_pointer_x / _y | crosshair top-left, screen px, clamped x 0..$F7, y 0..$CF |
| $FF06E0 / $FF06E2 | ram_pointer_dx / _dy | speeds (2, or $28 = jump a whole cell on the bar) |
| $FF06EA | ram_ui_slot | cell under the pointer on the bar (set every frame by pointer_update, consumed and cleared by the main loop) |
| $FF06AE | ram_target_obj | object under the pointer: 0 none, 1 Shaggy, 2 Scooby, n>=3 object slot n-3 of `ram_objects` ($FF1200, 26-byte records) |
| $FF06B6 | ram_target2_obj | second object (Use X with Y, Give X to Y); `$FF09DE` bit 0 = "waiting for the second object" |
| $FF06C0 | ram_verb_cur | selected verb id (0 none) |
| $FF06C4 | ram_default_verb | default verb of the hovered object (object+$16, 10 = Look for Shaggy/Scooby); shown red |
| $FF06BE | ram_action_verb | verb passed to action_execute (11 = walk / nothing selected) |
| $FF06C2 | ram_verb_redraw | argument of verb_label_redraw (which verb to repaint) |
| $FF06EC | ram_inv_page | inventory row |
| $FF0A1E | ram_inv_list | carried objects |
| $FF0800 | ram_hscroll_verb | verb-bar plane hscroll (0 verb view, $FF00 = -256 inventory view) |
| $FF0ACB | ram_bar_slide | slide counter / $FF idle |
| $FF08BC | status string | "Take Archway" etc. for the top line |
| $FF09E0 / $FF09E2 | ram_pad1 / ram_pad_last | pad, active low; pointer_update runs *before* pad_read in the same VBlank, so it sees the pad of the previous frame |

Verb ids: 1 Pull, 2 Take, 3 Open, 4 Shut, 5 Use, 6 Give, 7 Push, 8 Talk, 9 Eat, 10 Look.
Words for the status line: `verb_names` $02FE5E (NUL separated, index = id). Failure messages
when a verb has no script: `verb_fail_msgs` $02FEA0 (e.g. "I can't open \1." where \1 is replaced by
the object name from `object_name_ptr`, "that/him/her" for the second object).

## 5. Hit-testing under the pointer

All of it runs every frame in the main loop (`Reset`+$E94...$13B0) while bit 3 of $FF09DC is set
(otherwise $12CC handles the *walking* case: Shaggy's own position vs the objects).

**Room area (pointer y < $A8).** Center point `cx = (x + 8 + cam_x) >> 3`, `cy = (y - 8 + cam_y) >> 3`
(`cam_x = $FF06CA`, `cam_y = $FF06CE`; the -8 in y is the 16-line room-name band minus the +8 of the
crosshair center). If `y < 8` nothing is hit. Then every object record (`$FF1200`, 26 bytes,
`ram_objects`; count `$FF06F4`) in the current room (`+6 == room`) is tried in order, last hit wins:
* static objects: record `+2` = shape id, looked up in the room data (`header+$24` table of longs
  into `header+$1C`): a rectangle (x, y, w, h) in map cells; hit if `x <= cx < x+w`, `y <= cy < y+h`.
* objects with `+$18` bit 1 (moving sprites: actors): hit if the crosshair rectangle (not just a
  point) overlaps the actor's box: sprite position `$FF04E4+...` minus/plus the half-extent tables
  `$0324C0..$03259A` indexed by sprite type.
* Shaggy / Scooby follower: tested against `ram_shaggy_x_prev` etc. +- tables $032550/$032598
  (x), $0324C2/$03250A (y); gives target 2.
Result: `ram_target_obj = object_index + 3` (or 2), 0 if nothing. In "second object" mode
($FF09DE bit 0) the result goes to `ram_target2_obj` instead (and is cleared if equal to the first).
If the target changed, the default verb is looked up (`ram_default_verb`, repaint label in variant 2) and the
status line is rebuilt (`status_line_update`).

**Bar area (pointer y >= $A8), verb view** (pointer_update, $5E4A at $61F6):
`col = (x - 8) / 40` with x<48 -> 0 and x>=208 -> 5 clamped; `row = y > $C0`; slot
`ram_ui_slot = 1 + col + 5*row` for col 0-4 (col 5 maps to col 4); so slot 1-5 top row,
6-10 bottom row, id = `verb_ids[slot-1]` (table `dat_02FE3A[slot]`). In terms of the crosshair
*center* (x+8): cell k = center in [16+40k, 56+40k). The row split is at y = $C1 (top-left y).
Selection (main loop $1092): only **while B is held** (pad bit 4 = 0) and the slot is not 0:
`ram_verb_cur` = id (previous label repainted variant 0, new one variant 1), `$FF09DE` bit 0
cleared. If B is not held, `ram_ui_slot` is cleared and the verb is left unchanged.

**Bar area, inventory view** (bit 4 set): `t = 1 + min(x / 40, 5)` (+6 on the lower row, y > $C0),
then `ram_ui_slot = t - 1` for t <= 7 and `t - 2` above, giving: upper row cols 0..5 -> 0,1,2,3,4,5;
lower row cols 0..5 -> 6,6,7,8,9,10. The D-pad snap never lets the pointer sit in column 0
(x snaps to 40 minimum), so a mouse should clamp x >= 40 in this view. Special slots:
**5 = page up** (`ram_inv_page` -1 if > 0), **10 = page down** (+1 while `(count-1)>>2 > page`);
both are processed while B is held and the pointer is on them. Item slots: after `slot >= 5: slot -= 1`
the item is list index `4*page + slot - 1` (slots 1-4 = upper row, 6-9 -> 5-8 = lower row);
`ram_target_obj = ram_inv_list[idx] + 3`, or 0 past the end of the list. The inventory slot
screen boxes follow from the cell grid: cell c spans x = 40c..40c+39 (c = 1..4 hold items,
y 168-191 upper row and 192-207 lower row), arrows in c = 5 (x 200+).

Crosshair sprite on the bar: snapped to the cell: sprite at screen x = 40*col (+8 in verb view) + 8,
y = 168 / 192 (cursor $128/$140 minus 128), 32x16, tile base `$FF06E4` (verb view; `ui_tile_base_c`
in sprite_build) or `$FF06E8` (inventory). While L/R are not held the pointer X is also snapped to
the cell center (`x = 40*col + $14 (+8)`), so a mouse should rewrite x every frame.

## 6. What a mouse click must do

(Implemented in #23: [docs/mouse.md](../mouse.md). Two corrections from that work:
* On the bar the game rewrites the crosshair x (cell snap) every frame.
* `ram_target_obj` can lag the crosshair by several frames, because the main loop is
  slower than the VBlank.
Wait until it equals a C copy of the hit test before clicking.)

Everything is state the game reads from RAM each VBlank, so a mouse driver (host side, in
`pad_read` HLE or before each frame) needs:

1. **Enter pointer mode** (first mouse move/click while walking): inject one A or B press edge
   (pad bit 6 / bit 4 low for one frame; previous frame high), or set `$FF09DC` bit 3 and the
   pointer position directly. Entering via the edge also does the required setup
   (`$FF09DE` bit0 = 0, `ram_pointer_dx/dy` = 2, `ram_verb_cur` = 0, positions the pointer at
   Shaggy). Direct set needs the same side effects; prefer the edge.
2. **Position**: write `ram_pointer_x` = mouse_x (game px) - 8 and `ram_pointer_y` = mouse_y - 8 for
   the room area (so that the crosshair *center* is the mouse), clamp x 0..$F7, y 0..$CF. For the bar
   (mouse_y >= 168) write `ram_pointer_y` = mouse_y, `ram_pointer_x` = mouse_x - 8 (hot spot is the
   top-left in the bar, center in the room; the formulas above fold both in). Do this *before*
   pointer_update runs in the frame (i.e. between frames); the D-pad code only adds/subtracts, so
   do not also feed D-pad input.
3. **Verb click**: with the pointer on a verb cell, hold B (pad bit 4 low) for at least 2 frames
   (pointer_update sets `ram_ui_slot` in frame N, the main loop reads it in frame N+1), then release.
   The selection sticks (`ram_verb_cur`).
4. **Object click**: with the pointer over the object (so `ram_target_obj` != 0), press B: an *edge*
   (pad bit 4 high in `ram_pad_last`, low in `ram_pad1`). pointer_update ($6292): `ram_action_verb` =
   `ram_verb_cur`, or `ram_default_verb` if no verb is selected, or nothing happens if both are 0;
   if `ram_target_obj != 0` it sets `$FF09DC` bit 7 and the main loop calls `action_execute`
   ($0019B0) which runs the object's script with that verb, then clears `ram_verb_cur`.
   Clicking empty floor (target 0) with B leaves pointer mode and Shaggy walks (code at $62E8).
5. **Leave pointer mode / cancel the verb**: A press edge ($62E8): clears bit 3, repaints the old
   label, ends "second object" mode.
6. **Inventory**: C press edge toggles view (slide, 16 frames; ignore clicks while
   `ram_bar_slide` != $FF). Page arrows and item clicks are B edges on the slots above
   (page arrows act while B is held: 1 step per press).
7. Two-object verbs (Use / Give): select verb, click first object (sets `$FF09DE` bit 0 and waits),
   click the second object; target2 is `ram_target2_obj`.

A direct-call alternative that avoids pad injection: write `ram_verb_cur`/`ram_target_obj` and then
set `$FF09DC` bit 7 (plus `ram_action_verb`); the main loop will run `action_execute`. This skips
the status-line refresh and label repaint, so call `verb_label_redraw` yourself (set `ram_verb_redraw`,
D0 = variant).

## 7. Ported functions

`port/src/game/verbs.c`: `object_name_ptr` (140/140), `verb_label_redraw` (200/200),
`inventory_build_page` (3/3). All bit-exact on registers, flags, VRAM and RAM under difftest.
Subtle: `movem.w d2-d3` restores sign-extended; `inventory_build_page` leaves D0-D5/A0-A4 in
specific states and the paging is per row.

## 8. Open points / uncertainty

* The **inventory slot index mapping** (rows 1/2 on the right-hand arrows, the exact value of
  `ram_ui_slot` for the two rows) comes from code reading, not from a live test with a populated
  inventory; the Lobby has nothing to pick up on the capture route, so `inventory_build_page`
  only has 3 captured states (all with an empty list).
* Behaviour of B on empty floor (walk-to) and of the second-object flow was read, not exercised.
* Whether page arrows fire on B held or on the edge needs a live check; the code at $1128-$11B0
  tests B level (B not pressed -> jump to $1222), i.e. they act every frame B is held and the
  pointer is on the arrow.
* `hblank_verb_bar` cannot be replaced in C under difftest (it busy-waits on VDP status / HV counter
  to land on line 167). A native renderer does not need it: draw the bar as its own layer.
