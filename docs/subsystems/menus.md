# Subsystem: title screen, menus, passwords, sound test, DEBUG editors (GitLab #21)

Code: `port/src/game/menus.c` (title, menu builder, cursor loop, mystery sub-menu, quit),
`menu_password.c` (ENTER PASSWORD screen, encoder/checker glue), `password.c` (the codec as plain C),
`menu_debug.c` (hidden DEBUG ROOMS / OBJECTS), `soundtest.c`; shared names in `menus.h`.
Formats of the password itself: `docs/passwords.md`. Test driver: `port/tools/dt-menus` (menu tours in
`port/scripts/menus/`), `port/tools/password_test.sh` (codec vs `tools/py/password.py`).

## Screen flow

```
power-on -> logos -> title_screen $008196
   title_setup (VRAM art, palette, password pre-fill)  -> palette_fade
   bat animation, 28 pictures, 5 frames each (any of Start/A/B/C skips it)
        -> menu_open $00851E  (title menu: PLAY BLAKE'S HOTEL / PLAY HA HA CARNIVAL / SOUND TEST)
                              (pause menu, once a game was started: CONTINUE / QUIT MYSTERY / + DEBUG ROOMS, OBJECTS)
             menu_show -> menu_select (cursor loop)  -> item -> table dispatch (68k: lsl / movea / jmp)
   item 1/2  menu_play_blake/carnival -> mystery_menu $008FCA: START / CONTINUE / CANCEL (menu_choose + menu_select)
        START    -> menu_start_game $008CD6 (restores the gameplay hooks, rts out of title_screen)
        CONTINUE -> ENTER PASSWORD screen ($009008..): key grid, END -> password_check -> menu_start_game
        CANCEL   -> menu_open
   item 3    sound_test $00940C ... C leaves (restarts the music)
pause menu:  CONTINUE -> menu_start_game;  QUIT MYSTERY -> quit_confirm ($008F8A) -> soft_reset ($008F9A)
             DEBUG ROOMS -> debug_rooms $0088A2;  DEBUG OBJECTS -> debug_objects $008C40
```

`menu_start_game` is also the way out of the pause menu: `title_screen` is called once by Reset ($0007xx,
$00079x) and returns (`rts` at `$008D34`) when a game is to be played, so the *whole* menu system runs nested
inside that one call and finishes with an `rts`. Everything below is therefore "code between a `bsr title_screen`
and its `rts`".

## Functions

Status: "C" = replaced and difftested on the menu tours (N/N states = all calls on the tours, or the first 200).
`HLE_RANGE` = a code range that stops in front of a blocking call (docs/porting.md "Waiting for VBlank").
The addresses of the ranges are the instruction after the original's wait.

### Title screen

