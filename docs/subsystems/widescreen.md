# Widescreen (GitLab #26): a wider room view, presentation only

Code: `port/src/render/widescreen.c` (rules, camera, map fill, counters, mouse mapping),
`render_view` / `render_line_pixels_view` in `port/src/render/render.c`, the second frame in
`port/src/render/render_emu.c`, margin clicks in `port/src/mouse/mouse.c`.
Tests: `port/tools/wstest.sh` (all 51 rooms), `STORYTEST_WS=1 port/tools/storytest.sh [long]`.

## Using it

```
build/port/scoobyport                                  # played: 16:9 (398 x 224) by default
build/port/scoobyport --options widescreen=21:9        # 523 px; also W:H, a width (256..640), auto, 4:3
build/port/scoobyport --options widescreen=auto        # the window's aspect, follows resizes
F10                                                    # widescreen on / off
--options ws_stats=1  (or SCOOBY_WS_STATS=1)           # counters at exit;  SCOOBY_WS_DEBUG=1: rule changes
```

Needs the native renderer (the default). Scripted / batch runs (`--script`, `--frames`,
`--screenshot`) stay 4:3 unless they pass `--options widescreen=...`, so every existing test, its
screenshots and its mouse coordinates are unchanged. Script commands `fmouse X Y` / `fclick B X Y`
take pixels of the shown (wide) frame, like the window's mouse; `mouse` / `click` stay 4:3 screen pixels.

## Presentation only

