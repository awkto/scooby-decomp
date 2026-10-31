# Main loop, interrupts and frame timing (GitLab #12)

The map of how a frame is put together and how the game gets from power-on to gameplay.
C code: `port/src/game/mainloop.c`. Names: `data/symbols.txt` / `data/ram.txt` (block
"#12 main loop"). Tools: `build/port/frameprof` (per-frame profile, see the end),
`tools/py/blocking.py` (which functions wait for an interrupt).

## The short version

- There is **no frame-locked main loop**. Reset falls into an endless loop at `$000D38`
  that runs as fast as the CPU allows. In the Lobby one pass takes 1 to 4 frames, mostly
  spent decompressing actor animation frames (`rle_decompress`) and walking the object
  tables. frameprof on the capture route shows 0 to 3 passes per frame.
- **Time is kept by the VBlank interrupt.** VBlank does the display upload, reads the
  pads and steps every timer and countdown. The main loop waits for it only through
  `wait_vblank` (bit 0 of `ram_vblank_flags`) and through flags and countdowns that VBlank
  code clears or decrements.
- Both interrupt vectors are trampolines through RAM hooks:

  | vector | address | code | hook |
  | --- | --- | --- | --- |
  | VBlank (level 6) | `$0008BC` | `movem.l d0-a6,-(sp)` / `movea.l ($FF0000),a0` / `jsr (a0)` / `movem.l` / `rte` | `ram_vblank_hook` `$FF0000` |
  | HBlank (level 4) | `$0008AA` | same with `$FF0004` | `ram_hblank_hook` `$FF0004` |

  The hook targets are ordinary `rts` functions and are the ones ported to C. The
  trampolines are C blocks since #24 (`port/src/game/waits.c`, docs/native.md): save the
  registers, call the hook at top level, restore, `rte`.
- **Input is handled on the VBlank side.** `vblank_main` → `sub_005E4A` and its callees
  ($5E4A–$7080) move Shaggy with the D-pad (`ram_shaggy_x/y` are written there), move
  the pointer, enter pointer mode on A/B (`$006C70`: set `VBF_POINTER`), slide the verb
  bar between verbs and inventory on C (`$005F20`: 16 frames of ±16 px on
  `ram_hscroll_verb`, then toggle `VBF_INVENTORY`), and run Scooby's follow logic. The
  main loop only *interprets* the result: what is under the pointer or under Shaggy,
  verb clicks, and zone scripts.

## Hooks by phase

| phase | VBlank hook | HBlank hook | set at |
| --- | --- | --- | --- |
| boot, logos | `vblank_main` | `hblank_verb_bar`, then `hblank_nop` from `$000952` | Reset `$0008CE` |
| title screen and menus | `vblank_title_fade` `$00A38C` | `hblank_nop` | `sub_008196` `$008276` |
| mystery intro (map/card) | `vblank_light` `$00A4A4` (old hook saved on the stack) | `hblank_nop`, then `hblank_verb_bar` | `sub_0013BA` |
| gameplay | `vblank_main` `$00A65C` | `hblank_verb_bar` `$01006C` | `sub_008CD6` (menu "start") |

During the logos `vblank_main` is the hook, but `VBF_FRAME_WORK` is clear and `$FF09EB`
bit 1 makes `sub_00FA78` skip the scroll update. Only the VDP register restore and the
common tail run.

## What runs in VBlank

`vblank_main` `$00A65C` (gameplay):

1. VDP reg 2 = `$30` (plane A at `$C000`), reg 4 = `$07` (plane B at `$E000`), reg 5 = `$6C`
   (SAT at `$D800`). This undoes the verb-bar split from the previous frame.
2. `sub_00FA78`: camera → scroll registers (`ram_hscroll_a/b`, vscroll, `$FF0AB3` column
   and row refresh requests). Skipped while `$FF09EB` bit 1 is set.