| Address | C name | What | Status |
|---|---|---|---|
| `$008196` | (68k) `bsr palette_read_cram` | first instruction of `title_screen`, waits for VBlank | 68k (blocking call) |
| `$00819A` | `title_mask_irq` | `move #$2700,sr` (split off so the long set-up below is charged masked, not in 1000-cycle slices) | range |
| `$00819E` | `title_setup` | VRAM clear, tile art (LZSS `$01155A`), 3 RLE pieces, font, plane B, title RLE table -> `$FF3800/$FF5000`, palette, interrupt hooks = `vblank_title_fade` / `hblank_nop`, password pre-fill; ends before `jsr palette_fade` | range |
| `$0083E8` | `title_bats_start` | bat animation set-up (`a6` = frame list `$017064`, `d6` = 27) | range |
| `$00840E`, `$008416` | `title_bats_first`, `title_bats_frame` | one bat picture: RLE unpack, scaled sprite draw through the sprite subsystem (`$E40C`, `$F272`), depth `$FF0010`/`$FF0012` shrinking by `$20`; sound `$0B` on pictures 27 and 13 | range |
| `$0084AE` | `title_bats_wait` | after a VBlank: key pressed -> menu; countdown over -> DMA the picture, `dbra` | range |
| `$00879E` | `title_anim` | (#12) VBlank-side title animation | C (mainloop.c) |
| `$008876` | `title_strip_dma` | DMA of `$FF8C00` to VRAM `$DC00`; **never called** | C, no states possible |

### Menu builder and cursor loop

| Address | C name | What | Status |
|---|---|---|---|
| `$00851E` | `menu_open` | chooses title / pause / debug menu (screen, action table, item count); the 68k then pushes table + count, unpacks the screen and spins on `ram_frame_countdown` | range |
| `$008590` | `menu_show` | after the spin: planes + screen DMA, pause menu shows the password rows; ends before `$0085CC` (68k pops the count, `bsr menu_select`, dispatches through the table) | range |
| `$008D36` | (68k) `menu_choose` | push count, unpack screen, spin; the part after the spin: | |
| `$008D4A` | `menu_choose_show` | planes + screen DMA | range |
| `$008D6A`.. | `menu_select`, `menu_select_frame`, `menu_select_draw`, `menu_select_pad`, `menu_select_tick`, `menu_select_done` | the item cursor loop: up/down move by `$18` px, Start chooses, cursor sprite at VRAM `$FC60` blinks through `$008F14`; also the DEBUG cheat code matcher | ranges |
| `$008F1C`, `$008F40` | `menu_refresh_planes`, `menu_title_sprites` | plane A DMAs; title sprite Y words into the SAT at VRAM `$FC00` | C, exact |
| `$008F8A`, `$008F94`, `$008F9A` | `quit_confirm`, `quit_confirm_done`, `soft_reset_begin` | QUIT MYSTERY confirmation, then the reset (the tail `movea.l (0),sp / jmp (4)` stays 68k: the SP changes) | ranges |
| `$008CDA` | `menu_start_game` | gameplay hooks back, start room = `$FF000A`; `rts` out of `title_screen` | range |
| `$008FB8`, `$008FC2`, `$008FCA`, `$008FEA` | `menu_play_blake`, `menu_play_carnival`, `mystery_menu`, `mystery_menu_done` | the START / CONTINUE / CANCEL sub-menu | ranges |

### Passwords

| Address | C name | What | Status |
|---|---|---|---|
| `$008660` | `password_show` | encode (`$FF06AC`, `$FF2A00..+28`) into the text at `$FF08BC`; uses `password.c` | C |
| `$00870E`, `$008710` | `password_shift_symbol`, `password_shift_bits` | the encoder's bit shifter (`roxl` chains through X) | C, exact incl. X |
| `$00873E`, `$00876C` | `password_scramble`, `password_unscramble` | the xor chains (encode / decode) | C |
| `$009298` | `password_check` | END: text -> symbols -> 256 bits -> `unscramble` -> checksum; bad: sound `$05`, `rts`; good: sound `$59`... | range |
| `$009382`, `$009392` | `password_read_byte`, `password_read_bit` | the bit reader of the checker | C, exact incl. X |
| `$00933E` | `password_accept` | clears `$FF2A00..+$100`, copies the 29 bytes, start room to `$FF0A90`, -> `menu_start_game` | range |
| `$0093A2`, `$0093B6`, `$0093CC`, `$0093DC`, `$009162`, `$00927E` | `password_cursor_left/right`, `password_switch_row`, `password_clear`, `password_key_noop`, `password_cancel` | the icon row handlers | C |
| `$009010`, `$00903A`, `$009052`, `$009160`, `$0091D8` | `password_screen_start/frame/input/icon_done/tick` | the key grid loop | ranges |
| `$0093F8` | `password_unused_start` | `bra menu_start_game`, never referenced | C, no states possible |

### Sound test and DEBUG editors

| Address | C name | What | Status |
|---|---|---|---|
| `$00940C`.. | `sound_test`, `sound_test_show`, `sound_test_frame`, `sound_test_poll`, `sound_test_name`, `sound_test_exit` | sound test | ranges |
| `$0088A2`, `$0088AC`, `$0088B6` | `debug_rooms`, `_pick`, `_done` | DEBUG ROOMS | ranges |
| `$0088D8`.. | `rooms_browser`, `rooms_page`, `rooms_rows`, `rooms_input` | the list with the red bar (rooms) | ranges |
| `$008AB4`.. | `objects_browser`, `objects_page`, `objects_rows`, `objects_input` | same for objects | ranges |
| `$008C40`, `$008C80`, `$008CC0` | `debug_objects`, `debug_objects_move`, `debug_objects_done` | DEBUG OBJECTS | ranges |
| `$0096F4` | `menu_load_screen` (+ `_dma`) | unpack screen a0, wait a frame, DMA to plane A | ranges |

### What stays 68k (and why)

Stack pushes/pops that straddle a wait cannot be in a range (it must leave with the SP it entered with, and
difftest only recognises an exit at the entry SP):
- `$00857A..$00858E` push item table and count, unpack the screen, spin on the frame countdown;
  `$0085CC..$0085DA` pop the count, `bsr menu_select`, then `lsl.w #2,d0 / movea.l (sp)+,a0 / movea.l (a0,d0),a0 /
  jmp (a0)` - the table dispatch.
- `$008D36..$008D44` / `$008D68`: the same around `menu_choose`.
- `$008F9E` `bclr`/`bsr palette_read_cram` and `$008FA6..` `movea.l (0).l,sp / movea.l (4).l,a0 / jmp (a0)`:
  the reset itself (the SP is reloaded).
- `$00915E` `jsr (a0)` of the password icon handlers, `$009292` `tst.l (sp)+` of the cancel icon, `$009328`
  `lea 4(sp),sp` of the accept path.
- `$008C60..$008C74` (push object index and record, the rooms browser, `$008C78..$008C7E` pops) in DEBUG OBJECTS.
- Every wait: `wait_vblank`, `wait_vblanks`, `palette_read_cram`, `palette_fade`, the spins on `ram_frame_countdown`.

## Data

RAM (names in data/ram.txt, `#21` block; they live in the scratch of `ram_sat_buf` that nothing uses while the
menus run: no actors exist and the SAT is not built):

| Address | Name | Meaning |
|---|---|---|
| `$FF0008` | (`MENU_CURSOR_TILE`) | first tile number of the title sprite art (cursor sprite uses `+$68..`) |
| `$FF000A` | `ram_menu_room` | start room: copied from `ram_room_index` at the title; DEBUG ROOMS changes it; `menu_start_game` copies it back |
| `$FF0010` | `ram_menu_pad_prev` | byte: pad 1 of the previous menu frame; as a word: bat scale depth |
| `$FF0012` | `ram_title_bat_depth_b` | second bat group's depth |
| `$FF001E` | `ram_pw_raw` | the 32 raw password bytes `r[0..31]` (`+0` room/chk, `+1` checksum, `+2..` state) |
| `$FF003E` | `ram_pw_sym` | the 5-bit symbols |
| `$FF08BA` | `ram_cheat_progress` | cheat keys matched |
| `$FF08BC` | `ram_08bc` | password text: 2 rows x (5 groups x (5 chars + space) + NUL) = 62 bytes. In play the status line uses it |
| `$FF09ED` | `ram_09ed` | menu flags: bit 0 game started (pause menu), bit 1 cheat entered (DEBUG items), bit 2 started from a password, bit 4 bat animation over |
| `$FF06AA` | `ram_mystery_idx` | 0 Blake's Hotel, 1 Ha Ha Carnival |

ROM: action tables `$0085DC` (title: `menu_play_blake`, `menu_play_carnival`, `sound_test`) and `$0085E8` (pause:
`menu_start_game`, `quit_confirm`, `debug_rooms`, `debug_objects`); the title sprite SAT template `$0085F8`
(13 x 8 bytes = 0x68; the Y words are rewritten each frame by `menu_title_sprites`); cursor blink tile offsets
`$008F14`; the cheat table `$010118`; password alphabet `$00871E`; screens (RLE): title menu `$01896C`, pause
`$018BFA`, pause with DEBUG `$018CBE`, START/CONTINUE/CANCEL `$018A56` (Blake) / `$018B3C` (Carnival), ENTER
PASSWORD `$018E48`, quit confirm `$018F58`, sound test `$018C72`, DEBUG ROOMS `$018DD0`, DEBUG OBJECTS `$018E08`.

## Hidden DEBUG menu

Cheat code on any cursor menu: **A, B, A, C, A, B, Left, Right, A, B, Left**, each held a few frames and released
(`cheat_step`: the pad must equal the next expected key; still holding the previous one is allowed; releasing
everything keeps the progress, any other key resets it). It sets `MF_DEBUG` and plays sound 3; the next
`menu_open` of the pause menu shows ROOMS and OBJECTS. The list browsers ($0088D8/$008AB4) page 16 rows at a time
(A / C page, up/down move the bar, B picks). The record formats are in `menu_debug.c`.

## The password codec (`password.c`)

`password_encode(const password_state *, char out[51])`, `password_decode(const char pw[50], password_state *)`
(false on a bad character, length, or checksum), plus `password_checksum`, the 5-bit `password_pack/unpack`, the
xor-chain `password_raw_from_state` / `password_state_from_raw` and the 2x31 display form
(`password_to_display/_decode_display`). `password_state` = `room` (5 bits, 2..31) + the first 29 bytes of the
story-state block `$FF2A00`. No dependency on the emulator: the save-slot feature (#28) can link `password.c` alone,
and `port/tools/password_test.sh` compares it with `tools/py/password.py` on 5000 random states (+ ~1000 tampered
passwords that must be rejected).

Note: the in-game encoder reads the unused two bits of the last character from stale RAM (`$FF003D`), so a password
shown by the game can differ from `password.py`'s in the last character's low bits; the checker ignores them. The
C `password_show` reads the same byte so that RAM matches the original.

## Quirks found

- The QUIT MYSTERY confirmation is called with `d0` = the pause item's offset (4), so `menu_select` sets up four
  cursor positions; positions below the two printed items count as "yes". Kept as is.
- `ram_menu_pad_prev` (`$FF0010`) is the same byte pair as the bat depth at the title. Both uses are in different
  screens.
- `title_strip_dma` ($008876) and `password_unused_start` ($0093F8) are never called.
- The password text rows live in `$FF08BC`, the same buffer as the status line; the password screens clobber it (the
  status line is rebuilt by `status_line_update` before it is next drawn).

## Testing

`port/tools/dt-menus` plays `port/scripts/menus/tour1..3.txt` (generated by `gen.py`: boot, bats, title menu, cheat
code, sound test, password screen with typing / icons / bad END / CLEAR / cancel, valid password start, pause menu,
quit, Carnival start, DEBUG ROOMS / OBJECTS) with `--hle none` and captures every function's entry states from them
(`runs/states/<ADDR>`), then difftests each one. The exemption table (dead registers, flags) is in `difftest.c`
("#21 menus"); the proof of each entry is in the comment above the function ("live out").
`port/scripts/menus/tour.sh N [hle]` runs one tour and makes a screenshot montage.

## Difftest results

`port/tools/dt-menus` (tours 1-3, one session; "N/N" = all calls on the route when it has fewer than 200). The
registers and flags that are ignored for each range (dead for everything that runs after it) are in `difftest.c`;
the "live out" comment above each C function is the proof. `port/tools/password_test.sh`: 5998 vectors (998 tampered),
0 failures. Not testable: `title_strip_dma` and `password_unused_start` (dead code, no call on any route; both
trivial) and the 68k remainders listed above.

| Function | Address | States equal |
| --- | --- | --- |
| `password_show` | `$008660` | 2250/2250 (poked RAM variants) |
| `password_check` | `$009298` | 600/600 (poked RAM variants) |
| `password_scramble` | `$00873E` | 2250/2250 (poked RAM variants) |
| `password_unscramble` | `$00876C` | 600/600 (poked RAM variants) |
| `password_accept` | `$00933E` | 150/150 (poked RAM variants) |
| `title_mask_irq` | `$00819A` | 10/10 |
| `title_setup` | `$00819E` | 10/10 |
| `title_bats_start` | `$0083E8` | 10/10 |
| `title_bats_first` | `$00840E` | 10/10 |
| `title_bats_frame` | `$008416` | 200/200 |
| `title_bats_wait` | `$0084AE` | 200/200 |
| `menu_open` | `$00851E` | 15/15 |
| `menu_show` | `$008590` | 15/15 |
| `menu_choose_show` | `$008D4A` | 5/5 |
| `menu_select` | `$008D6A` | 20/20 |
| `menu_select_frame` | `$008D8E` | 200/200 |
| `menu_select_draw` | `$008D96` | 200/200 |
| `menu_select_pad` | `$008DA6` | 200/200 |
| `menu_select_tick` | `$008E78` | 200/200 |
| `menu_select_done` | `$008EE6` | 19/19 |
| `quit_confirm` | `$008F8A` | 2/2 |
| `quit_confirm_done` | `$008F94` | 2/2 |
| `soft_reset_begin` | `$008F9A` | 1/1 |
| `menu_start_game` | `$008CDA` | 8/8 |
| `menu_play_blake` | `$008FB8` | 2/2 |
| `menu_play_carnival` | `$008FC2` | 1/1 |
| `mystery_menu` | `$008FCA` | 3/3 |
| `mystery_menu_done` | `$008FEA` | 3/3 |
| `menu_refresh_planes` | `$008F1C` | 200/200 |
| `menu_title_sprites` | `$008F40` | 200/200 |
| `password_shift_bits` | `$008710` | 200/200 |
| `password_shift_symbol` | `$00870E` | 200/200 |
| `password_scramble` | `$00873E` | 15/15 |
| `password_unscramble` | `$00876C` | 4/4 |
| `password_show` | `$008660` | 15/15 |
| `password_read_bit` | `$009392` | 200/200 |
| `password_read_byte` | `$009382` | 128/128 |
| `password_check` | `$009298` | 4/4 |
| `password_accept` | `$00933E` | 1/1 |
| `password_cursor_left` | `$0093A2` | 9/9 |
| `password_cursor_right` | `$0093B6` | 9/9 |
| `password_switch_row` | `$0093CC` | 6/6 |
| `password_clear` | `$0093DC` | 3/3 |
| `password_key_noop` | `$009162` | 3/3 |
| `password_cancel` | `$00927E` | 1/1 |
| `password_screen_start` | `$009010` | 2/2 |
| `password_screen_frame` | `$00903A` | 200/200 |
| `password_screen_input` | `$009052` | 200/200 |
| `password_screen_icon_done` | `$009160` | 33/33 |
| `password_screen_tick` | `$0091D8` | 200/200 |
| `sound_test` | `$00940C` | 1/1 |
| `sound_test_show` | `$00944E` | 1/1 |
| `sound_test_frame` | `$00946C` | 200/200 |
| `sound_test_poll` | `$00947C` | 200/200 |
| `sound_test_name` | `$0096AC` | 33/33 |
| `sound_test_exit` | `$009648` | 1/1 |
| `rooms_browser` | `$0088D8` | 2/2 |
| `rooms_page` | `$0088FC` | 9/9 |
| `rooms_rows` | `$00892C` | 140/140 |
| `rooms_input` | `$008976` | 200/200 |
| `objects_browser` | `$008AB4` | 1/1 |
| `objects_page` | `$008AD4` | 5/5 |
| `objects_rows` | `$008B00` | 80/80 |
| `objects_input` | `$008B1A` | 200/200 |
| `menu_load_screen` | `$0096F4` | 2/2 |
| `menu_load_screen_dma` | `$009710` | 2/2 |
| `debug_rooms` | `$0088A2` | 1/1 |
| `debug_rooms_pick` | `$0088AC` | 1/1 |
| `debug_rooms_done` | `$0088B6` | 1/1 |
| `debug_objects` | `$008C40` | 1/1 |
| `debug_objects_move` | `$008C80` | 1/1 |
| `debug_objects_done` | `$008CC0` | 1/1 |

Gates after the change (all on a VM, `--hle all`, merged with main at `8a58544`): `hosttest.sh` PASS, `irqtest.sh` PASS
(no lost VBlank on the pointer / inventory / mouse routes), `mousetest.sh` all passed (including "title -> Lobby with
clicks" with the C menus), `STORYTEST_HLE=all storytest.sh` PASS on all 6 routes (none == all). The three tours also
play to the end with `--hle all` (`tour.sh -h all 1 2 3`): every screen comes up; pixel-exact equality with `--hle none`
is not expected (a recorded open-loop route drifts as soon as the charged cycles differ by a frame).
