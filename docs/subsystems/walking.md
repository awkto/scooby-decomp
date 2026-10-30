# Walking: input to movement, walkable area, depth, camera, room exits (issue #16)

This is the reference for the click-to-walk work (#23). Everything below was read from
`src/scooby.s` and checked in the emulator: the C port in `port/src/game/walk.c` passes difftest,
and the walkable-area decoding was compared with Shaggy's real positions (section 3.4).

Tools:
* `tools/py/walkmap.py`: decodes a room's collision map, prints the room walk data, and renders an
  overlay PNG. Example: `walkmap.py png 16 out.png --points pts.txt`.
* `tools/py/walkexplore.py`: makes harness scripts that walk Shaggy around at random and log his
  position.
* `port/tools/walk_dt.sh`: extra difftest coverage for this subsystem in 11 rooms.
* `port/scripts/walk_pointer.txt`: pointer-mode click test (section 8).

## 1. Functions

| addr | name | what | status |
|---|---|---|---|
| $005E4A | `player_update` | Per-frame Shaggy update, called from `vblank_main` ($00A6B0). Runs Shaggy's scripted path, the verb-bar page slide (C), the pointer cursor (when the menu is open) or D-pad walking with collision, then the auto-walk step. Falls through the four tails below. | **C**, difftest 200/200 + walk_dt |
| $006AE8 | `player_zone_flags` | tail: Shaggy's "behind" bit from zone bit 15 | **C**, 200/200 |
| $006B28 | `camera_follow` | tail: dead-zone camera and column/row stream requests | **C**, 200/200 |
| $006C28 | `verb_menu_open_check` | tail: a new A or B press opens pointer mode at Shaggy | **C**, 200/200 |
| $006D24 | `actors_update` | tail: Scooby (ride-along, path, wander timer, pose, auto-walk), the per-row depth of every actor, and actors 2-5 (step to target or path). Holds the single `rts` of `player_update`. | **C**, 200/200 |
| $0073D6 | `walk_blocked_probe` | Z=0 if `ram_walk_probe` ($FF0642/46) is on a blocked pixel. Bounds come in D2/D3. | **C**, 200/200 |
| $007476 | `walk_blocked_line_probe` | same test for `ram_line_probe` ($FF064A/4E). Only negative coordinates count as out of range; zone mask $7FF. | **C**, 200/200 |
| $00750A | `pad_dir8` | D-pad to 0 DR, 1 UL, 2 UR, 3 DL, 4 L, 5 R, 6 U, 7 D | **C**, 7/7 on the capture route + walk_dt |
| $005002 | `shaggy_walk_to` | script walk-to: d0,d1 = target px, d2 = speed, d7 = animation base. Plans a straight line or a route through the room's two waypoints, then **blocks** (calls `$00B0B8` once per frame) until Shaggy arrives. | 68k, documented (section 7) |
| $005378 | `walk_line_clear` | steps a probe from `ram_line_from` to (d0,d1) in unit steps along the major axis; d3=1 (Z=0) as soon as a blocked pixel is hit | 68k, documented |
| $005364 / $00532A / $00547A / $00548E | Shaggy line test, waypoint reachability, direction setup | | 68k, documented |
| $0056B0 ... $005AE0 | `scooby_walk_to` and helpers (same design, Scooby's variables) | | 68k, documented |
| main loop $0012CC (inside `Reset`) | walk-on trigger test: room exits (section 6) | | 68k (main-loop subsystem) |

Why the walk-to family stays in 68k: `shaggy_walk_to` and `scooby_walk_to` are called from script
opcodes ($0E/$0F) and from the idle wander in the main loop, and they wait for the arrival frame by
frame inside the call. The HLE layer cannot run that as one C function (porting.md: "functions that
wait for VBlank stay in 68k or become state machines"). Their per-frame half (the auto-walk step) is
part of `player_update` and is in C.

**Register contract.** `player_update` and its tails only return to `vblank_main` through the `rts`
at $0073D4. The VBlank vector restores D0-D7/A0-A6 and SR (movem/rte), and `vblank_main` reads no
register after the call. So difftest ignores the final D0-D7/A0-A6 and CCR for these five entries:
`dt_ignores` in `port/tools/difftest.c`, explained in the comment at the top of `walk.c`. RAM, VRAM
and port writes are compared in full.

## 2. Coordinate systems

* **World (room) pixels.** Each actor has an X and a Y, both 16.16 fixed point. The integer pixel
  is the high word.
  * The arrays start at X[6] = `$FF04DC` and Y[6] = `$FF04F4`.
  * Actor 0 is Shaggy (`$FF04DC`/`$FF04F4`), 1 is Scooby (`$FF04E0`/`$FF04F8`), 2-5 are other
    actors (ghosts, NPCs).
  * The point is the actor's **feet**. All collision, depth and trigger tests use this point.
  * Room size in pixels is `ram_map_width*8` x `ram_map_height*8` (cells at `$FF06C6`/`$FF06C8`);
    the Lobby is 512 x 152.
* **Cells.** cell = pixel >> 3. Zone map, room spawn points, waypoints, idle spots and object
  trigger rectangles are all in cells.
* **Camera.** `ram_camera_x` ($FF06CA) and `ram_camera_y` ($FF06CE) hold the world pixel at the
  top-left of the room view. The room view is screen lines 16-167 (256 x 152). The verb bar starts
  at line 168.
  * screen_x = world_x - camera_x
  * screen_y = world_y - camera_y + 16
  * Plane hscroll is -camera_x (`$FF07F8`), written by the VBlank streamer `$00FA78`.
* **Pointer.** `$FF06DC`/`$FF06DE` are screen-like coordinates (section 8 and docs/subsystems/verbs.md).
  * When pointer mode opens, the game sets x = Shaggy_x - camera_x and y = Shaggy_y - camera_y - $2C.
  * The object hit test uses the world cell ((px+8+camera_x)>>3, (py-8+camera_y)>>3).

## 3. The zone map (third map chunk, RAM `$FFE000`)

### 3.1 Format: a collision *name table*

There is one word per 8x8 cell, `width*height` words, row-major. It works like a VDP name table,
except that it points at 4bpp **collision tiles** instead of graphics:

```
zone word z = ZONE[(y>>3)*width + (x>>3)]
  bit 15      "behind": an actor standing here gets its bit in ram_actors_behind ($FF0ABB);
              the sprite code then draws it behind the foreground (plane priority)
  bits 0-14   collision index; 0 = free cell (no test). In ROM only 1..57 occur; bits 11-14 are never set.
COLL_NAMES[$02AF8E + 2*index]   (150 words)
  bits 0-10   collision tile number in COLL_TILES ($02B0BA, 32 bytes each, 20 tiles)
  bit 11      hflip     bit 12  vflip
blocked(x,y) = nibble (x&7, y&7) of that tile (flips applied) != 0
```

`walk_blocked_probe` also returns *blocked* for x<0, y<0, x>width*8-1 and y>height*8-1.
`walk_blocked_line_probe` masks the index with $7FF instead of $7FFF and has no upper bound check.
The data never sets bits 11-14, so the two tests agree inside the room.

So collision is **per pixel**. The map does not paint the floor: it draws a thin **fence**, 1-3
cells wide, around each walkable area. Diagonal walls use the slanted collision tiles. Cells inside
and outside the fence are 0 (free). The walkable area is therefore *the connected component of free
pixels that contains the actor*, not "all free pixels". A pathfinder must flood-fill from Shaggy's
position (section 9).

Statistics (`walkmap.py stats`): 51 rooms. Indices 1-57 are used. Bit 15 occurs only in the
Basement (12 cells) in mystery 0. Rooms without any collision (index max 0): the Mystery Machine,
Outside the Hotel (40x19), the Roller Coaster and the Crane Game. Those are scripted scenes or free
areas.

### 3.2 No exits, hotspots or depth in the zone map

The zone map holds only collision plus the behind bit:
* exits and hotspots are object rectangles (section 6);
* depth comes from a per-row table (section 4).

### 3.3 Room walk data (after the zone-map chunk, `ram_room_depth_rows` $FF0692)

The room loader (`$00758A`) leaves a pointer to the bytes that follow the third LZSS stream:

```
+0              w[height]  depth per cell row; negative ($FFFF) = interpolate (section 4)
+2*height       ($FF0696)  8 x (w cell_x, w cell_y): spawn points; $FF069E = byte offset of the one
                           used when entering (set by the room-change script); Shaggy = (x*8, y*8),
                           Scooby = (x*8, y*8-1)
+2*height+$20   ($FF069A)  up to 8 longs, -1 terminated (count -> $FF06A0): Scooby's idle spots.
                           word 0: bits 0-11 cell x, bits 12-13 arrival facing ($FF0640),
                           bits 14-15 pose animation base index (dat_scooby_pose_anims $000E8C:
                           $0C,$51,$14,$1E); word 1: cell y
+2*height+$40   ($FF069A+$20) two path waypoints (w x, w y) in cells; (0,0) = unused
...             further room data (not decoded here)
```

Lobby: depth rows 0 = 12, 7 = 8, 18 = 0. Spawns (14,13) (36,8) (36,16) (33,6) (57,13) (47,13)
(39,13). Idle spots (39,8) (24,9) (22,12) (12,15) (38,18). No waypoints.

### 3.4 Visual validation (Lobby)

`walkmap.py png 16 runs/walk/lobby_val.png --points runs/walk/lobby_pts.txt` draws the room
background with blocked pixels tinted red. The white dots are 6847 Shaggy positions logged by four
random-walk runs in the emulator (`walkexplore.py port/scripts/lobby.txt SEED 4000`, harness
`peek` every 3 frames).

* The visited area fills the inside of the fenced polygon and stops at the fence on every side.
  This holds for both halves of the Lobby (the fireplace side and the stairs side) and for the dark
  middle strip.
* 5623 of the 5758 positions on free pixels lie in the single 20448-pixel component that a flood
  fill from Shaggy produces. The other 135 are all (55,118), the spawn point used when entering
  from the Hallway, which sits just outside the fence line.
* 1089 positions (16%) are *on* blocked pixels, and every one of them is within 4 px of a free
  pixel. That is the wall-slide (section 5.3): when a straight move is blocked, Shaggy is put on the
  probe's blocked column/row and auto-walks up to 4 px sideways to the free pixel found. No check is
  made during those few frames.

The decoding matches the game. The PNGs are ROM-derived, so they stay in `runs/` and are not
committed.

## 4. Depth (perspective scale) and speed

Each frame `actors_update` computes a depth for every actor whose `ram_actors_depth_on` bit
($FF0AB8) is set, unless `$FF09E9` bit 1 is set:

```
row  = y >> 3
prev = last row < row with table[row'] >= 0  (row 0 is always looked at, even if row == 0),
       value pv, index pr  (pr carries over from the previous actor if none is found!)
next = first row >= that with table >= 0, value nv, index nr (none: nv = 0, nr = height)
depth = nv*16 - ((nv-pv)*16 * (nr*8 + 7 - y)) / ((nr-pr)*8)      (68k divs: overflow keeps the dividend)
```

The result goes to `ram_actor_depth_x[i]` ($FF05A0+2i) and `ram_actor_depth_y[i]` ($FF05AC+2i).
If it changed, bit i of `$FF0ABE` is set so the sprite code rescales. In the Lobby, depth runs from
200 at the top row to 0 at the bottom (`WalkMap.depth()` in walkmap.py mirrors this).

Speeds per frame, as 16.16 values (the 0x155 factor is about 1.33):

| who | x speed | y speed |
|---|---|---|
| Shaggy, D-pad | (256-depth)*$155 = 1.33 px at depth 0 | half of the x speed |
| Shaggy, auto-walk | ((256-depth)*$155>>8) * dir_x, dir in 8.8 | ((256-depth_y)*$AA>>8) * dir_y |
| Scooby, auto-walk | factor $12A | factor $95 |

So everyone slows down towards the back of the room. In the Lobby that goes down to about 0.29
px/frame at the top row.

## 5. Shaggy's movement each frame (`player_update`)

Order of work:
1. Scripted path ($0AC0 bit 0).
2. Verb-bar page slide (C, 16 frames, `ram_hscroll_verb` ±16 per frame).
3. If pointer mode is open (`$FF09DC` bit 3): pointer movement, A/B handling, and nothing else
   moves Shaggy.

Otherwise `player_update` continues with the steps below. Some flags send it straight to the tails
instead:
* `$09E8` bit 2 (vehicle?), `$0ABF` bit 0 and `$09E9` bit 4 are special modes.
* `$FF09DC` bit 7 means a script is running. Then only the auto-walk runs.

### 5.1 D-pad walking

1. `ram_walk_probe` = Shaggy's position. Speed = (256 - depth_x)*$155.
2. The probe moves by the D-pad:
   * x by `speed`, y by `speed/2`;
   * clamped to 0 and to the room size;
   * `$09E9` bit 0 inverts vertical control.
3. **Turning.** Animations 0-3 are walk R/L/U/D, 4-7 stand, $20-$23 "bored".
   * When the held direction is not the current walk animation, `shaggy_turn_anims`
     [$0FC720 + facing*8 + dir*2] supplies a turn animation. Its bit in `ram_shaggy_turning`
     ($FF0ACC) is set and the probe does not move that frame.
   * While the turn animation runs, Shaggy moves at half speed.
   * When the sprite code reports the animation looped (`$0ABD` bit 0), the facing (`$FF05B8`)
     becomes the new direction.
4. **Idle.** After 300 frames without input (`$FF0686`) the bored animation plays, otherwise the
   standing animation.
5. **Collision.** If the probe pixel is free, Shaggy's position becomes the probe (both 16.16).
   Otherwise, see 5.3.

### 5.2 Auto-walk (`ram_actors_moving` bit 0 = `$FF0AB7`)

* Shaggy moves by (dir_x, dir_y) at the speeds above until he passes `ram_actor_target`
  ($FF05DC/$FF05E8).
* Each axis stops on its own, and the position snaps to the target. Exception: x does **not** snap
  when walking left.
* Then the next waypoint is popped from `$FF065A` (count) / `$FF065C`, consumed from the end.
  Each waypoint has target x, target y, dir x, dir y and facing.
* With no waypoints left, the bit is cleared. If a script is running, the standing animation is
  set.
* There are **no collision tests during an auto-walk**. Whoever sets the target must ensure the
  straight line is free.

### 5.3 Wall slide

When the probe is blocked:
* **Diagonal input:** Shaggy stops.
* **Pure left/right:** the probe tries y-1..y-4, then y+1..y+4. At the first free pixel, Shaggy's
  x becomes the probe x and a 1-px auto-walk in y starts (dir ±$100).
* **Pure up/down:** the probe tries x-1..x-3, then x+1..x+3, the same way.
* **No D-pad input** (for example a script pushed Shaggy into a wall): D0 still holds the probe's
  result, and that value picks the axis. This edge case is reproduced in C.

### 5.4 Scripted paths

Per actor: `ram_actor_path_ptr` ($FF05F4+4i) points at word pairs (x,y), read every
`$FF0AAB+i` frames. Commands:
* `$FFFD a`: set animation a.
* `$FFFC/$FFFB d`: set depth d (Shaggy only; `$FFFB` continues with the next entry).
* `$FFFE`: end.
* `$FFFF`: loop, or end if the actor's bit in `$FF0AC3` is set.

### 5.5 Quirk kept for equivalence (actors 2-5)

After a path step for actor i, the 68k leaves A0 pointing into the path table. The rest of that
iteration then uses the path table as if it were the actor's X/Y: the arrival test, the zone test,
and the address of the next actor. The step-to-target code also clamps with a long compare against
the target **word pair** (x_i, x_i+1).

## 6. Camera

`camera_follow` keeps Shaggy inside a dead zone of the 256x152 view:
* horizontal: screen x 76..180;
* vertical: room-view y 51..101.

The camera is clamped to [0, width*8-256] and [0, height*8-152]. When the camera crosses a cell
boundary, the code sets `ram_camera_scroll_req` ($FF0AB3: bit 0 right, 1 left, 2 up, 3 down). The
VBlank streamer `$00FA78` then writes the new column/row and the scroll registers.

Exceptions:
* `$09E8` bit 1 locks the camera.
* Scripts move the camera themselves (script subop $05).

## 7. Room exits and walk-to-object

### Exits

The zone map has no exit cells. Exits are **walk-on rectangles of room objects**, tested by the main
loop at `$0012CC` (in `Reset`) every frame while pointer mode is closed.

For each object record (`ram_objects` $FF1200, $1A bytes, count `$FF06F4`) that passes all of:
* record+6 == the current room;
* record+$18 bit 1 clear (bit 1 marks sprite actors);
* record+2 = id != 0;

the rectangle (x, y, w, h in cells) is read from `header+$1C + long[header+$24 + 4*(id-1)]`. If
Shaggy's cell (x>>3, y>>3) lies in [x, x+w) x [y, y+h):
1. `$FF06AE` = record index + 3.
2. When that differs from the last frame, event $0B ("walk on") goes to the object's script
   (`$FF06BE` = $0B, `$0019B0`).
3. The script then changes the room: script ops $11/$18 write `$FF06AC` and `$FF069E`, which is the
   spawn point in the new room.

Whether a rectangle is an exit depends on its script (#18/#19). Plain hotspots use the same
rectangles for the pointer.

Lobby objects with walk-on rectangles, from a RAM dump (`build/w/objs.py`-style decoding):

| rec | id | rect (cells) | px | observed |
|---|---|---|---|---|
| 43 | 157 | x32 y4 w3 h2 | (256,32)-(280,48) | top door: random walk left to the Hallway (room 9) at x 256-281, y 47-48 |
| 42 | 158 | x36 y18 w10 h1 | (288,144)-(368,152) | bottom edge: to the Cafe (room 13) at x 299-359, y 143-147 |
| 35,36,34,40,41,44 | 160,161,156,159,165,164 | rug, window, stairs, ... | | hotspots |

For mouse click-to-walk, "go through this exit" means: walk to a free pixel inside the exit
rectangle. Once Shaggy's feet are in it, the game does the rest.

### Walk-to-object: `shaggy_walk_to` ($005002), the built-in pathing

Verb scripts (op $0E/$0F) walk Shaggy to an object with `shaggy_walk_to(x, y, speed, anim)`.

**Planning:**
1. If Shaggy is already at (x,y), return.
2. If `$FF2A00` bit 4 is set, or `walk_line_clear(Shaggy -> target)` finds no blocked pixel, walk
   straight there.
3. Otherwise `shaggy_waypoints_reachable` builds a mask of which of the room's two waypoints W0/W1
   (`$FF069A+$20`) are reachable in a straight line:
   * **both:** use the waypoint with the smaller |W|² (absolute coordinates, not the distance to
     Shaggy), and only if the waypoint-to-target line is clear;
   * **one:** go via it, or via W0 then W1 (or W1 then W0) when the direct waypoint-to-target line
     is blocked;
   * **none:** walk straight anyway.
4. The legs are queued in `$FF065A/$065C` (dir and facing per leg from `shaggy_set_dir`). The first
   leg starts at once.

**Moving:** sets `ram_actors_moving` bit 0 and the walk animation, then loops on `$00B0B8` (one
frame) until the auto-walk in `player_update` clears the bit.

**`walk_line_clear` ($005378):**
* It steps a 16.16 probe one pixel at a time along the major axis, with the minor axis as a
  fraction, using `walk_blocked_line_probe`.
* It does not test the end pixel's neighbourhood and gives up on the first blocked pixel.

So the game has a two-waypoint "pathfinder". Many rooms have no waypoints, and there it is only a
straight line. Scooby's version (`scooby_walk_to`, $0056B0) is the same with Scooby's variables. It
is also how Scooby wanders: every 100 frames (`$FF0688`) the main loop picks a random idle spot
(`$000E00`).

## 8. Pointer mode does not walk to the floor

Question from #7: does clicking the floor in pointer mode make Shaggy walk there? **No.** The script
`port/scripts/walk_pointer.txt` checks it in the emulator:
1. From the Lobby, B opens the pointer: `$FF09DC` $44 -> $4C, crosshair at (128,20).
2. DL x30 moves it to (68,80), which is over the Lobby floor.
3. B and then A are pressed. After 240 frames Shaggy is still at (288,64). A closes pointer mode.

The code agrees:
* The pointer branch of the main loop ($000E94-$0012CA) only hit-tests objects/Scooby and the verb
  bar. It sets `$FF06AE`/`$FF06B6` and the selected verb, never a walk target.
* `player_update` does not move Shaggy while bit 3 is set.

Shaggy only walks to things when a verb's script calls `shaggy_walk_to`.

For #23, the game's own `shaggy_walk_to` is not usable as a mouse walk:
* it blocks (script context);
* it plans only via two fixed waypoints;
* it is missing in rooms with no waypoints.

The **auto-walk** state machine it drives *is* reusable, though. It runs every frame inside
`player_update`, is non-blocking and already in C. Section 9 builds on it.

## 9. C API proposal for #23 (click-to-walk)

**Implemented** (#23) in `port/src/game/walkpath.[ch]`, described in [docs/mouse.md](../mouse.md)
section 3. Differences from the proposal below:
* A* runs on full pixels, not a coarsened grid.
* Planning is done fresh per click, with no cache.
* The walk starts without waiting for the turn animation.
* Exits are recognised by "default verb LOOK + rectangle reachable".

Everything needed is in RAM once a room is loaded. All coordinates below are world pixels; convert
a mouse position with section 2.

```c
/* walk.h additions (proposal) */

/* --- queries (pure, read RAM/ROM; mirror walk_blocked_probe exactly) */
bool walk_is_blocked(int x, int y);          /* out of room or collision nibble != 0 */
static inline bool walk_is_walkable(int x, int y) { return !walk_is_blocked(x, y); }
uint16_t walk_zone_word(int x, int y);       /* raw word; bit 15 = behind */
int  walk_depth_at(int y);                   /* section 4 formula (0 = near) */

/* reachable area: flood fill (4-neighbour) of free pixels from Shaggy's feet.  Cache it
 * per room load (512x152 Lobby = 78 KB as bytes, or 1 bit/px); invalidate when
 * $FF06AC/$FF06AA or the zone map changes (some scripts rewrite cells). */
typedef struct walk_area walk_area;
const walk_area *walk_area_get(void);
bool walk_area_contains(const walk_area *a, int x, int y);
/* nearest reachable pixel to a click (BFS outward from the click, or a distance transform);
 * clicks on walls/background snap to the floor edge under the cursor */
bool walk_area_nearest(const walk_area *a, int x, int y, int *ox, int *oy);

/* --- exits / hotspots (from the object table, section 7) */
typedef struct { int x0, y0, x1, y1; int record; int id; } walk_rect;   /* pixels, x1/y1 exclusive */
int  walk_triggers(walk_rect *out, int max); /* walk-on rects of objects in this room */
int  walk_trigger_at(int x, int y);          /* record index or -1 */

/* --- path: A* on a coarsened grid (e.g. 2x2 or 4x4 px cells, a cell is free if all of
 * its pixels are), 8-connected, with octile cost; y cost x2 to match the half vertical
 * speed; then string-pull: drop a waypoint whenever walk_line_is_clear(prev, next) */
bool walk_line_is_clear(int x0, int y0, int x1, int y1); /* same stepping as $005378 */
int  walk_find_path(int sx, int sy, int tx, int ty, walk_point *out, int max);

/* --- drive Shaggy (non-blocking; reuses the per-frame auto-walk of player_update) */
bool walk_start(const walk_point *pts, int n);  /* returns false if a script runs */
void walk_cancel(void);                         /* clear ACTORS_MOVING bit 0, stand */
bool walk_active(void);
```

How to implement `walk_start` with the game's own machinery, without touching the 68k code:
1. Only when `$FF09DC` bit 7 (script) and bit 3 (pointer) are clear, and `$FF0AC0` bit 0
   (scripted path) is clear.
2. Write the legs in reverse order into `SHAGGY_WP_X..` ($FF065C: x[i] at +0, y[i] at +4,
   dir_x[i] at +8, dir_y[i] at +12, facing[i] at +16). The table has room for **2** pending legs.
   For longer paths, keep the rest in C and refill each time `SHAGGY_WP_COUNT` reaches 0, or
   simply set the next target when `ACTORS_MOVING` bit 0 clears.
3. For each leg compute dir as `shaggy_set_dir` ($00548E) does:
   * the major axis gets ±$100;
   * the minor axis gets (|minor| * $100) / |major|, with y doubled for the comparison;
   * both are then multiplied by the speed argument (the scripts use 1);
   * facing = 0 R / 1 L / 2 U / 3 D from the dominant axis.
4. Set `ACTOR_TARGET_X/Y(0)`, `SHAGGY_DIR_X/Y`, `SHAGGY_FACING`, the walk animation
   (`ACTOR_ANIM(0)` = facing, or the turn animation, plus bit 0 of `$FF0AB5`), then set
   `ACTORS_MOVING` bit 0.
5. `player_update` then steps Shaggy every frame at the correct depth-scaled speed. The D-pad is
   ignored while he moves, because the auto-walk branch wins. Camera, depth, the "behind" flag and
   walk-on exits all keep working, since they run off his position.
6. Auto-walk does no collision checks, so every leg must be clear (`walk_line_is_clear`). Leave a
   1-px margin (or erode the area by 1 px) so legs never graze the fence.

Mouse clicks then map as:
* click on the floor: `walk_find_path` to the nearest reachable pixel;
* click inside an exit rectangle: walk to the rectangle's nearest reachable pixel (the game takes
  over on arrival);
* click on an object with a verb: leave it to the verb script (it calls `shaggy_walk_to` itself;
  see verbs.md).

## 10. RAM summary

All names are in `data/ram.txt` and `port/src/game/walk.h`.

| addr | name | notes |
|---|---|---|
| $FF0488+2i | actor animation id | `ram_cur_index`/`ram_484a`/`ram_484c` = actors 0/1/2 (names left to the sprite subsystem) |
| $FF04DC / $FF04F4 +4i | actor X / Y 16.16 | 0 Shaggy, 1 Scooby, 2-5 |
| $FF0514+4i / $FF0528+4i | actors 2-5 step x / y | |
| $FF05A0 / $FF05AC +2i | depth x / y | |
| $FF05B8 / $FF05BA | Shaggy / Scooby facing | 0 R 1 L 2 U 3 D |
| $FF05DC / $FF05E8 +2i | auto-walk target | |
| $FF05F4 / $FF060C / $FF0AA5 / $FF0AAB | path ptr / pos / delay / reload | |
| $FF062A | Shaggy D-pad speed 16.16 | |
| $FF062E / $FF0630 | Shaggy auto-walk dir 8.8 | |
| $FF063A / $FF063C | Scooby dir | |
| $FF0642 / $FF0646 | walk probe 16.16 | |
| $FF064A / $FF064E | line probe 16.16 | |
| $FF0652 / $FF0656 | line start 16.16 | was `ram_view_x0/y0` |
| $FF065A / $FF065C | Shaggy waypoint count / table | |
| $FF0670 / $FF0672 | Scooby waypoint count / table | |
| $FF0686 / $FF0688 | idle / wander timers | |
| $FF0692 / $FF0696 / $FF069A / $FF069E / $FF06A0 | room walk data pointers, spawn offset, idle-spot count | |
| $FF06CA / $FF06CE | camera x / y | |
| $FF0AB3 | scroll request | |
| $FF0AB4..$FF0AC3 | actor bit sets | active, anim-new, moving, depth-on, behind, anim-done, anim-looped, depth-new, on-path, no-zone, path-once |
| $FF0ACC | Shaggy turning | |

## 11. Status and open points

* **Ported to C** (`port/src/game/walk.c`): the 8 entries of section 1.
  * Default capture route: each passes difftest on 200 states. `pad_dir8` passes on all 7 calls
    on that route.
  * `port/tools/walk_dt.sh`: 40 states per function in each of 11 rooms. The rooms are the Lobby,
    w0 rooms 2/4/6/9/15/20, w1 rooms 11/19, and the natural routes w0_r17 and w1_r27. The run
    walks at random with A/B/C taps. Result: 3520/3520 equal.
  * `port/tools/hosttest.sh` passes. With HLE on, the Lobby differs only in the fireplace area
    (x 0-117, y 56-84), which is animation.
* **Play test** (`port/tools/walk_play.sh`): a 2500-segment random walk.
  * With all C replacements, the game walks, scrolls and goes through the Lobby exits into the
    Cafe and the Hallway.
  * Long runs end in different places with `--hle all` and `--hle none`. Enabling only
    `pad_read` (not part of this subsystem) diverges just as much. The cause is timing: C
    functions charge different cycle counts, which shifts the main loop and Scooby's random
    wander. It is not a logic difference; difftest covers the logic.
* **Not ported:** the walk-to family ($005002-$005AE0), which blocks across frames. It needs either
  a state-machine rewrite together with the script interpreter (#19), or to stay in 68k.
* **Not decoded:**
  * the meaning of `$09E8` bits 1-3, `$09E9` bits 0-4/6, `$09EA`, `$09EB` bits 2/6 and `$0ABF`
    (special modes: vehicles, carried actor, inverted controls);
  * the room data after the waypoints.
* An actor whose depth row has no defined row above it uses the previous actor's row index (stale
  D4). In theory equal rows could divide by zero; this never happens in the shipped data.
