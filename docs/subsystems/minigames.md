# Script sub-ops that need a late-game state: credits, fullscreen picture, Crane Game, Bumper Car duel (GitLab #19, third pass)

C: `port/src/game/minigames.c` (HLE_RANGE chains). States: `tools/scripts/story/w0_full.txt`
(Blake's Hotel, power-on to the credits; credits and picture) and `w1_part.txt` (Carnival:
Crane Game, Bumper Car duel). Captured once with `difftest capture-multi`
(`tools/py/script_dt_minigames.sh FUNC...`, states in `runs/states/<ADDR>_<route>`).
All four sub-ops wait for VBlank in loops, so each is a chain of ranges that stop in front of
the 68k `bsr wait_vblank` / `bsr actor_animate` / `bsr room_fade_in` and start again behind it.

## Sub-op $1E ending_credits ($0041B6)

1. `wait_vblanks(30)`, set `$FF09EB` bit 1.
2. Unpack two LZSS streams with `lzss_decompress` into `$FF7000`: the credits tiles `$0296FA`
   (DMA'd to VRAM at tile base A `$FF068A` * 32) and the tile map `$029C72` (every word += tile
   base A). The map is a tall picture, 32 cells wide, one row = 32 words.
3. Loop (one pass per frame: `actor_animate`, `wait_vblank`): the vertical scroll `$FF07FC` (D3)
   goes up one pixel every second frame (D5 toggles); every 8 pixels the next map row (32
   words) is copied with `vdp_copy_to_vram` to plane A row `(D2 & 31)` (VRAM `$C000 + 128*row`,
   D2 starts at 28); the loop ends when the read pointer A0 reaches the end of the map (A3).
4. Hold 300 frames (`$FF08AE`), clear `$FF09DC` bit 6, `palette_read_cram` (fade out),
   `bra soft_reset`: the game restarts at the Sega logo (room word `$FF06AC` = 0).

Ranges: `$41B6` (wait), `$41BE` (setup, ~3.17 M cycles of LZSS), `$422A` (per frame step, avg
182 cycles), `$425C` (hold). The tail `bra soft_reset` stays one 68k instruction.

## Sub-op $05 / $0C fullscreen_picture ($0045A4)

`$05` shows a full-screen picture once (`game_state` `$FF2A00` bit 1 = shown; if it is set the
op returns at once). `$0C` ("bar_hide_once") sets that bit and only runs `bar_hide` (`$0045A4`
tests `2(a5) == $0C`). The 5 calls in w0_full are all `$05` of the unmasking cutscene scenes.

Sequence: wait for the actor tile DMA (`$FF0AB6` = 0, 68k spin), clear `$FF09DC` bit 6, fade
to black, save VRAM `$0000-$43FF` (8704 words, by data-port reads with the mask at 7) at
`$FF2C00`, window register `$12` = 0 (verb bar off), tiles `$02B33A` -> VRAM 0, plane A map
`$02EF7A` (1216 words at `$C080`), plane B rows `$02F97A` (19 times 32 words, 64 bytes
apart), palette `$02F8FA` (64 words) to the text buffer `$FF08BC`. Viewer, at most 201
frames, ended by A or B: every 4th frame `bchg #0,($FF06CA)` (the **high** byte of camera X:
the scroll jumps 256 px, so the picture flips between two animation frames side by side);
every frame the palette buffer goes to CRAM (`vdp_dma_cram`) and the 8 colours at `$FF090C`
rotate by one (colour cycling). Afterwards: black palette, VRAM and camera restored, window
register 2, `room_draw_view`, SR = `$2000`, registers popped, status line redrawn, 5 frames,
the room palette DMA'd back, then `bar_hide` (68k `bsr $482E`).
Stack: 60 bytes (movem d0-a6) + 4 (camera Y/X) + 2 (frame counter) while it runs.
Ranges `$45A4 $45D2 $45E4 $46F8 $4716 $4770 $4782 $479A`; the loop's first wait is
`$46F4`, exits `$46F4` (SP-6 / 0) and `$4716` (SP-4 / +2).

## Sub-op $27 minigame_throw: the Crane Game claw ($003F58)

