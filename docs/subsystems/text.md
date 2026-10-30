# Text and dialogue rendering, font (GitLab #20)

C: `port/src/game/text.c` (text_show, text_wait, status line, say) and `port/src/game/vdp.c` (the glyph writers, ported by #13; names there are authoritative: `draw_status_line` = text_box_print, `draw_text_wrapped` = talk_line_print). Strings listing: `tools/py/textdump.py` -> `build/textdump.txt` (ROM-derived, gitignored).
Everything below was read from `src/scooby.s` and checked with difftest on captured states.

## 1. How text gets on the screen (summary)

* Text is **tiles, never sprites**. A glyph is the name-table word `char + ram_font_base` (`$FF068C`).
  The font is 95 tiles for ASCII `$20..$7E`, loaded into VRAM by `font_tiles_load` `$007A2A` (room.c) after the
  room tiles; `ram_font_base` = (VRAM tile index of the first glyph) - `$20`, bit 15 = priority, bits 13-14 =
  palette line (so the same font can be drawn in a different palette by changing those bits).
* **Two places show text**:
  * the **window plane** (VRAM `$D000`, 32 x 32 cells, shown through VDP window register `$12`): the status line
    at the top ("Take Archway", the room name), every dialogue (`say`) box and the failure messages
    ("I can't open that."). One routine, `draw_status_line` `$00A054`, draws all of it.
  * **plane A** (`$C000`) for title / sound-test / debug-room menus (`draw_text_plane_a` `$009F72` + 3 helpers) and
    the **verb-bar plane** (`$A000`, row `$AA82`+) for the entries of the TALK menu (`draw_text_wrapped` `$009FA2`).
* **A dialogue line is not typed out.** The whole text appears at once, stays for a time proportional to its
  length (`4 * length` frames) and goes away when A or B is pressed (or Start in a skippable cutscene).
  `text_show` `$00A130` = print, wait, clear.
* There is no text box frame or portrait: the box is just the window plane, 1 + n text rows high, in
  front of the room at the top of the screen (`reg $12` = rows; 1 line = 2 rows = 16 px, text at screen
  y 8..15; 2 lines = 3 rows, ...).

## 2. Functions

| address | name (C) | what | status |
| --- | --- | --- | --- |
| `$00A236` | `str_length` (strlen) | D0.l = length of the NUL-terminated string at A0 | C |
| `$009F72` | `draw_text_plane_a` | print A0 at column D1, row D2 of plane A (`$C000`, 32 cells/row); regs preserved | C, port/src/game/vdp.c (#13; goes through the renderer hook) |
| `$009F30` | `draw_text_centered` | same, column = (32 - length) / 2 (D1 ignored) | C, port/src/game/vdp.c (#13; goes through the renderer hook) |
| `$009F58` | `draw_text_plane_a_hi` | `draw_text_plane_a` with the font base's priority bit forced on (menus, in front of sprites) | C, port/src/game/vdp.c (#13; goes through the renderer hook) |
| `$009F46` | `draw_text_labelled` | blank the row (34 spaces `$02FFF4`), then print with `draw_text_priority` | C, port/src/game/vdp.c (#13; goes through the renderer hook) |
| `$009FA2` | `draw_text_wrapped` | one TALK-menu choice: left aligned into plane `$A000` from `$AA82`, 128 bytes per row, wraps like `draw_status_line`; returns D0 = lines, `$FF080A` = lines * 300 | C, port/src/game/vdp.c (#13; goes through the renderer hook) |
| `$00A054` | `draw_status_line` | clear the window plane, print A0 centred + wrapped at 30 chars, set the window height, arm the timer | C, port/src/game/vdp.c (#13; goes through the renderer hook) |
| `$00A172` | `text_clear` | empty `$FF08BC`, `draw_status_line` it (clears the box, height 2) | C |
| `$00A130` | `text_show` (+ `_print`, `_clear`, `_done`) | show a line and wait for it: 4 ranges around the stack push and the wait | C (ranges) |
| `$00A18C` | `text_wait` (+ `_frame`, `_tick`) | the wait: pacing timer, A/B/Start, keeps the actors running | C (ranges; the VBlank/actor/pad spins stay 68k) |
| `$00A828` | `status_line_update` | builds "Verb object [with/to object2]" or the room name for the top line | C |
| `$004D80` | `script_op_say` (+ 6 ranges) | script op `$10`: text box with colour, palette, optional Shaggy pose | C (ranges); the pose waits ($004DDE, $004DEE) and the pose-back part ($004E82..$004EDA) stay 68k: no route plays them, no states |
| `$000400` | `region_print` | lockout screen text (wrong region) | 68k: never runs on a legitimate ROM |
| `$007A2A` | `font_tiles_load` | recolour + write the 95 glyph tiles | C (room.c, #18) |
| `$005B5E` / `$005B56` | `object_name_ptr` / `str_copy` | object id -> name string / strcpy | C (verbs.c / util.c) |
| `$0019B0` .. `$001BA0` | failure text in `action_execute` | `\x01` substitution, see section 7 | C (script.c, #19) |

All keep the exact register and flag contract (difftest compares them), so the 68k callers (title screens,
TALK menu, `action_execute`, main loop, other scripts) are unaffected.

## 3. Strings

Plain ASCII, NUL terminated, no length byte, no escape codes except **`\x01`** (section 7). Where they are:

| place | what |
| --- | --- |
| text pool of a mystery (header `+$20`: `$14199C` Blake's Hotel, `$1AF33C` Ha Ha Carnival) | room names (room table entry `+$00`), object names (object table `+$04` of 8-byte entries), the text of every `say` (op `$10`), `talk_choice` text and reply (op `$02`). Operands are *long offsets from the pool base*. |
| `$02FE5E` `verb_names` | `"Pull "`, `"Take "`, `"Open "`, `"Shut "`, `"Use "`, `"Give "`, `"Push "`, `"Talk to "`, `"Eat "`, `"Look at "` (index = verb id, 0 = empty); note the trailing spaces: the object name is appended directly |
| `$02FE50` / `$02FE57` | `"Shaggy"`, `"Scooby"` (object ids 1 and 2; the pool names start at id 3) |
| `$02FEA0` `verb_fail_msgs` | 11 pointers (verb id - 1) to the default failure texts `$02FECC..$02FFD9` |
| `$02FFDB`, `$02FFE0`, `$02FFE4` | `"that"`, `"him"`, `"her"` (replacement of `\x01`) |
| `$02FFE8`, `$02FFEF` | `" with "`, `" to "` (status line, second object) |
| `$02FFF4` | 34 spaces (blank plane-A line) |
| `$008A9C`, `$008AA2` | `"Limbo"`, `"Player Inventory"` (debug room menu) |
| `$FF08BC` (RAM) | the string being shown: status line / failure text buffer |

`tools/py/textdump.py` lists all 1166 distinct strings with their ROM address and where they are used (say / talk /
names / UI); it walks the script byte code with `scriptdis.py` (759 `say`, 55 `talk_choice` + 52 replies, 232 object names,
38 rooms, 11 failure messages, ...). Output `build/textdump.txt`, never committed. Found as in docs/walkthrough.md
`said()`: every read of a string start in a harness trace is a message shown.

## 4. Font format

* ROM `$030F12..$031AF2` (3040 bytes = `$5F0` words = **95 tiles**), one tile = 8 x 8 px, 4 bits/pixel (32 bytes), char
  `$20` (space) first, `$7E` last. In ROM it is a **1-colour font**: every nibble is `0` (background) or `1` (ink).
* `font_tiles_load(D0 = ink colour, D1 = background colour, D2 = font base)` rewrites each nibble to those palette
  indices on the way to VRAM at `((base & $7FF) + $20) * 32`. The game uses ink 15 / background 1 normally;
  `say` loads it per line with the line's own ink colour (bits 0-3 of the op) and sets the palette-line bits in
  `ram_font_base` (bits 4-5 of the op), then reloads colour 15 afterwards. A reload costs ~1.02 M 68k cycles (about
  8 frames); that is why a speaking line with a new colour hesitates for a moment on real hardware.
* In RAM: `ram_font_base` `$FF068C` = `(first glyph tile - $20) | $8000`. Tile = char + base, so a string is drawn
  by adding the base to every byte. The *blank* window cell is `ram_ui_tile_base_f` `$FF08B8` (a solid-colour-1 tile after
  the UI tiles). The verb labels use a different font (LZSS `$030916`, pre-rendered name tables `$030016`) and are not
  text in this sense.

## 5. Layout, word wrap, window height

`text_box_print(A0 = string)`:

1. Fill the 32 x 32 window plane (`$D000`, 1024 words) with the blank cell.
2. n = length. While more than **30 characters** are left: find the cut by scanning backwards from offset 30
   for a space (`$20`) at the offset, or a `'-'` in the *next* byte (`wrap_find_cut`); the line is the characters before
   the cut, and the cut character starts the next line (so a line after a space break begins with a blank cell, which is
   what the blank is for). Quirk: a string without a space or `-` in its first 31 characters scans below A0; the game's
   data never does.
3. **Centring**: a line of n characters is written at `row_base + $1E - (n & ~1)` bytes, i.e. centred in the 30 usable columns
   (word addresses, so odd lengths round down). Rows are `$40` bytes apart (32 cells), the first row is row 1
   (`$D042` = row 1, column 1).
4. Window height = (rows used + 1): VDP reg `$12` and `ram_verb_bar_window` `$FF0AD1` (the HBlank hook re-applies it). A
   one-line text gives 2, two lines 3, etc. Reg `$12` bit 7 = 0 means "window covers the top N rows".
5. Pacing timer `$FF080A` = **length * 16** (word, *total* length, not per line).

`draw_text_wrapped` does steps 2-3 without centring: column D1, row D0 of the verb-bar plane (`$AA82` + row * 128 +
col * 2), returns the number of lines in D0.

`status_line_update` (called from the main loop and the action code) only rewrites the line when something changed
(room, hovered object, selected verb, second object, "waiting for 2nd target"), see the function comment.

## 6. Pacing and dismissal (`text_wait` `$00A18C`)

```
timer = (length * 16) >> 1                          ; lsr.w $FF080A
wait until neither A nor B is held                   ; (so the press that started the action does not skip it)
loop:
    sub_00B0B8 ; run_object_events                   ; actors keep animating, events keep firing
    if script_flags2.1 (skippable) and script_flags2.0 (Start held): break
    if verb_state.4 (cutscene text) and actors_anim_done.0: sub_0020DA
    wait_vblank
    timer -= pacing_table[ram_text_speed]            ; 10, 5, 2, 1, 0 for offsets 0, 2, 4, 6, 8; fixed at 4 -> 2/frame
    if timer < 0 (borrow): break
    if A pressed: wait until A released; break
    if B pressed: wait until B released; break
```
So a text stays `length * 4` frames (30 characters: 2 s) unless dismissed with A or B (after releasing the button).
The pacing table index `$FF080C` is only ever written once (`game_reset`: 4); there is no option screen for it.
`text_show` brackets this: pointer mode is switched off (`ram_vblank_flags` bit 3) so the verb bar ignores input,
and restored after; `verb_state` bit 1 asks for the status line to be redrawn; the window is set back to height 2.

## 7. Name substitution (`\x01`)

Only the **default failure texts** (`verb_fail_msgs`) contain a placeholder, byte `$01`. `action_execute` (script.c, ranges at
`$001B1E..$001B94`) copies the message into `$FF08BC` and replaces `\x01` by a **pronoun**, not by the object name (the
brief of this task and verbs.md say "object name": that is wrong): `"that"`, `"him"` or `"her"`, chosen by the pronoun
byte of the target object's script header (`ram_obj_script_hdr2` `$FF06B0`, high byte): bit 0 clear = `"that"`; bit 0 set
= a person, bit 1 set = `"her"` else `"him"`. The result is shown with `text_show`, e.g. `"I can't open that."`,
`"I can't talk to him"`. The low byte of that header word is the colour (bits 0-3) / palette (bits 4-5) the object's
TALK replies use. The status line, in contrast, appends the real object names (`object_name_ptr`) after the verb word.

## 8. Script op `$10` `say`

```
10 | ATTR.w | TEXT_OFF.l        8 bytes, execute mode only (scan mode just skips it)
  ATTR bits 0-3   ink colour of the font for this line
       bits 4-5   palette line -> bits 13-14 of ram_font_base
       bit 12     cutscene text: actors keep animating (anim-done bit 0, verb_state bit 4) while the box is up
       bit 13     the speaker is Shaggy: bits 14-15 = facing 0..3; a pose from dat_0FC740[new*2 + old*8]
                  plays first, and the old facing is restored (with the pose back) afterwards
  TEXT_OFF.l      string at header+$20 pool + offset
```
Sequence (`script_op_say*` ranges): push D7; [pose: new facing, play the animation until it started and finished];
`text_clear`; save `ram_font_base`; set palette bits; `font_tiles_load(ink, 1)`; A0 = text; **`text_show`**; verb_state
bit 4 off; restore font base; `font_tiles_load(15, 1)`; [pose back, same waits]; pop D7; A5 += 8. The ranges split at
each stack push/pop and each call that waits for VBlank (`sub_00B0B8`, `text_show`'s wait).

## 9. RAM

| address | name | meaning |
| --- | --- | --- |
| `$FF068C` | `ram_font_base` | tile base added to a character; palette bits 13-14, priority bit 15 |
| `$FF08B8` | `ram_ui_tile_base_f` | name-table word of the empty window cell |
| `$FF08BC` | `ram_08bc` | string buffer: status line / failure message / empty string |
| `$FF080A` | text timer | `length*16` after `draw_status_line` (halved at the start of `text_wait`), `lines*300` after `draw_text_wrapped` |
| `$FF080C` | text speed | byte offset in the pacing table `$00A182` (always 4) |
| `$FF0AD1` | `ram_verb_bar_window` | reg `$12` value as seen by the HBlank hook |
| `$FF09DC` bit 3 | pointer mode | cleared while a text is up |
| `$FF09DE` bit 1 / bit 4 | `ram_verb_state` | redraw status line / cutscene text |
| `$FF0ACA` bits 0, 1 | `ram_script_flags2` | Start held / cutscene skippable |
| `$FF0488`, `$FF0AB5` bit 0 | animation index / request | Shaggy pose request used by `say` |

## 10. Difftest results

See the end of this file (filled in by the last run).