The game is not told anything. Nothing writes to the emulated machine: the renderer draws a second,
wider frame from the same live VDP state at the same per-line moments as the 4:3 frame (so mid-frame
changes show as in 4:3), and fills what the planes do not hold from the game's own RAM. The 4:3
frame is still drawn (rendertest compares it with GPGX). Gameplay mode (a camera / activation
window that knows about the margins, as xmen2's R6) is not implemented.

What a 4:3 player cannot see but the wide view shows: more of the room (that is the point), and the
actors standing there. The game draws actors whose hot spot is within x -80..336 of the 4:3 screen
(`actor_draw`, sprite.c) and pauses the animation of actors outside that window (`actor_animate`):
both are game logic and stay as they are. With the 71-px margins of 16:9 (screen x -71..326) every
actor in the view is inside that window, so they are drawn and animate; with 21:9 (x -133..389) an
actor in the outer 53 px on either side is not drawn (the game culled it). Documented, not changed.

## The view

- **Width**: 16:9 at 224 lines = 398 px (71 px each side of 256). Up to 640 (`RENDER_MAX_W`).
- **Lines**: the room band only. A line is a room line if it is 16..(verb-bar band) and the window
  plane does not cover it (`render_line_is_window`: register 18 with the band, or a register-17
  split). The name band (0-15), text boxes (the window grows down over the room) and the verb bar /
  inventory (168-223) are the 4:3 line, centred, with **black sides**. Why not extend them: the
  window plane is 32 cells wide in H32 (nothing to show beyond), and the verb-bar plane `$A000`
  holds the verb page and the inventory page side by side (the bar slides between them by
  h-scroll), so a wider bar would show the other page. Their background is black, so black sides
  are seamless (screenshots).
- **Camera**: the frame's own: plane B's h-scroll is `-camera x` (camera_update), resolved to the
  world value with `ram_camera_x` ($FF06CA); v-scroll is camera y. The view's left edge is
  `rcam = clamp(cam - margin, 0, room width - view width)`; a room narrower than the view is
  centred (bars on both sides); the 4:3 screen is always inside the view. So at a room edge the 4:3
  part of the room band sits off-centre and the extra room is on the other side.
- **Bars**: black where the view passes the room's left/right edge. Room widths (cells; `gfx.py rooms`):
  32 (most rooms: 256 px, so only bars), 40, 44, 48 (bars both sides at 16:9), 52, 58, 60, 62, 64,
  and 87 (w1 r6 The Bumper Cars, 696 px, wider than the 64-cell plane).

## The planes hold only the 4:3 window

`room_draw_view` ($7950) writes the 32 x 19-cell camera window into the 64 x 32-cell planes
(plane col = world col & 63, plane row = (world row + 2) & 31, the 2 rows of the name band), and
the column/row streamers (`$FC3A`/`$FD5E`/`$FE74`/`$FF68`, called by camera_update when the
camera crosses a cell) write one new column / row of the window; `overlay_apply` redraws only the
visible part of an overlay. The rest of the planes is stale (wstest: hundreds of thousands of
margin cells per room differ from the map). A 87-cell room cannot be in a 64-cell plane anyway.

So the view takes:
- the cells the 4:3 picture shows (world columns `cam/8 .. (cam+255)/8`) from **VRAM**, exactly
  as the 4:3 frame (the 4:3 part of every wide line equals the 4:3 line: counter `centre`, 0 over
  all tests);
- every other cell from the game's **working copies of the room's name tables** in RAM: plane B
  `$FF7000`, plane A `$FF8C00`, width x height words (formats.md "Map pack"). They are what the
  streamers draw from and what overlays patch (opened doors etc. appear in the margins too). The
  copies are read, never written. `render_view.names` holds them per frame indexed by view cell
  (not plane cell: a view wider than 504 px spans more than 64 columns).
- **terrain check**: every frame, the 4:3 window of both planes is compared with the map. In
  every room view of all tests it matches exactly; a frame where it does not is shown 4:3
  (rule `planes-not-map`). (The 87x44 room's plane B copy, 7656 bytes, runs 0x1E8 bytes into
  plane A's at `$FF8C00`; the streamers read the same RAM, so the map fill shows exactly what the
  game would stream; the check passes there too.)

## Sprites

The VDP's sprite x is 9 bits (screen x = x - 128). The game writes the 16-bit word; a sprite whose
word is outside 0..511 is out of sight in 4:3 only because of the wrap, and a wider view would show
it 512 px away. Such pieces are hidden in the wide view (`sprites_wrapped`); `sprites_lost` counts
those whose real position would be inside the view (0 at 16:9: the cull window keeps x words in
range). The per-line sprite / pixel limits stay the 4:3 machine's, so the 4:3 part is identical.
None wrapped in any test so far.

## Per-screen rules (`ws_rules`, widescreen.c; first match decides)

| rule | screens | shown |
|---|---|---|
| `not-room` | VBlank hook != `vblank_main` ($A65C): logos, title, menus, password, intro pages, credits | 4:3 centred |
| `no-map` | no mystery header, or a map size that is not a room's | 4:3 |
| `closeup` | `$FF09DE` bit 6: the close-up layout (camera_update puts plane A at Shaggy): close-up pictures (script sub-op 07), and two rooms that use the layout for a mask that follows Shaggy: **The Maze (w0 r8)** (darkness; widening would show the maze unmasked) and the **Bumper Cars (w1 r6)** ride | 4:3 |
| `scroll-mode` | line/cell h-scroll, 2-cell v-scroll, planes not 64 x 32 | 4:3 |
| `plane-split` | planes A and B scrolled differently: the ending credits (plane A scrolls up over the last room: w0 r4, w1 r25) and a few frames of room transitions | 4:3 |
| `camera-outside` | the h-scroll shows plane columns outside the room's camera range: the full-screen picture (sub-op 05, e.g. Shaggy and Scooby in the Hallway) animates by flipping h-scroll between the two halves of the plane | 4:3 |
| `planes-not-map` | the planes' 4:3 window is not the room map: the other frames of full-screen pictures, and the ~69 black frames between a room change and the new room's load (the room number is already the new one, VRAM cleared) | 4:3 |
| `room` | everything else in a room, including dialogue / text boxes, the inventory bar, cut scenes played in the room (intro pans: the camera moves, the view follows) | wide |

The inventory is not a screen of its own in this game: the bar slides to the item page below the
room, which stays visible; the bar is 4:3 like the verb page.

## Mouse

`main.c` maps the window position to a frame pixel as before, then `ws_frame_to_game` to the
game's screen pixel: through the view in the room band of a wide frame (x may be < 0 or > 255), the
centred 4:3 offset elsewhere. In the margins (`mouse.c` `in_margin`):
- a **left click walks toward that world point** (the same planner / auto-walk as a floor click in
  the 4:3 view: the target is a reachable pixel of the room, a state a pad player reaches by walking);
- hovering does not open pointer mode and the right button does nothing: the game's crosshair is
  clamped to the 4:3 screen (`mg_pointer_place`) and its hit test cannot see the margins, so objects
  there are clicked after the camera has brought them in.
The host arrow is drawn at the frame position (`ws_game_to_frame`).

## Tests

**Game state equality** (`STORYTEST_WS=1 port/tools/storytest.sh` and `... long`): each closed-loop
story route runs twice with `--hle all` and the native renderer, `widescreen=4:3` and
`widescreen=16:9`; PASS = identical checkpoints including a sha1 of the whole work RAM at every
checkpoint, and an identical per-frame hash of RAM + VRAM + CRAM + VSRAM + VDP registers + Z80 RAM
(`SCOOBY_STATE_LOG=FILE` in emu.c) at every frame. No presentation-only RAM range has to be
excluded: the wide view keeps all its state on the host.

**All rooms** (`port/tools/wstest.sh`, 4 parallel workers, ~6 min on a VM): every warp script of
`tools/scripts/rooms/` plus a walk left/right with 5 screenshots (`runs/wstest/ROOM/*.png`), once
16:9 and once 4:3; checks the per-frame state hash equality, the counters (lost sprites, unneeded
bars, centre differences: 0) and that the room is shown wide; reports wrapped sprites, bar frames
and the 4:3 frames by rule. Then the margin-click test (`port/scripts/ws/margin.txt`).

Results: see the end of this file.
