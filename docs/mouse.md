# Mouse control (issue #23)

`scoobyport` can be played with a mouse alone, from the title screen on. The keyboard and
gamepad work as before, and you can switch between them at any moment. `--no-mouse`
turns the mouse off.

The game itself knows only the pad. The mouse does what a player with a pad would do: it
opens the game's pointer mode, moves the game's crosshair, holds B on a verb and presses
B on an object. To walk Shaggy to the floor, it plans a path and drives the game's own
auto-walk. Background: [walking.md](subsystems/walking.md) and
[verbs.md](subsystems/verbs.md).

## 1. Controls

| Mouse | In a room | Elsewhere |
| --- | --- | --- |
| move | The crosshair follows the mouse, and object names show in the status line as in the original. Pointer mode opens by itself while Shaggy stands still. | |
| left click, floor | Shaggy walks there along a path around obstacles. A click on a wall walks to the nearest floor pixel. A click during a walk changes the target. | |
| left click, exit (door, stairs, rug: any object whose default verb is LOOK that Shaggy can step into) | Shaggy walks into its rectangle. The game's walk-on trigger does the rest, for example the room change. | |
| left click, other object | The selected verb, or the object's default verb (shown in red), is used on it. Shaggy walks over if the script says so. | |
| left click, verb on the bar | Selects the verb (white). | |
| left click, arrow icon | Slides to the inventory. The left arrow slides back. | |
| left click, inventory item | The item is the object of the verb ("Use X with ..." then waits for a second object click). | |
| left click, page arrows | Inventory one row up or down. | |
| left click while text, a choice or a cut scene is up | B (next text / confirm) | Start (title menu, intro pages) |
| right click on an object | LOOK at it: selects LOOK on the bar, then clicks the object. | A |
| right click elsewhere | Leaves pointer mode, which also drops the selected verb. | A |
| middle click | Start (pause menu, skips the intro cut scene) | Start |
| wheel | Inventory page up/down (inventory view) | D-pad up/down (dialogue choices) |

Any pad input (keyboard or gamepad) hands control back to the pad at once. If the mouse
had opened pointer mode only to hover, the first direction press closes it again, so
the D-pad walks as usual. The mouse takes control again when it moves at least 4 game
pixels, or when it is clicked. A bumped mouse therefore does not take over.

The host draws an arrow cursor into the game image, at game resolution, so it scales
with the picture. The OS cursor is hidden over the window. In pointer mode over the room
view the arrow is left out, because the game's own crosshair is centred on the mouse
there.

## 2. Design

```
SDL events (main.c)            --script: mouse X Y / click L|R|M [X Y] / wheel N
        |  window px -> game px (mouse_map_window: integer scale, letterbox)
        v
port/src/mouse/mouse.c         host side, SDL-free: event -> plan of steps, one frame at a
        |                      time; returns the pad buttons to inject; draws the cursor
        v
port/src/game/mousehook.c      game state for the mouse: mode, crosshair RAM, hit test,
port/src/game/walkpath.c       verb-bar geometry / collision, exits, A*, auto-walk driver
        |
        v   RAM writes + pad bits, before each emulated frame (main.c: mouse_frame(pad))
     the game (68k or C replacements: identical behaviour, nothing is patched)
```

`mouse_frame()` runs once per emulated frame, before the frame. It works the same with
`--hle all` and `--hle none`, and mousetest.sh runs every test both ways. It writes only
what the original input path writes:

* **Pad bits** for A, B, C, Start and the D-pad. These go through the normal `pad_read`
  and its edge logic.
* **The crosshair**: `$FF06DC`/`$FF06DE`, written every frame while pointer mode is open.
  The D-pad code only adds to these words, so this is the same input path. The game's
  own hit test (main loop) and bar-cell mapping (`player_update`) then run as usual.
* **Shaggy's auto-walk**: the variables that `shaggy_walk_to` ($005002) writes (target,
  direction, facing, animation, waypoint queue, `ACTORS_MOVING` bit 0). Section 4 lists
  them.

### 2.1 Modes (`mg_mode_get`)

| mode | condition | what a click does |
| --- | --- | --- |
| `MG_NONE` | no room loaded: `ram_vblank_hook` ($FF0000) != `vblank_main` ($A65C), or no mystery header / map | Start (left/middle), A (right) |
| `MG_BUSY` | `$FF09DC` bit 7 (an action or script runs), or a mode in which `player_update` never reaches the pointer-open check (`$FF2A00` bit 1, `$FF09DE` bit 2, Shaggy on a scripted path, `$09E8` bit 2, `$0ABF` bit 0, `$09E9` bits 4/6, the bar sliding) | B (left), A (right), Start (middle), U/D (wheel) |
| `MG_FREE` | in a room, pointer mode closed, nothing scripted | floor: walk at once (no pointer needed); objects and the bar: open pointer mode first |
| `MG_POINTER` | `$FF09DC` bit 3 | the click flow below |