3. Only when `VBF_FRAME_WORK` (bit 6 of `ram_vblank_flags`) is set. The flag is cleared
   while the work runs (re-entry guard). The main loop also clears it to pause the work
   while it rebuilds the screen (pause menu, room load).
   - `sub_00A7CE`: palette cycling (5 ranges, `$FF0890` table, `$FF0ACD` enable bits)
   - `sub_005C8E`: if `VBF_HUD_DMA` is set and the raster is at lines `$E1`–`$F6`, upload the
     inventory/HUD (`$FF2C00` → VRAM `$B000`, `$FF0328` rows → `$AACC`) and clear the
     flag. `sub_005CF6` in the main loop sets the flag and spins until it is cleared.
   - `sub_00AF24`: actor tile DMA (one per frame, raster window `$E000`–`$F000` of the HV
     counter), `sub_00A984` builds the sprite table, SAT DMA (`ram_sat_buf` → `$D800`),
     decrement the 6 actor timers at `$FF0618`.
   - `move.w #$2300,sr`: HBlank may now interrupt the rest of the handler
   - `sub_00A704`: the two status meters (`$FF0804/$FF0806`, drawn at `$AAF6` / `$AC76`),
     countdowns `$FF0AB1/$FF0AB2`. `sub_005C5A/5C74` in the main loop spin on these.
   - `sub_007C04` if `VBF_OBJ_TIMERS`: object timers (`+$0A` of each object record).
     When a timer expires the object's event script is requested (`+$18` bit 2) or its
     state changes.
   - `sub_005E4A`: scripted Shaggy path playback, then all pad-driven movement, the pointer,
     the verb-bar slide and Scooby. This is the big input handler, see above.
   - set `VBF_FRAME_WORK` again, `move.w #$2600,sr`
4. The common tail, identical to `vblank_light`:
   - `pad_read`
   - Start held → `ram_script_flags2` bit 0 (skips text and cutscenes)
   - `$FF0AC7` countdown
   - **clear `VBF_WAITING`**, which releases `wait_vblank`
   - `ram_08ae` (`FRAME_COUNTDOWN`) countdown. The title and menu code paces itself with
     it: `move.w #4,(ram_08ae)` … `tst.w (ram_08ae) / bpl` = wait 5 frames.

`vblank_title_fade` `$00A38C` (title screen and menus):
- While `$FF09EB` bit 7 is set, it plays the random "lightning" effect: a random delay of
  0–199 frames, then palette 2 flashes, one of 3 picture pieces is drawn into plane B, the
  palette fades back in 4 steps, and the piece is restored.
- Then `title_anim` `$00879E`: title sprite bounce and the plane-A strip scroller.
- Then the common tail.

`vblank_light` `$00A4A4`: the common tail only (mystery intro card).

### HBlank: the verb-bar split

Boot VDP registers (table `$010123`, written by Reset at `$00093C`): reg 0 = `$14` (HInt
on), reg 1 = `$64`, reg 10 = `$A3`, so HBlank fires once per frame at line ~164.
`hblank_verb_bar` then **busy-waits on the VDP status HBlank bit and the HV counter** until
line `$A7` (167), H ≥ `$50`. It then switches the bottom 56 lines to the verb/inventory bar:
- reg `$12` = `$1C`
- VSRAM 0/1 = 0
- reg 2 = `$28` and reg 4 = `$05` (planes A and B both at `$A000`)
- hscroll (`$DC00`) = `ram_hscroll_verb` for both planes
- reg `$12` = `$9200 | ($FF0AD1)`