Game Tent, Crane Game (Tokens USE Crane Game). Actor 2 is the claw (X `$FF04E8`, copy at
`$FF04EC`; Y `$FF0500`); the prize pistol is on a belt, a scripted path whose position is
`$FF0610` (wraps at `$140`, about 1.66 per frame; reset to 0 when path bit 2 of `$FF0AC0`
is clear). `ram_pad1` is set to `$FF` at the start. Each frame (paced by spinning on
`$FF08AE`, zeroed each time and decremented by VBlank): Left/Right move the claw -/+2 px
within X 100..148. A or B drops it: Y += 2 per frame (one `wait_vblank` each) up to 74. The
catch test uses `$FF0610`: claw X = 148: `$5C..$78`; any other X: `$F8..$114`. Catch: sound
`$63`, claw animation 28 (slot 3, `$FF048E`), result 1 in `$FF120C` (object 0 state word);
miss: animation 27, result 0. Wait for the animation (`$FF0ABC` bit 3), sound `$62`, freeze
slot 2 (the prize) on a catch, then the claw rises 1 px per frame to Y 0, `palette_read_cram`,
set `$FF09DC` bit 6, unfreeze. The loop at `$3FE2` (move while a direction is held after the
drop) is dead code: its branch always has Z set. Ranges `$3F58 $3F86 $3FDC $4018 $408A $40B2
$40CE`.

## Sub-op $32 minigame_tug: the Bumper Car duel ($003142)

Bumper Cars (room 7): Shaggy's car (actor 0, pad) against the Clown's (actor 2, computer).
Headings 0..7 clockwise from north are the actors' animation numbers (`$FF0488`, `+4`).
Per-frame state: see the table at the top of the duel in `minigames.c` and `data/ram.txt`
(`ram_duel_*` at `$FF0A84-$FF0AA4`, `$FF05B8/$FF05BC` push headings, `$FF09EA/$FF09EC`
flags). Rules:

* Gas: A or B accelerates Shaggy (speed `$FF0A90` 0..7; timer `$FF0AA0` = speed+8 frames
  between steps), otherwise he slows (timer = 7-speed). The Clown always accelerates.
* Steering: Left/Right change the heading by one (only when the last turn animation is done,
  `$FF0ABC` bit 0 / 2), each turn costs one speed point. The Clown aims at the heading opposite
  to the bearing `duel_bearing()` (`$3A7A`: octant from the signs and ratio of dx/dy), at most
  every 20 frames, turning the shorter way.
* Movement: position += `vec[heading][speed]` + `vec[push][recoil]` (`$003D96`: per heading 8
  X words then 8 Y words, 32 bytes; heading 0 = north, y down). The new point is tested with
  the zone probe (`walk_blocked_line_probe`): blocked = wall hit = sound `$60`, pending damage
  taken, recoil = old speed, speed 0, random spin flag (the `$FF0428` generator stepped as
  `rng = swap(rol(~rng, 1))`, carry picks bit 1 or 2 / 3 or 4 of `$FF09EA`), and the car goes
  back to last frame's position.
* Bump: hit boxes (`$0F88DC` / `$0FA77A` + frame offsets) overlap: sound `$72`; each car's
  damage to the other is its speed, halved unless it faces the other, 0 if the victim faces it
  (bumper to bumper), halved if the victim faces away; sound `$25`/`$28` says who lost more;
  both are pushed back (recoil = speed, at least 4); both undo the move.
* Hit points `$FF0A98`/`$FF0A9A` start at 63 and are drawn as 64-pixel meters (tile bytes
  built in `ram_text_buf` and `$FF0328`, uploaded to VRAM `$A300`/`$A400` every frame).
  The first below 0 explodes (animation 8) and ends the game: `$FF120C` = 1 if the Clown
  lost, 0 if Shaggy did; `fade_out`, sounds `$7B` off / `$71` room music, Shaggy's animation
  set back to `$032700`, facing restored.
* Start pauses (`sound_cmd_pause`, jingle `$59`) until Start again; HInt counter reg 10 is
  set to `$94` during the duel.
* `$34D8` (decrease speed) is dead code.

Ranges: `$3142` setup (HUD tiles and maps, 42 K cycles), `$331E` intro done, `$334E/$3360`
jingles, `$337A` death test, `$338E` pause test, `$33B6/$33C4` pause waits, `$33D8` frame
(27 K cycles, charged whole), `$38E0` explosion wait, `$38EC` finish.

## Difftest

`tools/py/script_dt_minigames.sh` (VM). Setup and frame have dead registers at their exits
(the loop reloads from RAM): run with `DTOPTS="--ignore-regs d0,d1,d2,d3,d4,d5,d6,d7,a0,a1,a2,a3,a4
--ignore-flags"`; A5/A6/SP and all memory, VDP and sound writes are compared. See
`docs/subsystems/script.md` for the numbers.