### 2.2 The click flow (plan steps)

A click becomes a short plan. Each step can take several frames and checks the game state
on every frame. If the state is not what the step expects, the plan is dropped (for
example when a script starts).

1. **Open** pointer mode: inject one A press edge. `verb_menu_open_check` then sets bit 3,
   clears the verb and second-object state, and puts the crosshair on Shaggy. Wait until
   bit 3 is set (at most 8 frames).
2. **Settle**: write the crosshair and wait until the game's hit test has seen it. This
   needs at least 3 frames with the same position written. In addition, `$FF06AE` (or
   `$FF06B6` in second-object mode) must equal the same hit test redone in C
   (`mg_hit_test`, a copy of $000E94-$001066). The second condition matters. The main
   loop can be several frames behind the VBlank: with `--hle none` the first version
   clicked before the hit test had run. The wait gives up after 40 frames. The bar needs
   no wait, because `player_update` maps the cell every VBlank.
3. **Decide** from the game's own RAM (`$FF06AE`, `$FF06C0`, `$FF06C4`, `$FF09DE` bit 0):
   * Bar, verb cell: hold B until `$FF06C0` is the verb (at most 30 frames). The main loop
     selects only while B is held and `$FF06EA` is set.
   * Bar, arrow icon: one C press, then wait out the 16-frame slide.
   * Bar, page arrow: hold B until `$FF06EC` changes, so that it pages exactly once.
   * Bar, item: B press edge.
   * Room, object with a verb: B press edge. The game's `$006292` sets `$FF06BE` and bit 7,
     and the main loop calls `action_execute`.
   * Room, exit (default verb LOOK, no verb selected, rectangle reachable), or floor:
     close pointer mode (A edge, wait until bit 3 clears), then walk.
4. **Right click on an object**: hold B on the LOOK cell, move back, settle, then B edge
   (only if an object is still under the crosshair).

`SCOOBY_MOUSE_DEBUG=1` logs every decision, walk plan and walk end to stderr.

### 2.3 Why the crosshair stays

The game's crosshair sprite stays visible and is the cursor in the room view:
* the game centres it on the mouse (hot spot = crosshair centre, checked in a screenshot);
* on the bar it snaps to the cell under the mouse, which is the original highlight.

Hiding it would mean changing `sprite_build` ($00A984), which belongs to the sprite
subsystem. It would also take away the only feedback the game gives about the bar cell.
The host draws its arrow only where the crosshair does not mark the mouse: on the bar,
outside pointer mode, and in menus.

## 3. Click-to-walk (`walkpath.c`)

**Walkable area.** `walk_is_blocked(x, y)` is the per-pixel test of `walk_blocked_probe`
($0073D6): zone word -> collision name -> 4bpp collision tile, and out of range counts as
blocked. The zone map only draws fences (walking.md 3.1). The walkable area is therefore
the 4-connected flood fill of free pixels from Shaggy's feet. If he stands on a fence pixel
(wall slide, some spawn points), the fill starts from the nearest free pixel.

**Goal.**
* Floor click: the reachable pixel nearest to the click.
* Exit: the reachable pixel nearest to the rectangle, preferring pixels inside it and
  closest to the click.
* Floor click within 4 px of a walk-on rectangle: the same as an exit. This is "click
  near the door".

**Path.**
1. A* runs on the reachable pixels eroded by 1 px (`WALK_MARGIN`). Legs then never touch
   the fence: the auto-walk itself does no collision test (walking.md 5.2).
2. Costs follow the game's speeds: x 10, y 20 (the vertical speed is half), diagonal 21.
   The heuristic is `10*max(|dx|, 2|dy|)`, which is admissible and consistent.
3. The path is string-pulled into straight legs. Every pixel along a leg, and both
   neighbours on the minor axis, must be in the eroded area.
4. A last short leg goes from the eroded area to the goal (e.g. into an exit rectangle).
   If erosion disconnects the area, A* falls back to the raw area.

Planning is done fresh for each click (a few full-room passes over at most ~80k pixels),
so no cache has to be kept in sync with room changes or scripts that rewrite the zone map.

**Driving Shaggy** (`walk_start` / `walk_tick`). `walk_start` does what `$005592`
(`shaggy_walk_to` after planning) does, without blocking:
1. Write the first leg to `ACTOR_TARGET_X/Y(0)` ($FF05DC/$FF05E8).
2. Write the direction from `shaggy_set_dir`'s formula (`walk_leg_dir`) to
   $FF062E/$FF0630, and the facing to $FF05B8.
3. Set the turn animation (`$0FC720[facing][dir]`, which equals the walk animation when no
   turn is needed), plus `$FF0618 = -1` and `ACTORS_ANIM_NEW` bit 0.
4. Clear `SHAGGY_TURNING`. A D-pad turn in progress would otherwise take the frame.
5. Set `ACTORS_MOVING` bit 0.