It also stores the HV counter value in `$FF08B2`. It is a raster effect. It stays in
68k while GPGX draws the picture; with the native renderer (#25, docs/subsystems/render.md) the
split is a band of the frame and the C replacement only records it. A native renderer should do the split at line 167 itself
(the main loop's "pointer y ≥ `$A8`" test matches the same line). frameprof shows the
handler costs ~35 loop iterations per frame.

`hblank_nop` `$01006A`: `rts`.

## Reset: boot to gameplay

Reset `$000200` is one 735-instruction "function" made of the pieces `$200`, `$2FA`, `$690`,
`$8CE`, `$BF4` and `$E94` (see `data/functions.txt`). Frame numbers are from frameprof with
no input. `port/scripts/boot_screens.txt` takes a screenshot of each screen. With `--hle all`
the first four are pixel-identical to `--hle none`. From logo 5 on, the animation phase is
shifted by a frame or so: the C ranges take less emulated time.

| addr | frames | what |
| --- | --- | --- |
| `$000200` | 0 | Standard Sega start-up. TMSS (`'SEGA'` → `$A14000` when the version register says so). VDP registers, Z80 stub and PSG mute from the table at `$00028E`. Clears all 64 KiB of work RAM, VRAM, CRAM and VSRAM. `move.w #$2700,sr`. |
| `$0002FA` | 0 | Region check. The version register's region is mapped through `$000436` and must appear in the header region field `$0001F0`. Otherwise the 1-bpp font at `$0004B8` draws a "wrong region" screen (`sub_000400` prints the strings at `$00044E`/`$00046B`/`$00046E`) and the game hangs at `$0003FE`. |
| `$000690` | 0 | `jsr sub_00069A`: **factory test on controller port 2**. Only if port 2 answers a TH/TR handshake (bits 1–2 = `11` with TH=1 and `00` with TH=0, which a normal pad doesn't) it sets up the VDP, sums the ROM words `$200`..header end and compares with the header checksum `$00018E`. It fills the screen with colour 2 (bad) or 1 (good), reports the result on port 2 data bit 4 and halts. Normally it returns at once. |
| `$0008CE` | 0–3 | Hooks = `vblank_main`/`hblank_verb_bar`, `sound_init` (Z80 driver), music `$2F` (`ram_cur_music`), `game_init` (wait for DMA, `ram_mystery_idx` = 0, RNG seed `$45C3F2D1` plus one step, falls into `game_reset`), clear VRAM/CRAM, VDP regs 0–18 from `$010123`, `hblank_nop`. |
| `$000956` | ~12–400 | Logo 1: **SEGA** (with the shine). 12×4 map `$023226` at `$C594`, tiles LZSS `$023286`. Fade in `$0236A0`, wait 51 frames, 8 steps of palette rotation (CRAM words 3–10), wait 101, fade out. |
| `$000A00` | ~400–700 | Logo 2: **Scooby-Doo Mystery title / legal screen** (Acclaim presents, Hanna-Barbera, Sunsoft notices), H40 (`$8C81`). 40×28 map `$0278E0`, tiles `$023720`, palette `$0281A0`, 300 frames. |
| `$000A88` | ~700–1060 | Logo 3: **Acclaim** (slides in), H32, `$FF09EB` bit 1. Maps `$0219C0` (`$C480`) and `$021B40` (`$E480`), tiles LZSS `$021E40`, palette `$02241A`. Plane B slides in: `ram_hscroll_b` from −256 to +48 by 4 per frame. Wait 200. |
| `$000B48` | ~872–1200 | Logo 4: **Sunsoft**. 26×9 map `$02249A` at `$C406`, tiles LZSS `$02266E`, palette `$0231A6`, 300 frames. |
| `$000BF4` | ~1209–1639 | Logo 5: **illusions** (The Illusions Gaming Company, the developer), animated. Tiles LZSS `$0287C8`, two maps (`$028220` → `$FF4000` as 15 frames of 20×5 words, `$02864C` → `$FF5000`). `sub_000BC8` DMAs frame d3 to `$C410`. 4 loops × 15 frames, 2 frames each. Then the 25×15 map at `$C288`, wait 200, fade out. |
| `$000CB4` | ~1639 | `bsr sub_008196`: **title screen and main menu** (below). It returns only when a game starts. |
| `$000CB8` | | `sound_cmd_stop_all`, `game_reset`, `sub_007F66` (load the mystery chosen in the menu: copy the object table from the mystery header, then `sub_0013BA` = mystery intro card with `vblank_light`, ends on a button or after `$10E` steps), `sub_00801A` (actor init). |
| `$000CD0` | | Set `VBF_SCRIPT`. Run the mystery's start-up scripts (`script_run_frame` twice, from a5/a6 left by `sub_007F66`). Clear `VBF_SCRIPT`. If `$FF09ED` bit 2 (game restored from a password): room = `$FF0A90`, `sub_002138` (load room). Then `sub_00219A` (status line, fade in) and `script_subop_1C` (wait for the actors to settle). |
| `$000D38` | | **Main loop** (below), never left. A new game from the pause menu re-enters the title code. "Quit" jumps to the reset vector (`sub_008F9A`: `movea.l ($0).w,sp` / `jmp ($4)`). |

### Title screen and menus (`sub_008196` `$008196`)

The title subsystem is not ported in #12. Its flow:

1. `palette_read_cram` (fade out). Hooks = `vblank_title_fade` / `hblank_nop`. Load the
   title graphics (RLE tables `$010136`, `$016E6A`…).
2. `sub_008660`: build the **password** for the current game state. 29 bytes of
   `ram_game_state` plus a check byte, encoded 5 bits per character through
   `$00871E`.
3. The bat animation: 28 RLE frames `title_bats_frames`, sound `$0B`, paced by
   `ram_08ae`. Any button skips to `sub_00851E`.
4. `sub_00851E` is the menu, drawn into plane A, and the cursor is `sub_008D6A`.
   - The first time (no game running) there are 3 entries via `$0085DC`:
     `sub_008FB8` (mystery 0, Blake's Hotel), `sub_008FC2` (mystery 1, Ha Ha Carnival)
     and `sound_test` `$00940C`.
   - In the pause menu (`$FF09ED` bit 0) there are 4 or 2 entries via `$0085E8`:
     `sub_008CD6` (continue), `sub_008F8A` (quit to reset), and with `$FF09ED` bit 1
     two more, `sub_0088A2` and `sub_008C40`. These pick a room number and move an object
     to another room: they look like debug entries. Not verified in play.
5. `sub_008CD6` (start or continue) sets `$FF09ED` bit 0 (a game is running, so the next
   menu is the pause menu), takes the room chosen in the menu, writes VDP reg 16 = `$01`
   and reg 5 = `$6C`, restores `vblank_main`/`hblank_verb_bar`, and `rts`. That `rts` returns from `sub_008196` to Reset `$000CB8` (new
   game) or to the main loop `$000D92` (pause).

## The main loop (`$000D38`)

```
loop:  jsr sub_00AFDA                   object event scripts (objects with +$18 bit 2)      [blocks]
       if VBF_SCRIPT:                    a room script is pending
           bsr sub_0019B0 / clr.w SECOND_TARGET / jsr wait_vblank /
           bclr VBF_SCRIPT / jsr wait_vblank / bra loop
       if Start pressed and not $FF09E9 bit 4:                            pause
           bclr VBF_FRAME_WORK / palette_read_cram / sub_008196 (title menu) /
           sub_002196 (reload room) / bclr VBF_SCRIPT / wait_vblank / bra loop
       jsr sub_00A828                   status line ("Walk to", "Use X with Y", room name)
       remember: HOVER_PREV, VBLANK_FLAGS_PREV, VERB_STATE_PREV, ROOM_NAME_PREV, CUR_VERB_PREV, SECOND_PREV
       ambient event: if AMBIENT_TIMER < 0 and Scooby idle ($FF0ABC bit 1) and allowed:
           AMBIENT_TIMER = 100; pick a random entry of the room table (ram_room_ptr_69a)
           bsr sub_0056B0 (spawn at its cell)                                               [blocks]
       if VBF_POINTER (pointer mode):
           pointer y >= 168 -> verb bar: verb mode: B on a slot selects its verb (draw_verb_button)
                                          inventory mode: hover items, slots 5/10 scroll [blocks]
           else find the target under the pointer: Scooby's box, objects in this room
                (hot-spot rectangle, or a box around the actor the object follows)
           -> HOVER_TARGET (or SECOND_TARGET while a two-object verb waits)
           un-highlight the old target's default verb, highlight the new one's
       else (walk mode):
           HOVER_TARGET = object zone under Shaggy's cell
           new zone and its script not run yet: ZONE_TRIGGER = 11, set VBF_SCRIPT,
           bsr sub_0019B0 (zone script), mark it run ($FF0AC9 bit 7), wait_vblank, bra loop  [blocks]
       jsr sub_00B0B8                   actor animation (frame decompression, sounds)     [blocks]
       bra loop
```

Targets (`HOVER_TARGET`, `SECOND_TARGET`):
- 0 = nothing
- 2 = Scooby
- 10 = the default verb (used as a verb id in the highlight code)
- 3 + n = object record n

Object records start at `$FF1200`, are 26 bytes each, and `$FF06F4` holds the number of
records − 1. Built by `sub_007F66` from the mystery header:

| offset | size | meaning |
| --- | --- | --- |
| +$00 | long | (from the header) |
| +$02 | word | hot-spot number, 1-based (0 = none). Rectangle = header[$1C] + header[$24][n−1]: x, y, w, h in 8-px cells |
| +$06 | word | room the object is in; 1 = carried (in the inventory) |
| +$08 | word | (from the header) |
| +$0A | word | timer, counted down by `sub_007C04` in VBlank |
| +$12 | long | only if +$18 bit 1: data of the actor the object follows |
| +$16 | byte | default verb |
| +$18 | byte | flags: bit 0 = timer active?, bit 1 = follows actor (+$19)+2, bit 2 = event script pending (`sub_00AFDA` runs it) |
| +$19 | byte | actor index − 2 (`$FF` = none) |

Actors: 16.16 positions `ACTOR_X` `$FF04DC` + 4n and `ACTOR_Y` `$FF04F4` + 4n, n = 0..5.
Shaggy is 0 and Scooby is 1. `$FF0AB4` bit n = actor n present.

Inventory: `$FF0A1E` holds the carried object numbers as words, terminated by a negative
word. `INV_PAGE` `$FF06EC` is the scroll row (4 items per row; slots 1–4 and 6–9 show
items; 5 and 10 are the arrows).

## Waiting for VBlank: how the port handles it

C code runs in zero emulated time, and interrupts never fire inside it. Any path that
waits for an interrupt hangs if it runs inside C: through `hle_call68k` at run time, and
in difftest, where IRQs are held so the original never returns. The rule (also in
`docs/porting.md`, "Waiting for VBlank"):

- `tools/py/blocking.py NAME` says whether a function can block. 91 functions can,
  including most script opcodes, `palette_fade`, `palette_read_cram`, `sub_00B0B8`,
  `sub_00AFDA`, `sub_0019B0`, `sub_005B92`, `sub_005C5A/74` and `sub_005CF6`.
- The code between blocking calls is ported as `HLE_RANGE`s. A range stops in front of
  the blocking `jsr`/`bsr` (the exit is the address of that instruction), the 68k makes
  the call, and the code after it is the next range. So the main loop is 11 C ranges plus
  the original 68k call instructions:

| range | addr | exits |
| --- | --- | --- |
| `main_loop_pass` | `$000D3E` | `$D4A $D88 $E86 $116C $1174 $11A8 $11B0 $1228 $1392 $13B0` |
| `main_loop_after_spawn` | `$000E8A` | `$116C $1174 $11A8 $11B0 $1228 $1392 $13B0` |
| `main_loop_after_click` | `$00122E` | `$13B0` |
| `main_loop_after_inv_back` / `_fwd` | `$001178` / `$0011B4` | `$1228` |
| `main_loop_after_script` | `$000D4E` | `$D54` |
| `main_loop_script_done` | `$000D5A` | `$D62` |
| `main_loop_after_pause` | `$000D96` | `$D9E` |
| `main_loop_after_zone_script` | `$001396` | `$13A6` |

- The wait primitives (`wait_vblank` `$974C`, `wait_vblanks` `$9742`) and the spin loops
  stay in 68k. In the native port each becomes "yield to the host until the next VBlank":
  the host runs the VBlank hook, then resumes the game code.
- Nested 68k calls from C run with interrupts held and the VDP line base following the
  CPU (#30, see `docs/porting.md`). `port/tools/irqtest.sh` checks that every gameplay
  frame still runs `vblank_main`.
- HV-counter *reads* without a wait (`sub_005C8E`, `sub_00AF24` decide whether there is
  still time for a DMA) are fine in C. In difftest the sandbox V counter is constant. In
  real play the C code reads earlier than the 68k would, which only shifts a DMA by a
  frame.
- `vblank_main` lowers the mask to 3 in the original. The C version never lowers it
  before a nested 68k call (a pending HBlank would run `hblank_verb_bar` inside the nested
  call, and its raster wait would never end). Only the final SR (`$2600`) is reproduced.
  During VBlank there is no HBlank interrupt anyway.

## Status of the subsystem

Every function below is in `port/src/game/mainloop.c` unless the status says 68k.
difftest results are from `port/tools/dt NAME` (capture route), plus extra routes where
noted (`--script port/scripts/lobby.txt --script port/scripts/pointer_play.txt` or
`inventory_play.txt`).

| addr | name | what | status |
| --- | --- | --- | --- |
| `$0008BC` | VBlank | vector trampoline via `$FF0000` | 68k (trampoline) |
| `$0008AA` | HBlank | vector trampoline via `$FF0004` | 68k (trampoline) |
| `$00A65C` | vblank_main | gameplay VBlank | C, 200/200 |
| `$00A4A4` | vblank_light | minimal VBlank, common tail | C, 200/200 |
| `$00A38C` | vblank_title_fade | title VBlank (lightning effect) | C, 200/200 |
| `$00879E` | title_anim | title sprite bounce + strip scroller | C, 200/200 |
| `$01006A` | hblank_nop | `rts` | C, 200/200 |
| `$01006C` | hblank_verb_bar | raster split at line 167 | 68k with the GPGX renderer (raster busy-wait); with `--renderer native` C (group `native`): records the verb-bar band for the native renderer, see render.md (#25) |
| `$00974C` | wait_vblank | set `VBF_WAITING`, spin until VBlank clears it | 68k: the wait primitive |
| `$009742` | wait_vblanks | d0+1 × wait_vblank | 68k: the wait primitive |
| `$000D3E` | main_loop_pass | one main-loop pass | C, 200/200 + 300/300 pointer route + 200/200 inventory route. Exits seen: `$D4A $E86 $1228 $1392 $13B0`. The pause exit `$D88` happens once on the capture route but is not among the sampled states. The inventory-arrow exits `$116C $1174 $11A8 $11B0` are reached by the populated-inventory route of `port/tools/inv_dt.sh` (400/400 there, see verbs.md). |
| `$000E8A` | main_loop_after_spawn | pass after an ambient spawn | C, 9/9 capture route + 75/75 pointer route |
| `$00122E` | main_loop_after_click | verb highlight after a click | C, 200/200 + 200/200 pointer route |
| `$001178` `$0011B4` | main_loop_after_inv_back/fwd | after inventory scroll | C, 6/6 and 4/4 on the populated-inventory route (`port/tools/inv_dt.sh`) |
| `$000D4E` `$000D5A` `$000D96` `$001396` | main_loop_after_script, _script_done, _after_pause, _after_zone_script | 1–2 instruction glue between blocking calls | C, all route calls |
| `$000200` | boot_hw_init (range → `$2FA`) | Sega start-up; skipped on soft reset | C (reset.c), 3/3 |
| `$0002FA` | region_check (range → `$690`/`$334`/`$3FE`) | region check | C, 3/3 (lockout screen itself 68k) |
| `$00069A` | factory_test | port-2 test-fixture detection | C, 3/3 (test screen at `$6F2`: 68k tail jump, unverified path) |
| `$00A248` | game_init | DMA wait, RNG seed + HV-based step | **68k**: HV value reaches the X flag |
| `$00A27A` | game_reset | reset game variables | C, 5/5 |
| `$0008CE` | boot_init (range → `$916`) | hooks, sound init, title music (stops before `jsr game_init`, which waits for the boot VRAM fill DMA) | C |
| `$00091C` | boot_vdp_init (range → `$9AC`) | clear VRAM/CRAM, VDP regs, logo 1 setup | C |
| `$0009B2` `$0009BC` `$0009E0` `$0009FA` | logo1_hold, logo1_shine, logo1_shine_upload, logo1_shine_next | logo 1 hold / palette shine loop | C, 3/3, 3/3, 24/24, 24/24 |
| `$000A0E` `$000A68` | logo2, logo2_hold | logo 2 | C, 3/3 |
| `$000A78` `$000B0E` `$000B18` | logo3, logo3_slide, logo3_slide_step | logo 3 + plane B slide | C, 3/3, 3/3, 200/200 |
| `$000B38` `$000BA6` | logo4, logo4_hold | logo 4 | C, 3/3 |
| `$000BB6` `$000BC8` `$000C4E` `$000C5A` | logo5, logo5_frame, logo5_anim, logo5_anim_step | animated logo 5 | C, 3/3, 183/183, 3/3, 180/180 |
| `$000C9C` | logos_done (→ `$CB4` title_screen) | | C, 3/3 |
| `$000CB8` `$000CD0` `$000CEC` `$000D08` | game_start, game_start_scripts, game_start_script2, game_start_room | game start after the menu | C, 2/2 each (route has no password game: `$D2C` exit unexercised) |
| `$000400` | region_print | lockout screen text | 68k (never runs on a correct region) |
| `$000892` | BusError | bus/address error: endless CRAM colour cycling | 68k (never runs) |

Reset code that stays 68k on purpose: every `jsr`/`bsr` to a blocking function (the
range exits), the `bra` instructions back to the loop head (`$D68`, `$DA4`, `$13AC`,
`$13B6`), `game_init`, the lockout screen (`$334`–`$3FE`), and the test-fixture screen
(`$6F2`–`$888`).

Functions called by the loop and the handlers that belong to other subsystems (called
through `hle_call68k`, or through the original `jsr` when they block):
- objects/scripts: `sub_00AFDA`, `sub_0019B0`, `sub_007C04`
- UI: `sub_00A828` status line, `sub_005BDC` draw_verb_button, `sub_005B92/5C5A/5C74/5CF6` inventory, `sub_00A704` meters, `sub_005C8E`
- actors: `sub_00B0B8`, `sub_00AF24`, `sub_005E4A`
- camera: `sub_00FA78`
- palette: `sub_00A7CE`, `palette_*`
- rooms: `sub_002138`, `sub_00219A`, `sub_002196`, `sub_0056B0`
- title: `sub_008196`

## frameprof

```
build/port/frameprof [--script FILE]... [--watch ADDR[=name]]... [--frames] [--hist FROM TO]
```

It plays the scripts (default `port/scripts/capture.txt`) with pure 68k emulation and
prints segments of frames with the same hooks, with VBlank, HBlank and main-loop pass
counts. `--watch` gives the first and last frame and the hit count of an address.
`--frames` prints one line per frame (hooks, counts, `ram_vblank_flags`). `--hist` prints
the most executed instructions in a frame range. Capture route, summary:

```
from    to      vblank hook        hblank hook        VBlanks loops min/max    waits   HBlanks
12      1660    vblank_main        hblank_nop            1548      0/0          1538      1545   (logos)
1661    2126    vblank_title_fade  hblank_nop             394      0/0           163       395   (title)
2128    2278    vblank_light       hblank_nop              94      0/0            93        95   (mystery card)
2279    3445    vblank_main        hblank_verb_bar       1007      0/2            50      1006   (Lobby)
3446    7293    vblank_title_fade  hblank_nop            3776      0/0          1515      3777   (pause menu)
```
