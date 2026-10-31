# High frame rate presentation (GitLab #27): interpolated motion, presentation only

Code: `port/src/render/hifps.c` (records, matching, the in-between renderer, counters, dumps),
the announcement in `sprite_build` (`port/src/game/sprite.c`), `ws_interp_view` in
`port/src/render/widescreen.c`, the present clock in `port/src/main.c`.
Tests: `port/tools/hifpstest.sh`, `STORYTEST_HIFPS=1 port/tools/storytest.sh [long]`.

## Using it

```
build/port/scoobyport                                  # high_fps=auto: the display's refresh if over 60 Hz
build/port/scoobyport --options high_fps=144           # a rate in Hz (61..1000); high_fps=off
F4                                                     # on / off (the option's rate, else the display's)
```

`high_fps` is an entry of the options table (`port/src/options.c`; `high_fps = 144` in the ini).
Needs the native renderer (the default). Scripted / batch runs (`--script`, `--frames`, ...) stay at
60 Hz unless they pass `--options high_fps=...`, so every existing test is unchanged; such runs
render each frame's in-between pictures right away (nothing is presented, see "Tests").

The toggle is **F4**, not F9: F9 is the quick-state load (with F5 = save, documented in README,
dist/README.txt and docs/USER_VERIFIED.md), so the high-frame-rate toggle took the free key.

## Timing (no added latency to whole frames, even spacing)

T = 1 / 59.92 s (the logic rate, unchanged). With rate H, **n = round(H x T)** pictures per logic
frame: 120 and 144 Hz give n = 2 (119.8 pictures a second), 165 and 180 give 3, 240 gives 4.

```
 present clock:  (k-1)T      (k-1)T + T/n   ...   (k-1)T + (n-1)T/n       kT
 shown:          R(k-1)      in-between 1   ...   in-between n-1          R(k)
                   |  pads read, logic k runs, R(k) is drawn (raster mode)
```

- The whole frame R(k) is presented at k x T on the present clock, and the pads of frame k+1 are
  read right after it, as with high_fps off. The n - 1 in-between pictures are drawn from R(k)'s
  state (already there) moved back toward R(k-1), and shown evenly spaced between (k-1)T and kT:
  they only show earlier what R(k) will show.
- Compared with high_fps off, where R(k) goes out as soon as its logic and drawing are done
  (a few ms after (k-1)T), the whole frame waits for its tick: the first change toward R(k) is
  seen T/n after (k-1)T, the full step at kT. That is the price of interpolating (as in xmen2's
  E4); extrapolating would avoid it but guesses.
- **Even spacing**: each picture is prepared (rendered, decorated, uploaded) first, then the host
  waits for its time and only flips. The clock advances by exactly T per logic frame (no
  absolute H-Hz clock, which would beat against 59.92 Hz); with audio on it is nudged by T/32 when
  the audio queue drifts more than a frame from its 50 ms target; more than 4 frames late, it
  restarts.
- **vsync** stays on by default; with vsync a rate above the display's refresh is capped to it
  (each flip waits for a refresh, so more pictures than refreshes would slow the game). Rates that
  are not a multiple of 59.92 Hz cannot be even on that display: 144 Hz gives 2 pictures per frame
  (119.8 a second) on a 144 Hz screen; 120 and 240 Hz screens match n = 2 / 4 up to the 59.92 vs 60
  beat (a doubled picture every few seconds).
- The host's arrow (mouse) is drawn at its current position in every picture
  (`mouse_draw_rgba_at`), so it moves at the display rate; the game still receives the mouse at its
  frame. In pointer mode the game's own crosshair is a game sprite and is not moved.

## What moves

An in-between picture is R(k)'s VDP state (snapshot at line 0 of its display) with:

| what | how | snaps |
|---|---|---|
| **camera** (room band's h-scroll entries, both planes; full-screen v-scroll) | an entry whose step equals the camera's step follows the camera; any other step (credits scroll, close-up plane A) is a straight line | step > 64 px (a cut), room change |
| camera position in between | the integer line from R(k-1)'s camera to R(k)'s; when the camera moved exactly with Shaggy's pixel step (camera_follow's dead zone: camera = his pixel - a constant), **his 16.16 line** minus that constant | |
| **actor sprites** | matched by owner (actor slot) + anchor: the actor's 16.16 world position; drawn at floor(world at f) - camera at f | world step > 48 px (teleport), owner not drawn in R(k-1) |
| not moved | name band, text boxes and the verb bar (window / band lines), UI sprites (crosshair, verb cursor, effect sprite, inventory highlight, the fixed and masking sprites), palettes, tiles, animation frames | |

Sub-pixel positions matter here: Shaggy walks 0.68 px (Lobby, depth-scaled) to 1.33 px (Boardwalk)
a frame, so at 60 Hz his pixel steps (and the camera's, which follows his pixel) come in an
uneven cadence (1-1-0, 1-1-2). Following the 16.16 positions, the steps fall evenly in time: in
the Lobby at 240 Hz the background steps one pixel every 6 pictures exactly (hifpstest's dumps).
Whole pixels only: the in-betweens are drawn at the game's resolution, so motion under 1 px a
frame gets even timing, not sub-pixel placement.

### The announcement (owner + anchor)

`sprite_build` ($00A984, C) calls `hifps_spr_begin(camera x, camera y)`, then for each actor it
draws `hifps_spr_actor(slot, first SAT entry, piece count, world x 16.16, world y 16.16)`, and
`hifps_spr_end(entries)`. Host memory only (the game layer does not read it; difftest is
unaffected). The entries before the first actor (pointer, effect sprite, inventory highlight,
fixed and masking sprites) are UI. The anchor is the actor's point (the hot spot), not the frame's
corner, so a new animation frame (other pieces, other size, flipped) changes in place as at 60 Hz:
no piece-by-piece matching, no ghost limbs.

At line 0 the announcement of the last `sprite_build` run becomes the frame's, **if** the VDP's
sprite table (VRAM x / attribute / y words and the VDP's internal y cache) equals `SAT_BUF` entry
for entry; otherwise (68k `sprite_build` with `--hle -sprite_build`, a room load clearing VRAM, a
state load) the owners are unknown. Without owners the frame is interpolated only if the sprite
table did not change since R(k-1) (boot, title: screen-fixed sprites stay put, as at 60 Hz),
else it is held.

## Frames that are not interpolated

Shown as at 60 Hz: the in-between pictures are R(k-1) itself until R(k) is due (counted by reason
in the `hifps:` report):
- still: nothing moved (the commonest; also sub-pixel motion that crosses no pixel);
- no previous frame / discontinuity (start, state load: `emu_state_load` calls `hifps_reset`);
- not gameplay: the VBlank hook is not `vblank_main` ($A65C): logos, title, menus, password,
  intro pages, credits;
- room changed; VDP layout changed (registers 0-5, 11-13, 16, 17, size, wide / 4:3);
- owners unknown (above);
- **frame mode differs**: before the first in-between of a frame, R(k)'s snapshot is rendered with
  no step (frame mode, `render_frame`'s contract) and compared with the raster frame R(k); if the
  game changed the VDP during the display (beyond the verb-bar band, which the scene carries),
  frame mode cannot reproduce R(k) and the frame is held. The verb-bar band's lines of every
  in-between are copied from the raster frame R(k): the main loop rewrites the bar (verb highlight)
  while the screen is drawn, after the line-0 snapshot (before this, 1-7 frames per story route
  differed in lines 178-190 only); the bar never moves and its masking sprites keep actors off it.
  With that, 0 frames differ in every test.

## Widescreen

A wide frame's in-betweens go through `ws_interp_view`: the view of the last finished wide frame
(same width, same rules decision) placed at the in-between camera, with R(k)'s 4:3 window
from VRAM and every other cell, including a column the moved camera shows just outside that
window, from the room's map in RAM (the streamers' source). Lines are composed as in raster
mode: room band wide with bars, the other lines 4:3 centred with black sides. The frame-mode check
covers the wide frame too (0 differences).

## Presentation only

The in-betweens are rendered from host copies (snapshot, work VRAM / VSRAM / sprite cache);
nothing is written to the emulated machine, and the whole frames are the raster frames. Checked by
`hifpstest.sh` and `STORYTEST_HIFPS=1 storytest.sh`: per-frame machine state hash
(`SCOOBY_STATE_LOG`) and per-frame hash of the shown whole frame (`SCOOBY_FRAME_HASH_LOG`) equal to
the high_fps=off run at every frame.

## Diagnostics

| env | |
|---|---|
| `SCOOBY_HIFPS_STATS=1` | the `hifps:` counters at exit (always on in eager / scripted runs) |
| `SCOOBY_HIFPS_DEBUG=1` / `2` | frames whose owners are unknown / held for frame mode; `2`: every frame's camera steps, scroll modes and moved owners |
| `SCOOBY_HIFPS_DUMP=DIR[,FROM-TO]` | write every picture of frames FROM..TO: `s<k>_00.ppm` = whole frame k, `s<k>_<j>.ppm` = in-between j of n between k and k+1 (sorted = presentation order) |
| `SCOOBY_FRAME_HASH_LOG=FILE` | `frame hash` of each shown whole frame (high_fps on or off) |
| `SCOOBY_PRESENT_LOG=FILE` | played runs: one row per present: us before / after the flip, us due, W / I / H (whole / interpolated / held), frame, j, n |

## Results (2026-10-31)

`port/tools/hifpstest.sh` (Lobby walk 3118 frames, Boardwalk warp + walk 5923 frames; 4:3 and 16:9;
120, 144, 240 Hz):

| | Lobby | Boardwalk |
|---|---|---|
| machine state / whole frames vs high_fps off | equal at every frame, all views and rates | equal |
| actor pieces matched on gameplay frames | 14,094 of 14,124 (99.79 %; 30 = an owner not drawn in the frame before) | 14,262 of 14,298 (99.75 %; 24 new, 12 snapped at a teleport) |
| frames interpolated / still / held | 346 / 2,094 / 678 | 744 / 2,067 / 3,112 (the password screen and warp) |
| frame-mode check | 0 of 346 differ | 0 of 744 |
| render per in-between, 4:3 | p50 0.35 ms, p99 0.6 ms | p50 0.4 ms, p99 0.7 ms |
| render per in-between, 16:9 | p50 0.65 ms, p99 1.2 ms | p50 0.7-1.0 ms, p99 1.2-1.4 ms |

Present clock (Lobby walk played in real time on a VM, no display, no vsync, no audio), spacing of
consecutive presents against T / n:

| rate | n | presents a second | within 1 ms of T/n | sd |
|---|---|---|---|---|
| 120 | 2 | 119.8 | 99.8 % | 0.12 ms |
| 144 | 2 | 119.8 | 100 % | 0.005 ms |
| 240 | 4 | 239.7 | 98.6 % | 0.26 ms |

Whole frames every 16.688 ms (T). The VM has no display, so judder on a real screen (vsync) is
not measured here.

## Known limits

- Whole pixels: no sub-pixel rendering (the in-betweens are drawn at the game's resolution and
  scaled like every frame). Rendering them at the window's scale would give sub-pixel motion;
  not done.
- Animation frames keep the game's rate; an in-between shows R(k)'s animation frame at the
  interpolated position (up to (n-1)/n of a frame early).
- Effect sprite (`$09E8` bit 4), the game's pointer-mode crosshair and other UI sprites are not
  interpolated.
- 144 Hz and other rates that are not a multiple of 59.92 Hz present n = round(H x T) pictures per
  frame (144 -> 119.8 a second), which a 144 Hz vsynced display cannot show evenly.
- The 68k `sprite_build` (HLE off for it) announces nothing: such frames are held when sprites
  change.