The original waits for the turn animation before it moves. The mouse walk does not: the
turn plays while Shaggy walks. Waiting would need the script-running flag, or the D-pad
idle code would reset the animation.

The next legs go into the game's 2-entry waypoint queue `$FF065A`/`$FF065C`. The game
consumes it from the end, so `walk_tick` (every frame) moves the pending entry up and puts
the next leg behind it. `player_update`'s auto-walk step pops legs on arrival, with the
game's own turn animations, depth-scaled speed, camera, "behind" flag and walk-on
triggers.

A walk ends by itself on arrival. `walk_tick` also drops it in these cases, and clears any
of our entries left in the waypoint queue (a later D-pad wall slide would pop them):
* a script starts (bit 7, for example a walk-on trigger such as an exit) or pointer mode
  opens: Shaggy is stopped, as he would stand still with the D-pad;
* the room changes;
* the D-pad (or its wall slide) takes over.

## 4. RAM written by the mouse

| address | what | writer in the original |
| --- | --- | --- |
| pad port | A/B/C/Start/D-pad bits | the player |
| $FF06DC / $FF06DE | crosshair | `player_update` D-pad code |
| $FF05DC / $FF05E8 | Shaggy's walk target | `shaggy_walk_to`, wall slide |
| $FF062E / $FF0630 | auto-walk direction (8.8) | same |
| $FF05B8 | facing | same |
| $FF0488, $FF0618, $FF0AB5 bit 0 | Shaggy's animation, restart | same |
| $FF065A / $FF065C.. | waypoint count / queue | `shaggy_walk_to` |
| $FF0AB7 bit 0 | auto-walk on | same |
| $FF0ACC | D-pad turn bits (cleared) | `player_update` |

Nothing else is written. Verb selection, object hit tests, label colours, the status line,
inventory paging and actions are all done by the game.

## 5. Findings

* **Exits have default verb LOOK**, like plain hotspots. In the Lobby, the top door/stairs
  (record 43, cells 32,4 3x2) and the bottom edge (record 42) are both 10. In the original
  you walk into them with the D-pad. The mouse rule "LOOK default + reachable rectangle =
  walk into it" covers doors, stairs and floor hotspots (the rug: stepping on it fires its
  walk-on event, as with the D-pad). Objects with another default verb get the verb, for
  example OPEN for the Hallway doors (records 71-76) and the Lobby door next to Shaggy
  (record 41).
* On the bar, the game snaps the crosshair x to the cell centre by itself, so the RAM
  value differs from what was written. Compare what you write, not the RAM.
* The main loop's hit test can lag the VBlank by several frames (more with `--hle none`),
  so `$FF06AE` is not valid 2 frames after a crosshair move. Compare it with the same test
  done in C.
* Title menu and intro pages accept Start. A 2-frame Start press was missed on the title
  menu (the harness route uses 4), so fallback presses last 4 frames. The 7th Start of the harness
  route skips the intro cut scene in room 3, where a B press does not.
* Lobby facts: camera (160,0) at entry, Shaggy (288,64), crosshair hot spot = sprite
  centre. The alcove left of the top door, world (226,50), is the nearest spot not in a
  straight line from Shaggy (the wall between the two alcoves). The planner walks there
  in 2 legs, via (237,56).

## 6. Tests

`port/tools/mousetest.sh` (on a VM, after building `build/port`) runs each test with all C
replacements and with none:

| test | script | checks |
| --- | --- | --- |
| mapping | `port/tools/mousemap_test.c` | window -> game px, 3x and 4x letterbox |
| click-to-walk | `port/scripts/mouse/walk.txt` | two walks (2 legs into the alcove, then out to the bottom) end within 3 px of the click (world = screen + camera at click time) |
| verb + object | `look.txt` | hover opens pointer mode; LOOK cell -> `$FF06C0`=10; click on the window -> bit 7 + `$FF06BE`=10; click closes the text; right click on the rug -> LOOK at record 35 |
| exit | `exit.txt` | click on the top door -> room 9 (Hallway) |
| bar / pad | `bar.txt` | arrow icon -> inventory view, left arrow -> verbs; D-pad closes the hover pointer and walks; a 1-px bump does not take over |
| --no-mouse | `exit.txt` | room stays 16 |
| soak | 60 random left clicks | every walk that ends by itself ends on its goal (within 2 px), none is lost |
| title | `title.txt` | power-on -> Lobby with clicks only; the host cursor is in the window shot |

Not covered by tests (no item to pick up early in the game, so no captured state with an
inventory): clicking inventory items, paging with several rows, and the second-object
flow ("Use X with Y", "Give X to Y"). The code follows verbs.md section 5, which was itself
read from the code. Dialogue choice menus (`$001C0C`) work with the wheel (U/D) and a click
(B), but are not tested. Hovering over a choice does not select it.
