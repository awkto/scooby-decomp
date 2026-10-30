# Story walkthrough routes (issue #29)

Status: **Blake's Hotel is solved end to end** (power-on -> credits, twelve verified segments, `hotel_1`..`hotel_12` in
`tools/py/story_full.py`, route `w0_full`). **Ha Ha Carnival is solved up to the Dart and the Ball** (five segments, route `w1_part`,
including the Crane Game and the Bumper Car duel minigames); the remaining chain to the ending (roller-coaster Paint Brush, painted Sign,
Application + Rubber Stamp, Clown in the Back Lot) is mapped from the scripts below but not yet driven. Everything is closed-loop: each
input is decided from RAM read back from the running game (hit-test target, room, inventory, story bytes), never from frame counts, so
the same driver runs on the harness and on the native port (`port/tools/storytest.sh`, `--hle none` vs `--hle all`). The flat harness
scripts the driver records replay from power-on with the plain batch harness and end in the state in their header (`verify.py`).

## Tools

| file | what |
|---|---|
| `tools/py/session.py` | `serve DIR` keeps `build/harness` alive reading its script from a FIFO behind a unix socket; a driver can step, peek RAM, decide, continue |
| `tools/py/story.py` | `Game` driver: boot, `walk_to`, pointer mode (`select_verb`, `verb_on`, `act VERB TID`, `use ITEM TID`), `scan_objects`, `said()` (see below), `explore_room`, strict mode |
| `tools/py/story_routes.py` | the route definitions + generator (`bash`-driven: learn pass with scans, then a strict pass that writes `tools/scripts/story/NAME.txt`) |
| `tools/py/crawl.py` | tries 8 verbs on every object of every room from the password-warp state, logs text / story-byte / inventory changes |
| `tools/py/collect_points.py`, `tools/scripts/story/points_w{0,1}.json` | cache of crosshair hit points per object (world coordinates) so strict routes need no scans |
| `tools/py/objects.py`, `scriptdump.py`, `combos.py` | static dump of the object table / scripts / the (verb, second object) handlers |
| `tools/py/verify.py` | replay a flat route in the batch harness and print room / inventory / story bytes |
| `tools/py/story_full.py` | the long walkthroughs: `hotel_1..12`, `carnival_1..5`, minigame drivers `crane_game`, `bumper_tug` |
| `tools/py/walkplan.py` | walk planner over the game's collision map (distance field, D-pad geometry, exit avoidance) |
| `tools/py/hotspots.py` | static hotspot rectangle of every object (ROM object record +2 -> overlay table) |
| `tools/py/blk.py`, `objrooms.py`, `storybytes.py` | puzzle-logic readers: disassembled script of an object without animation noise; initial room of every object; the 29-byte story table |
| `tools/dev/*.sh`, `run.py` | the exploration loop on a VM: persistent session (`start.sh`), detached snippets (`bg.sh`, `bgr.sh`), route generator (`genroute.sh`), port gate (`st.sh`) |

Key tricks:

* **What the game said** is read from the harness `trace`: every ROM read of a text-pool string start is a message
  shown. Strings are plain ASCII, NUL terminated, in the pool at header `+$20` (`$14199C` Blake, `$1AF33C` Carnival; the
  generic refusals `"I can't open ."` are at `$2FECC`). `said()` returns them in order. No OCR needed.
* **Routes are flat and deterministic.** Savestates are *not* bit-exact (a route recorded across `load`s replayed to a
  different state), so exploration may use savestates but the route generator runs in *strict* mode: no loads, no scans,
  object points from the JSON cache (`world = crosshair + camera`), everything it presses is recorded. The only extras
  the recorder emits are `poke FF06DC/FF06DE` (crosshair position), which is plain input the game reads every frame.
* Interaction recipe (all verified): `press A` enters/leaves pointer mode; poke the crosshair onto a verb cell and hold B
  4+ frames after >= 10 settle frames (shorter settle loses the click); poke it onto an object, check `$FF06AE`, press B;
  wait until `$FF09DC` bit 7 has been clear for 30 frames. **Use/Give**: select the verb, `press C` (inventory view,
  24 frames), put the crosshair on the item slot (slot c = 40*(c+1)+20, y 176/200), `press B`, `press C` back, click
  the target (the target then lands in `$FF06B6`, not `$FF06AE`). Two-object mode only starts if the *first* object's
  script has a Use/Give block with a second object (see `combos.py`), otherwise the verb fires at once on the item.
* Walking: D-pad is ~0.35 px/frame; floors are irregular, so `walk_to` moves axis-wise with sidesteps. Room exits are
  walked through (`exit_via`): after reaching the exit object keep pressing towards it. Pointer coordinates: `world =
  crosshair + camera ($FF06CA/$FF06CE)`, Shaggy's world y ~ crosshair y - 8.

## Routes (`tools/scripts/story/`)

All start at power-on. "warp" = through the CONTINUE password screen with the initial story state (script from
`tools/scripts/rooms/`), i.e. an isolated puzzle, not a from-the-start natural route. End states are asserted by the
generator and re-checked with `verify.py` (batch replay) - all six matched.

| route | start | what it does | end state |
|---|---|---|---|
| `w0_cafe_items` | natural (Lobby) | Lobby Archway -> Cafe; take **Bottle of Oil**, open the Cabinet and take the **Can Opener**, push the Radio round, open its battery compartment and take the **Battery** | room 13, inventory `[134,124,132]` |
| `w0_mine_wheels` | warp Mine (6) | take the three **Wheels** (the first two merge into "Wheels"), use Wheels on the Mine Car: "Well, I think it should work now." | `$07=7e`, `$08` bit4 set, wheels consumed |
| `w0_office` | warp Office (17) | look at the missing book, take the **Heater**, open the Drawer, take the **Scissors**, read the chalkboard | inventory `[96,92]` |
| `w0_kitchen` | warp Kitchen (15) | take the **Chili**, the **Beads**, push the Flour sack: reveals the **Peephole** (back of the Lobby painting) | inventory `[100,99]`, `$0D=c2` |
| `w1_front_lot` | natural (Front Lot, room 12) | take the Help Wanted **Sign**, walk through the Office Door into the Front Office | room 11, inventory `[84]` |
| `w1_boathouse` | warp Boathouse (14) | take the message **Bottle** and the **Boat** | inventory `[108,110]` |
| `w0_full` | natural | **the whole of Blake's Hotel**, power-on to the credits (12 segments, see below) | room 0 (game reset after the credits), same 29 story bytes as the header |
| `w1_part` | natural | Carnival: pole cheat, Magnet, Tokens, Crane Game, Tickets, Dart Game, Teddy Bear, Ball, Fun House, Bumper Car duel, Dart | room 7, inventory has Dart + Ball |

The six short routes start from password warps (isolated puzzles); `w0_full` and `w1_part` are the natural ones. Generate a route with
`tools/dev/genroute.sh ROUTE` on a VM (strict mode: no savestates, every probe recorded; it writes `runs/routes/ROUTE.txt`; pull it
before the next `vmrun`, which would wipe files outside `build/` and `runs/`).

## Story state ($FF2A00, 29 bytes)

The password carries exactly these 29 bytes: one bit per event / item / door. The tables below are **complete**: they are generated by
`tools/py/storybytes.py MYST` from the disassembly: for every flag bit, what the password-restore script (start script 2, which
rebuilds the world from the 29 bytes: items into the inventory, doors, objects moved) does when it is set, and which object script sets
or clears it under which verb (`event` = the object's timer script). `Fxx.b` in the disassembly = bit b of byte xx. Flags marked
*(none: engine/scratch flag)* are cutscene/minigame state that is never restored (the restore script ignores them).
Bytes `00`, `09`-`0B` are mostly engine scratch (scare-scene and animation phases); byte `0A` is also
reseeded from the RNG during the intro (the story checkpoints of storytest mask it).

### Blake's Hotel (mystery 0)

| byte | bit | restore effect (password load) | set by (object, verb) |
|---|---|---|---|
| `00` | 4 | (none: engine/scratch flag) | Door OPEN =1; Door OPEN =0 |
| `00` | 5 | Ancient Chieftan -> room 2 (The Basement); Scooby -> room 2 (The Basement); The Cook -> nowhere | Termites USE =1 |
| `00` | 6 | Fire -> nowhere | Pot o' Water USE =1 |
| `00` | 7 | Goblet -> INVENTORY |  |
| `01` | 0 | Goblet -> room 16 (The Lobby); Doll -> nowhere | Bellhop TALK =1; Doll GIVE =1 |
| `01` | 2 | Goblet state $00C6; Goblet -> room 20 (The Tomb) | Goblet USE =1 |
| `01` | 3 | Crumpled Note -> INVENTORY | Crumpled Note TAKE =1 |
| `01` | 4 | Light Bulb -> INVENTORY; Christmas Lights -> nowhere | Christmas Lights TAKE =1 |
| `01` | 5 | Bed Spring -> INVENTORY | Bed Spring TAKE =1 |
| `01` | 6 | Book -> INVENTORY | Book TAKE =1 |
| `01` | 7 | Antacid -> INVENTORY | Antacid TAKE =1 |
| `02` | 0 | Scissors -> INVENTORY | Scissors TAKE =1 |
| `02` | 1 | Heater -> INVENTORY | Heater TAKE =1 |
| `02` | 2 | Heater -> room 11 (Outside the Hotel); Heater state $0057 | Poison Oak USE =0; Heater TAKE =0; Heater USE =1 |
| `02` | 3 | Beads -> nowhere | Beads GIVE =1 |
| `02` | 4 | Beads -> INVENTORY | Beads TAKE =1 |
| `02` | 5 | Chili -> INVENTORY | Chili TAKE =1 |
| `02` | 6 | Chili -> nowhere; Empty Chili Can -> INVENTORY |  |
| `02` | 7 | Empty Chili Can -> nowhere; Termites -> INVENTORY | Empty Chili Can USE =1 |
| `03` | 0 | Vent Cover -> nowhere | Screwdriver USE =1 |
| `03` | 1 | Cow Bell -> INVENTORY | Cow Bell TAKE =1 |
| `03` | 2 | Soda Tab -> INVENTORY | Soda Tab TAKE =1 |
| `03` | 3 | Note -> INVENTORY | Note TAKE =1 |
| `03` | 4 | Pot -> INVENTORY | Pot TAKE =1 |
| `03` | 5 | Pot -> nowhere; Pot o' Water -> INVENTORY | Pot o' Water USE =0; Pot USE =1 |
| `03` | 6 | Can Opener -> INVENTORY | Can Opener TAKE =1 |
| `03` | 7 | Key -> INVENTORY | Key TAKE =1 |
| `04` | 0 | Lock -> nowhere | Key USE =1 |
| `04` | 1 | Bottle of Oil -> INVENTORY | Bottle of Oil TAKE =1 |
| `04` | 2 | Battery -> INVENTORY | Battery TAKE =1 |
| `04` | 3 | Screwdriver -> INVENTORY | Screwdriver TAKE =1 |
| `04` | 4 | Rack state $0009 | Crumpled Note USE =1 |
| `04` | 5 | Killer Lettuce -> nowhere |  |
| `04` | 6 | Medallion -> INVENTORY | Medallion TAKE =1 |
| `04` | 7 | Rope -> INVENTORY | Rope TAKE =1 |
| `05` | 0 | Rope state $0027; Rope -> room 4 (The Dungeon) | Rope USE =1 |
| `05` | 1 | Snow Covered Doors -> nowhere | Shovel USE =1 |
| `05` | 2 | Locked Doors -> nowhere | Crowbar USE =1 |
| `05` | 3 | Weed Killer -> INVENTORY | Weed Killer TAKE =1 |
| `05` | 4 | Work Gloves -> INVENTORY | Work Gloves TAKE =1 |
| `05` | 5 | Crowbar -> INVENTORY | Crowbar TAKE =1 |
| `05` | 6 | Poison Oak -> INVENTORY | Work Gloves USE =1 |
| `05` | 7 | Shovel -> INVENTORY | Shovel TAKE =1; Shovel USE =1 |
| `06` | 0 | Snowman -> nowhere; Hat -> room 12 (Outside the Hotel); Frozen Bell -> room 12 (Outside the Hotel) | Shovel USE =1 |
| `06` | 1 | Air Freshener -> INVENTORY |  |
| `06` | 2 | Pole -> INVENTORY | Pole TAKE =1; Pole USE =1 |
| `06` | 3 | Doll -> INVENTORY | Pole USE =1 |
| `06` | 4 | Battery -> nowhere; Soda Tab -> nowhere; Soda Tab and Battery -> INVENTORY | Soda Tab USE =1; Battery USE =1 |
| `06` | 5 | Battery -> nowhere; Light Bulb -> nowhere; Bulb and Battery -> INVENTORY | Battery USE =1; Light Bulb USE =1 |
| `06` | 6 | Light Bulb -> nowhere; Soda Tab -> nowhere; Bulb and Soda Tab -> INVENTORY | Soda Tab USE =1; Light Bulb USE =1 |
| `06` | 7 | Bulb and Battery -> nowhere; Soda Tab and Battery -> nowhere; Bulb and Soda Tab -> nowhere; Flashlight -> INVENTORY | Soda Tab USE =1; Battery USE =1; Light Bulb USE =1; Bulb and Soda Tab USE =1 |
| `07` | 0 | Frozen Bell -> INVENTORY | Frozen Bell TAKE =1 |
| `07` | 1 | Wheel -> nowhere | Wheel TAKE =1 |
| `07` | 2 | Wheel -> nowhere | Wheel TAKE =1 |
| `07` | 3 | Wheel -> nowhere | Wheel TAKE =1 |
| `07` | 4 | Wheel -> INVENTORY | Wheel TAKE =1 |
| `07` | 5 | Wheels -> INVENTORY | Wheel TAKE =1 |
| `07` | 6 | Wheels -> INVENTORY | Wheel TAKE =1 |
| `07` | 7 | Hose -> INVENTORY | Hose TAKE =1; Hose USE =1 |
| `08` | 0 | Hose state $00B6; Hose -> room 18 (The Shaft) | Hose USE =1 |
| `08` | 1 | Hose state $00B7; Hose -> room 18 (The Shaft) | Hose USE =1 |
| `08` | 2 | Extension Cord -> INVENTORY | Extension Cord TAKE =1 |
| `08` | 3 | Extension Cord state $0058; Extension Cord -> room 11 (Outside the Hotel);   -> room 11 (Outside the Hotel); Extension Cord state $0055;   -> nowhere; Heater state $0057 | Poison Oak USE =0; Extension Cord TAKE =0; Extension Cord USE =1 |
| `08` | 4 | Wheels -> nowhere; Mine Car state $0003 | Wheels USE =1 |
| `08` | 5 | (none: engine/scratch flag) | Passage WALK_ON =1; Door OPEN =0; Door OPEN =1 |
| `08` | 6 | (none: engine/scratch flag) | Passage WALK_ON =0; Secret Door WALK_ON =1 |
| `08` | 7 | (none: engine/scratch flag) | Door OPEN =1; Door OPEN =0 |
| `09` | 0 | (none: engine/scratch flag) | Christmas Lights TAKE =0; Bed Spring event =0; Bed Spring USE =1 |
| `09` | 3 | (none: engine/scratch flag) | Fred event =0 |
| `09` | 4 | (none: engine/scratch flag) | Daphne event =0 |
| `09` | 5 | (none: engine/scratch flag) | Velma event =0 |
| `09` | 6 | (none: engine/scratch flag) |   event =0 |
| `09` | 7 | (none: engine/scratch flag) |   event =0 |
| `0A` | 0 | (none: engine/scratch flag) |   event =0 |
| `0A` | 1 | (none: engine/scratch flag) |   event =0 |
| `0A` | 2 | (none: engine/scratch flag) |   event =0 |
| `0A` | 3 | (none: engine/scratch flag) |   event =1;   event =0 |
| `0A` | 4 | (none: engine/scratch flag) |   event =1;   event =0 |
| `0A` | 5 | (none: engine/scratch flag) |   event =1;   event =0 |
| `0A` | 6 | (none: engine/scratch flag) |   event =1;   event =0 |
| `0A` | 7 | (none: engine/scratch flag) |   event =1;   event =0 |
| `0B` | 0 | Statue state $00CA; Goblet state $00C8; Tomb state $00CC | Statue TALK =1 |
| `0B` | 1 | (none: engine/scratch flag) | Mine Car USE =0; Hose TAKE =0; Switch USE =1 |
| `0B` | 2 | Villain -> nowhere; Crumpled Note -> room 16 (The Lobby) | Peephole LOOK =1 |
| `0B` | 3 | (none: engine/scratch flag) | Door OPEN =1 |
| `0B` | 4 | (none: engine/scratch flag) | Door OPEN =1; Door OPEN =0 |
| `0B` | 5 | (none: engine/scratch flag) | Television USE =1; Television USE =0 |
| `0C` | 0 | Book -> nowhere | Book GIVE =1 |
| `0C` | 2 | (none: engine/scratch flag) | Antacid EAT =1; Termites USE =0 |
| `0C` | 4 | (none: engine/scratch flag) | Termites USE =1; Medallion USE =0 |
| `0C` | 5 | Uncle Blake -> room 20 (The Tomb); Uncle Blake state $0002 | Termites USE =1 |
| `0C` | 6 | (none: engine/scratch flag) | Rope USE =1 |
| `0D` | 0 | (none: engine/scratch flag) | Christmas Lights TAKE =1 |
| `0D` | 1 | (none: engine/scratch flag) | Archway WALK_ON =0 |
| `0D` | 2 |   -> room 21 (Across the River) | Scissors USE =1 |
| `0D` | 3 | (none: engine/scratch flag) | Microwave OPEN =1; Microwave SHUT =0; Microwave USE =0; Frozen Bell USE =1 |
| `0D` | 4 | (none: engine/scratch flag) | Refrigerator PUSH =0; Refrigerator OPEN =1; Refrigerator SHUT =0; Fridge Door SHUT =0 |
| `0D` | 5 | (none: engine/scratch flag) | Refrigerator PULL =0; Refrigerator PUSH =1 |
| `0D` | 6 | Flour state $0096; Peephole -> room 15 (The Kitchen) | Flour PUSH =1 |
| `0D` | 7 | (none: engine/scratch flag) | Drawer OPEN =0; Drawer SHUT =1 |
| `0E` | 0 | (none: engine/scratch flag) | Door OPEN =0; Door SHUT =1 |
| `0E` | 1 | (none: engine/scratch flag) | Outside Door OPEN =0; Outside Door SHUT =1; Main Entrance OPEN =0; Main Entrance SHUT =1 |
| `0E` | 2 | (none: engine/scratch flag) | Door OPEN =0 |
| `0E` | 3 | (none: engine/scratch flag) | Drawer OPEN =0; Drawer SHUT =1 |
| `0E` | 4 | (none: engine/scratch flag) | Drawer OPEN =0; Drawer SHUT =1 |
| `0E` | 5 | (none: engine/scratch flag) | Shed OPEN =0; Shed SHUT =1 |
| `0E` | 7 | (none: engine/scratch flag) | Cabinet OPEN =0; Cabinet SHUT =1 |
| `0F` | 0 | (none: engine/scratch flag) | Double Doors OPEN =0; Double Doors SHUT =1 |
| `0F` | 1 | (none: engine/scratch flag) | Locker OPEN =1; Locker SHUT =0 |
| `0F` | 2 | Bear state $0001 | Poison Oak USE =0; Heater TAKE =0; Heater USE =1 |
| `0F` | 3 | Frozen Bell -> nowhere | Microwave USE =1 |
| `0F` | 4 | Frozen Bell -> nowhere | Microwave USE =0; Frozen Bell TAKE =0; Frozen Bell USE =1 |
| `0F` | 5 | Totem Pole Bridge -> room 11 (Outside the Hotel) | Poison Oak USE =1 |
| `0F` | 6 | (none: engine/scratch flag) | Missing Book LOOK =1 |
| `0F` | 7 | (none: engine/scratch flag) | Refrigerator OPEN =1 |
| `10` | 0 | (none: engine/scratch flag) | Radio PUSH =1; Radio PULL =0 |
| `10` | 1 | (none: engine/scratch flag) | Radio OPEN =1; Radio SHUT =0 |

Door/cabinet open-shut states are bits `F0E.x`/`F0F.x`, `F0D.x` (set on restore, so not listed as restore effects): Lobby Door F0E.0,
Outside Door/Main Entrance F0E.1, Hallway Gardener door F0E.2, Gardener drawers F0E.3/F0E.4, Shed F0E.5, Cafe Cabinet F0E.7, cellar Double
Doors F0F.0, Basement Locker F0F.1, Office Drawer F0D.7, Microwave F0D.3, fridge F0D.4/F0D.5, Flour/Peephole F0D.6, Bear warmed F0F.2,
Totem Pole Bridge F0F.5, Missing Book seen F0F.6, fridge looked-in F0F.7; Hallway doors puzzle F0B.3 (all six opened) / F0B.4 (scare scene
running); Peephole scene seen F0B.2; engine running F0B.1; Statue answered F0B.0; Uncle Blake: freed F0C.5, in the Tomb F0C.3,
read the Book F0C.0, Scooby captured F0C.4 + F00.5; credits via `Medallion USE Hook`.

### Ha Ha Carnival (mystery 1)

| byte | bit | restore effect (password load) | set by (object, verb) |
|---|---|---|---|
| `00` | 4 | (none: engine/scratch flag) | Ladder to the Bungee Jump USE =1; Ladder to the Bungee Jump USE =0; Ladder USE =1; Ladder USE =0 |
| `00` | 5 | (none: engine/scratch flag) | Exit WALK_ON =0 |
| `00` | 6 | (none: engine/scratch flag) | Exit WALK_ON =1 |
| `00` | 7 | Dart -> room 7 (The Bumper Cars) | Bumper Car USE =1 |
| `01` | 0 | Dart -> INVENTORY | Dart TAKE =1 |
| `01` | 1 | Magnet -> room 27 (The Ferris Wheel); Magnet -> room 2 (The Ferris Wheel); Magnet state $0033 | Balloons USE =0; Pole PULL =1 |
| `01` | 2 | Bandage -> room 21 (The Haunted House) | Coffin OPEN =1 |
| `01` | 3 | Paint Brush -> INVENTORY | Paint Brush TAKE =1 |
| `01` | 4 | Paint Brush -> nowhere; Inky Brush -> INVENTORY | Paint Brush USE =1; Bucket with Inky USE =1 |
| `01` | 5 | Boat -> nowhere; Spark Plug -> nowhere; Boat with Spark Plug -> INVENTORY | Spark Plug USE =1; Boat USE =1 |
| `01` | 6 | Boat -> INVENTORY | Spark Plug USE =1; Boat TAKE =1 |
| `01` | 7 | Spark Plug -> INVENTORY | Spark Plug TAKE =1; Boat USE =1 |
| `02` | 0 | Magnet -> INVENTORY | Balloons USE =1 |
| `02` | 1 | Ball -> room 13 (The Back Lot); Balancing Bear -> nowhere | Teddy Bear GIVE =1 |
| `02` | 2 | Ball -> INVENTORY | Ball TAKE =1 |
| `02` | 3 | Bucket -> INVENTORY |   event =1 |
| `02` | 4 | Bucket -> nowhere; Bucket of Water -> INVENTORY | Bucket USE =1 |
| `02` | 5 | Bucket of Water -> nowhere; Bucket with Inky -> INVENTORY; Inky -> nowhere | Bucket of Water USE =1 |
| `02` | 6 | Tokens -> INVENTORY | Magnet USE =1 |
| `02` | 7 | Coupon -> INVENTORY | Coupon TAKE =1 |
| `03` | 0 | Hammer -> INVENTORY | Coupon GIVE =1; Pole PULL =1 |
| `03` | 1 | Boots -> INVENTORY | Boots TAKE =1 |
| `03` | 2 | Water Pistol -> INVENTORY | Water Pistol TAKE =1 |
| `03` | 3 | Darts -> room 19 (The Game Tent) | Darts USE =0; Tickets GIVE =1 |
| `03` | 4 | Application -> INVENTORY | Application TAKE =1 |
| `03` | 5 | Application -> nowhere | Rubber Stamp USE =1 |
| `03` | 6 | Wrench -> INVENTORY | Wrench TAKE =1 |
| `03` | 7 | Bolts -> INVENTORY | Wrench USE =1 |
| `04` | 0 | Sign -> INVENTORY | Sign TAKE =1 |
| `04` | 1 | Sign -> nowhere; Sign -> INVENTORY | Inky Brush USE =1 |
| `04` | 2 | Sign -> room 13 (The Back Lot) | Sign USE =1 |
| `04` | 3 | Taffy -> INVENTORY | Taffy TAKE =1; Taffy EAT =0; Taffy USE =0 |
| `04` | 4 | Bottle -> nowhere; Note -> INVENTORY | Bottle OPEN =1 |
| `04` | 5 | Kid -> room 12 (The Front Lot) |  |
| `04` | 6 | Kid -> room 24 (The Boardwalk) |  |
| `04` | 7 | Bottle -> INVENTORY | Bottle TAKE =1 |
| `05` | 0 | Rubber Stamp -> INVENTORY | Rubber Stamp TAKE =1 |
| `05` | 1 | Mummy Bandages -> INVENTORY | Mummy Bandages TAKE =1 |
| `05` | 2 | Boat with Spark Plug -> nowhere | Boat with Spark Plug USE =1 |
| `05` | 3 | (none: engine/scratch flag) | Dart USE =1 |
| `05` | 4 | (none: engine/scratch flag) |   event =1;   event =0 |
| `05` | 5 | Clown -> room 13 (The Back Lot) | Application USE =1; Sign USE =1 |
| `05` | 6 | (none: engine/scratch flag) | Boots GIVE =1 |
| `05` | 7 | (none: engine/scratch flag) | Dart USE =1; Dart USE =0 |
| `06` | 0 | (none: engine/scratch flag) | Dart USE =1; Dart USE =0 |
| `06` | 1 | Dart Attendant -> nowhere | Window 3 LOOK =1; Tent Flap WALK_ON =0 |
| `06` | 2 | (none: engine/scratch flag) | Madame Zelda TALK =1 |
| `06` | 3 | (none: engine/scratch flag) | Bucket TAKE =1 |
| `06` | 5 | (none: engine/scratch flag) | Window 2 LOOK =1 |
| `06` | 6 | (none: engine/scratch flag) | Mummy Bandages TAKE =1; Bottle OPEN =1 |
| `06` | 7 | (none: engine/scratch flag) | Boots GIVE =1 |
| `07` | 0 | Engine state $0000; Engine state $0001; Engine state $0003 | Switch USE =1; Switch USE =0 |
| `07` | 1 | (none: engine/scratch flag) | Taffy USE =1 |
| `07` | 3 | The Gang -> nowhere; Manager -> room 11 (The Front Office); Rubber Stamp -> room 11 (The Front Office) | Boat with Spark Plug USE =1 |
| `07` | 4 | (none: engine/scratch flag) | Balloons USE =1; Balloons USE =0 |
| `07` | 5 | (none: engine/scratch flag) | Pole PULL =1 |
| `07` | 6 | Mirror state $0015 | Mirror PUSH =1 |
| `07` | 7 | (none: engine/scratch flag) | Shed OPEN =1; Shed SHUT =0 |
| `08` | 0 | (none: engine/scratch flag) |   event =1;   event =0 |
| `08` | 1 | (none: engine/scratch flag) |   event =0;   event =1 |
| `08` | 2 | (none: engine/scratch flag) | Bolts USE =0 |
| `08` | 3 | Brake state $000D | Brake USE =1; Brake USE =0 |
| `08` | 4 | Brake state $000E | Brake USE =1; Brake USE =0 |
| `08` | 5 | (none: engine/scratch flag) | Darts USE =1 |
| `08` | 6 | Water Pistol -> nowhere; Tickets -> INVENTORY; Kid state $0002 | Water Pistol GIVE =1 |
| `08` | 7 | (none: engine/scratch flag) | Bucket of Water USE =1; Hatch OPEN =1; Hatch SHUT =0; Magnet USE =1 |
| `09` | 0 | Water Pistol -> room 19 (The Game Tent) |  |
| `09` | 1 | (none: engine/scratch flag) | Tent Flap WALK_ON =1 |
| `09` | 2 | (none: engine/scratch flag) |   event =1;   event =0 |
| `09` | 3 | (none: engine/scratch flag) | Trash Can OPEN =1; Trash Can SHUT =0 |
| `09` | 4 | (none: engine/scratch flag) | Bandage PULL =1 |
| `09` | 5 | (none: engine/scratch flag) | Coffin OPEN =1 |
| `09` | 6 | (none: engine/scratch flag) | Coffin OPEN =1; Coffin SHUT =0 |
| `09` | 7 | (none: engine/scratch flag) | Pole PULL =0 |
| `0A` | 0 | Hammer Game state $0066 | Pole PULL =1 |
| `0A` | 2 | (none: engine/scratch flag) | Balancing Bear event =0; Balancing Bear event =1 |
| `0A` | 3 | (none: engine/scratch flag) | Teddy Bear GIVE =0 |
| `0A` | 4 | (none: engine/scratch flag) | Trunk OPEN =1; Trunk SHUT =0 |
| `0A` | 6 | (none: engine/scratch flag) |   event =0 |
| `0A` | 7 | (none: engine/scratch flag) |   event =0 |
| `0B` | 0 | (none: engine/scratch flag) |   event =0 |
| `0B` | 1 | (none: engine/scratch flag) |   event =0 |
| `0B` | 2 | (none: engine/scratch flag) |   event =0 |
| `0B` | 3 | (none: engine/scratch flag) |   event =1;   event =0 |
| `0B` | 4 | (none: engine/scratch flag) |   event =1;   event =0 |
| `0B` | 5 | (none: engine/scratch flag) |   event =1;   event =0 |
| `0B` | 6 | (none: engine/scratch flag) |   event =1;   event =0 |
| `0B` | 7 | (none: engine/scratch flag) |   event =1;   event =0 |

Carnival state not restored but used: Switch/Engine F07.0/F07.1 (engine on / taffy belt), Gang rescued F07.3, Clown lured F05.5, Kid
tall enough F05.6, Coffin opened F09.5/F09.6, Bandage pulled F09.4, Hammer game broken F0A.0, Fun House choice object 3 var.

## Object scripts (found while decoding)

* Object table at header `+$28` (8 bytes per object: script offset, name offset); object `i` is hit-test id `i + 3` (ids 1,2
  are Shaggy and Scooby). Blake: 181 objects, Carnival ~155. Names/offsets: `tools/py/objects.py`.
* Script = `{u16 size, u16 0x000F, u16 0xFFFF}` then a sequence of **verb blocks** `0001 VERB OBJ2 LEN body...`
  (`script_op_01`: matches `ram_action_verb` == VERB and, for Use/Give, `ram_target2_obj` == OBJ2; if it matches the body
  runs in "frame" mode, else it skips LEN bytes). Conditions are `0003`/`0004` with a flag test (`btst bit, $FF2A00+byte` or
  an object-field compare, evaluator `sub_002796`), `0010 PARAMS 0000 TEXTOFF` says a string at pool + TEXTOFF, `000C n`
  runs sub-op *n* (table `script_subop_table` `$2EB4`: set flags, give/remove items, change rooms, cutscenes).
* So: an item "uses with" exactly those second objects that appear in its `0001 05/06 OBJ2` blocks - `combos.py` lists them.

## Puzzle chains (from the script tables + observed texts; **not all executed**)

Blake's Hotel (Use/Give pairs from `combos.py 0`; tid in brackets):

* Flashlight for the dark Maze: Light Bulb(179) + Battery(135) + Soda Tab(121) combine pairwise (Bulb+Battery=181, Bulb+Tab=180,
  Tab+Battery=182) into the final Flashlight(183). Battery: Cafe Radio (done in `w0_cafe_items`).
* Termites for Uncle Blake (Dungeon room 5): Can Opener(127) Use with Chili(103) -> Empty Chili Can(104); Screwdriver(140) Use
  with Vent Cover(108) (Kitchen); Empty Chili Can Use with Termites(107) -> Termites(105); Termites Use/Give with Uncle Blake(26).
* Boiling water / bell: Pot(124) Use with Sink(106) -> Pot o' Water(123) Use with Fire(40) (Lobby); Frozen Bell(117) Use with
  Microwave(116) -> Cow Bell(118).
* Heater(99) Use with Extension Cord(176); Cord Use with Outlet(60) (outside) - melts/clears the snow; Shovel(72) Use with Snow
  Covered Doors(59) / Snowman(71) / Bear(177).
* Shed: Key(129, Cafe shelf behind the Cook) Use with Lock(70); Crowbar(67) Use with Lock/Nail/Locked Doors/Vent Cover;
  Work Gloves(65) Use with Poison Oak(89); Weed Killer(66) Use with Killer Lettuce(168) (tomb path).
* Tomb: Book(91) Use with Missing Book(98) in the Office; Goblet(41) Use with Statue(166) then Talk to the statue ("Hokus Pokus!")
  -> Medallion(165) Use with Hook(146) in the Basement. Goblet: trade the Kachina Doll(172, caught with the Pole(171) across the
  river) and Beads(102) with the Bellhop(6). Rope(169) Use with Scissors(95) across the river; Crumpled Note(42) Use with Rack(145).
* Mine: three Wheels fit the Mine Car (done in `w0_mine_wheels`); Hose(148) Use with Gas(150) and Engine(147) in the Shaft
  ("The lift engine isn't running").
* Ending text: the villains are the Gardener (Jim) and the Cook; Tribal Police arrest them after the Medallion.

Ha Ha Carnival (`combos.py 1`): Spark Plug(29) Use with Boat(113); Boat Use with Pond(33) (rescue the gang);
Application(69/70) Use with Bulletin Board(67), Rubber Stamp(79) Use with Application(69) -> accepted clown application ->
Use with Clown(119); Dart(12) Use with Balloons(50-52); Tickets(64)/Darts(55) Give to Dart Attendant(53) -> Teddy Bear(54)
Give to Balancing Bear(122); Tokens(139) Use with Crane Game(62)/Madame Zelda(94); Hammer(146) Use with Hammer Game(145);
Bucket(128) Use with Faucet(96)/Pond(33)/Inky(143), Paint Brush(22) Use with Bucket with Inky(131), Inky Brush(23) Use with
Sign(87); Magnet(147) Use with Treasure(138); Wrench(72) Use with Bolts(154) (Haunted House); Coupon(148) Give to Attendant(144).
Ending: the Dart Attendant, Charlie, is the clown villain.

## Blake's Hotel: the complete walkthrough (`tools/py/story_full.py`, route `w0_full`)

Power-on to the credits in twelve chained segments (`hotel_1` .. `hotel_12`), every action decided from RAM (hit-test
target, room word, story bytes, inventory), none from frame counts. Each segment ends with `need()` assertions on RAM
conditions; the checkpoints (`story.py checkpoint`) are what `port/tools/storytest.sh` compares between `--hle none` and `--hle all`.
`tools/scripts/story/w0_full.txt` is the flat harness script of the same run (12 000 lines, replays with `verify.py`
to the same end state: room 0 = the game restarted at the Sega logo after the credits, same inventory, same 29 story bytes).
Object numbers are hit-test ids (object index + 3); `Fxx.b` = bit b of `$FF2A00+xx`.

The solution is the dependency chain below (found by reading the disassembled scripts, `scriptdis.py` / `blk.py`, not by trial):

```
END   Medallion USE Hook (Basement)  -> unmasking cutscene, Jim the Gardener, credits, reset            F0C.4=0
 Medallion  <- Statue TALK "XYZZY!" (Tomb; menu shows 4 wrong words first)         needs F0C.0 (Uncle Blake read the Book)
   F0C.0    <- Book GIVE Uncle Blake (Tomb)  needs F0C.3 (Tomb entered after he was freed, F0C.5)
   Book     <- Gardener's Room drawer 2 (Hallway door 81, unlocked by opening all six Hallway doors)
 Uncle Blake free (F0C.5) <- Termites USE Uncle Blake (lower Dungeon, room 5)
   Termites <- Empty Chili Can USE Termites (Kitchen)  needs the Vent Cover off (Screwdriver USE Vent Cover, Basement item)
   Empty Chili Can <- Can Opener (Cafe cabinet) USE Chili (Kitchen)
   room 5   <- Rope USE (room 4) after Rope USE Cuffs;  Rope <- Scissors USE Rope across the river (Office drawer)
   room 4   <- Tomb Archway;  Tomb needs the Killer Lettuce dead (Weed Killer, Shed)
 Tomb <- Pond Archway -> Maze (dark: Flashlight = Light Bulb + Battery + Soda Tab; the walk planner then solves the maze) -> Exit 161
   Pond <- Shaft Stone Path;  Air Freshener USE at the pond makes the Stone Bridge walkable
   Shaft <- Mine Car (three Wheels in the Mine fix it);  back up: Hose -> Engine -> Gas then Switch (engine on, F0B.1; the ride resets it)
   Mine <- Basement Secret Door, revealed by the Crumpled Note USE Rack (note from the Lobby after the peephole scene, fire put out with the Pot o' Water)
   Basement <- Outside Double Doors: Shovel USE Snow Covered Doors + Crowbar USE Locked Doors (Shed, opened with the Key)
 across the river <- Totem Pole Bridge <- Poison Oak USE Bear (Work Gloves from the Shed) after Heater + Extension Cord (Basement locker) warmed the cave
 Light Bulb <- Christmas Lights TAKE while Scooby bounces on the Bed Spring (Gardener's Room item, USE outside)
 Key <- Cafe shelf, only when the Cook is gone: Antacid (Gardener drawer 1) EAT in the Cafe
```

| seg | what | end condition (RAM) |
|---|---|---|
| `hotel_1` | boot, Lobby -> Cafe: Oil, Can Opener (cabinet), Battery (radio: push, open) | inventory has Can Opener, Battery |
| `hotel_2` | Hallway: open the six doors (each plays an Ancient Chieftan scare scene), Gardener door unlocks (F0B.3, F0E.2 clear); Gardener's Room: Air Freshener, Bed Spring, Antacid, Book | all four items |
| `hotel_3` | Cafe: EAT Antacid in front of the Cook, take the Key | Key |
| `hotel_4` | Hallway dumb waiter (Scooby rides along) -> Kitchen: Can Opener on Chili, Pot, Pot on Sink, push the fridge -> Soda Tab, push the Flour sack + LOOK Peephole (cutscene, F0B.2), back; Lobby: Pot o' Water on the Fire, Crumpled Note | Note, Soda Tab, Empty Can |
| `hotel_5` | Office: Heater, Scissors (drawer) | both |
| `hotel_6` | Outside -> Shed (Key on Lock, open): Crowbar, Work Gloves, Weed Killer, Shovel; Shovel/Crowbar open the cellar doors; Basement: Screwdriver, Extension Cord (locker), Note on the Rack | F04.4 |
| `hotel_7` | Secret Door -> Mine: three Wheels, Wheels on the Mine Car, ride; Shaft: Hose -> Engine, Hose -> Gas; Pond: Air Freshener; Switch, ride up, Basement | F08.1, F0B.1 |
| `hotel_8` | Outside: Cord + Heater (F0F.2), Gloves on the Poison Oak (Gardener's Room), Bed Spring -> Light Bulb, Poison Oak on the Bear -> bridge (F0F.5); across the river: Scissors on the Rope, Take Rope | Rope |
| `hotel_9` | Kitchen again: Screwdriver on the Vent Cover, Chili Can on the Termites; Battery + Light Bulb + Soda Tab -> Flashlight | Flashlight |
| `hotel_10` | Kitchen -> ... -> Basement -> Mine -> Shaft -> Pond -> dark Maze (room 7): USE Flashlight (room 8, 512x440 px), the planner walks to Exit 161 -> Tomb | room 20 |
| `hotel_11` | Tomb: Weed Killer on the Lettuce; Dungeon: Rope on the Cuffs, down the rope, Termites on Uncle Blake (cutscene, F0C.5); Tomb: Give Book (F0C.0), TALK Statue "XYZZY!" (Medallion appears), Take | Medallion |
| `hotel_12` | Pond, Shaft (Switch again), ride, Mine, Basement: Medallion on the Hook; wait for the credits to end | room word 0 |

Not needed for the ending (the scripts never test them): the Goblet (Bellhop trade for the Kachina Doll caught with the Pole across the
river), Beads, Frozen Bell/Cow Bell, Oil, Christmas-light order, the Snowman. `Statue TALK` works without the Goblet in place;
only the Medallion's animation shows the missing goblet.

Gotchas found on the way (all handled in `story.py` / `story_full.py`):

* **Pointer hit test**: the object under the crosshair is the world cell `((px+8+cam_x)>>3, (py-8+cam_y)>>3)`. The static hotspot
  rectangle of every non-actor object is in the ROM (`hotspots.py`: object record +2 -> overlay table), so `locate()` computes the
  point instead of scanning the screen; it probes the rectangle with the live hit test (and, in two-object mode, `$FF06B6`).
  Shaggy/Scooby/actors win the hit test where they overlap, so the whole rectangle is probed; actors (Uncle Blake, Scooby) are
  probed around their sprite (`probe_actor`). Every probe poke/frame is recorded in the flat route: the pointer position is
  game input and a replay that skips it diverges (found the hard way: the route replayed to a different state).
* **Walking**: `walkplan.py` plans on the game's own collision map (`walkmap.py`): D-pad geometry (x moves by the speed, y by half,
  diagonals stop at a wall, only straight moves slide), a 2 px safety margin off the collision fence, other exits' hotspots
  (`tools/scripts/story/walkon_w*.json`: objects with a WALK_ON that changes rooms) as no-go areas, stepping out of the zone first
  when a spawn point lies on another exit, off-model pixels (spawn just outside the fence) handled with axis-wise moves, walls
  the model does not know learned on the fly (replan around the pixel that blocked us), and "get as close as possible" for
  items in parts of a room Shaggy cannot enter (the Basement screwdriver; the pickup script walks him there itself). This solved
  the Lobby Outside Door gap, the Gardener door auto-entry from the Hallway spawn, and the dark Maze.
* **Inventory** is 4 slots per row, 8 visible, page in `$FF06EC` (scroll by clicking the arrows in the right cell); `pick_item` pages and
  retries; the second object of "Use X with Y" shows in `$FF06B6`.
* **TALK menu**: wait for `$FF06EE == $FFFF` (pointer hidden by `talk_menu_begin`), choice texts at `$FF0838`, used bits `$FF0874`,
  D moves the highlight (6-frame presses), B picks.
* **Ending**: `sys ending_credits` runs the credits and the game resets to the Sega logo (`$FF06AC` = 0).

## Ha Ha Carnival: what is driven (`carnival_1` .. `carnival_5`, route `w1_part`) and the rest of the chain

`w1_part` plays the first five segments closed-loop from power-on and ends with the **Dart and the Ball** in the inventory (104 s on the
harness; `tools/scripts/story/w1_part.txt` replays to the same end state):

| seg | what | end condition |
|---|---|---|
| `carnival_1` | Front Lot -> Boardwalk: Taffy; Archway -> Hammer Game: **pull the pole** instead of playing (Orbit cutscene: the Magnet flies to the Ferris wheel, the attendant gives the hammer) | F01.1 |
| `carnival_2` | Back Lot: Balloons USE with Scooby (he has to be on screen and still: `probe_actor` waits) -> Ferris wheel -> Magnet; Diving Bell: Valve down, Magnet USE Treasure -> Tokens, Valve up | Magnet, Tokens |
| `carnival_3` | Game Tent: Tokens in the Crane Game (**claw minigame**: `crane_game()` reads the pistol's path offset `$FF0610` and drops the claw so the offset at the bottom is in `$FC..$110`), Water Pistol -> Kid (Front Lot) for the Tickets, Tickets -> Dart Attendant, Darts x3 -> Teddy Bear | Teddy Bear |
| `carnival_4` | Back Lot: Teddy Bear GIVE Balancing Bear -> Ball | Ball |
| `carnival_5` | Boardwalk -> Fun House (the script op `wait_dpad_choice` waits for a direction: D = Bumper Cars, L = Haunted House, R = back) -> Bumper Cars: **Taffy on the Engine with the Switch off**, Switch on, the **car duel** (`bumper_tug()`: steer at the Clown's car, hold the gas; it wins on the first try), Dart on the floor | Dart |

Minigame notes (decoded from `script_subop_27_minigame_throw` `$3F58` and `script_subop_32_minigame_tug` `$3142`): the Crane Game
claw is x `$FF04E8` (100..148), A/B drops it (2 px/frame down to y `$4A`), and it catches iff the pistol's path offset at the bottom is in
`$F8..$114` (claw x != $94) or `$5C..$78` (x = $94); the offset advances about 1.66 per frame and wraps at $140. The Bumper Car duel is a
top-down arena: headings 0..7 clockwise from north in the actor anim word `$FF0488`, L/R turn, A/B gas (speed `$FF0A90`, 0..7,
vectors in `dat_003D96`), two 63-point meters `$FF0A98` (ours) / `$FF0A9A` (the Clown's), the first one below zero loses; a loss
returns to room 7 and the car can be driven again. The duel needs input from its first frame, so `start_bumper()` clicks USE without the
usual wait for the script to finish.

**Not driven** (the chain for the ending, from the scripts; Fxx.b = story bit):

```
END   Dart USE Ball in the Back Lot (room 13) while the Clown is there -> unmasking, Charlie the Dart Attendant, Orbit, credits
 Clown in room 13 (F05.5) <- Application #70 (accepted) USE Bulletin Board (Dressing Room) with the painted Sign #88 already in room 13,
                              or the painted Sign USE in the Back Lot after the application was posted
   painted Sign #88  <- Inky Brush USE Sign #87 (Front Lot, the Help Wanted board)
     Inky Brush     <- Paint Brush #22 USE Bucket with Inky #131
       Paint Brush  <- Shed #21 in the Roller Coaster (room 8): OPEN after F08.0 (the runaway-coaster scene: brakes #18/#19 USE in the
                       right order, Bolts #154 USE Track with the Wrench in hand)   [needs the Kid + Boots: see below]
       Bucket with Inky <- Bucket of Water USE Inky (Diving Bell, hatch open: F08.7 is already set by the Magnet step)
         Bucket of Water <- Bucket #128 USE Faucet #96 (Boardwalk);   Bucket <- Bungee Jump (Back Lot ladder -> platform -> Harness)
   Application #70 <- Rubber Stamp #79 USE Application #69 (rejected, Dressing Room pile: Take)
     Rubber Stamp #79 / Manager are placed in the Front Office when the gang is rescued: Boat with Spark Plug USE Pond (F07.3)
       Boat #113 (Boathouse) + Spark Plug #29 (Hall of Mirrors: Mirror PUSH, F07.6)
     Dressing Room (room 17): Game Tent flap #66 (walk-on conditions F09.4 / F09.1)
 Kid + Boots: Kid TALK "Wanna ride the roller coaster with me and Scoob?" (Front Lot) walks him to the Boardwalk; Boots #153 (Haunted House,
              room 21, Fun House choice L) GIVE Kid sets F05.6 (he is tall enough) and the Attendant #99 lets the party on (room 8)
 Haunted House: Coffin #155 OPEN, Bandage #157 PULL (F09.4; needs the Mummy Bandages from the Boardwalk trash can + the Note from the Boathouse bottle, F06.6)
```

Things that are *not* needed: Hammer, Coupon, Madame Zelda, the Ferris-wheel peek windows, Bungee ride itself (only the Bucket).

## Verification

    tools/py/verify.py tools/scripts/story/w0_full.txt runs/x     # on a VM (needs build/harness); ~80 s

prints `room`, `inv`, `story`; all eight routes reproduce the header's end state (`w0_full`: room 0 after the credits, 18 items, the
29 story bytes of the header; `w1_part`: room 7, Dart + Ball). The closed-loop drivers pass on the port (below). `make` still prints
`build/scooby.md: OK`; the merged coverage went from 83.6 % to 91.3 % with the two long routes (+1121 instruction starts, see
`docs/coverage.md`).

## Disassembler note: "14 traced instructions overlap ROM data reads"

The `tools/py/disasm.py` warning (`00259e 00272c 00286c 002870 002874 002878 00287a 00287e`) is **not a mis-trace**. Each of
the 8 instructions has its execute bit set by the CPU hook (they genuinely run: `$259e` is `bset #2,ram_game_state` in
`script_op_1C`, `$272c` is the `lea (6,a5),a5` tail of `script_op_1E`, `$286c-$287e` is the `bra.w`/`cmp` ladder of the
condition evaluator `sub_002796`), and in addition some 68k read touched the same bytes as data (2 bytes at `$259e`, 2 at
`$272e`, 18 at `$286e-$287f`). The decoded instructions are right either way, so nothing in the generator needed fixing; the
message was reworded to a note. Cause (not reproduced): all three sites are inside the script interpreter, whose operands are
fetched from `a5`-relative script words, so the likeliest source is an interpreter run with a script pointer that lies in
code (out-of-range object index / offsets under the all-ones and random story states of the earlier campaign). The
story crawl, some 1500 verb attempts on password-warp states and the six routes, never produced such a read (the crawler logs any read of those addresses to
`runs/overlap_hits.txt`; empty), which agrees with it needing a corrupt state. Open: a state that triggers it.
The two long routes added six more instructions to the list (`$226a 2270 2276 2278 227e` in `sub_00221A`/`sub_002278`, the room
enter/after-fade script runners, and `$2e7e`): same signature, same conclusion (the instructions run, and something also read the same
bytes as data; the disassembly is correct either way). The generator lists them in the note it prints.

Also fixed while here: `mergecov.py` now ignores the spurious exec start at `$000000` (the core reports it when a savestate is
loaded into a running session), and `covreport.py` now finds function entries through symbolic labels (it only knew
`loc_XXXXXX`, so the report was silently counting 1 function after the symbol renames). `loc_001C0C`, previously labelled "verb
PULL handler" in `docs/coverage.md`, is the TALK post-script handler (`action_execute`: `cmpi.w #8,d7`) and is now covered.

## Closed-loop validation in the native port (issue #32)

`port/tools/storytest.sh` replays the story routes against `build/port/scoobyport` with `--hle none` and `--hle all` and
compares a checkpoint after every high-level step (room, inventory, story bytes `$FF2A00..+29`).

* **Control channel**: none needed beyond the existing `--script`: the port's script reader accepts a FIFO, so
  `tools/py/session.py` drives it exactly like the harness (`SCOOBY_BACKEND=port:HLESPEC`). Two port changes: `echo` also
  appends `FRAME TEXT` to `OUTDIR/peek.log` (the sync marker), and `trace`/`untrace` are accepted and ignored. `story.py`
  decides each next input from RAM it reads back (walk until position reached, settle until `$FF09DC` bit 7 is clear, retry the
  verb select), so emulated-time differences do not matter. `said()` returns `[]` on the port (no bus trace); checks are RAM based.
* `STORY_PRESS_MIN=5` (set by the script) stretches the 3-4 frame presses of the password-warp scripts: with the C replacements a
  3-frame D press on the title sub-menu is lost, so CONTINUE was not selected (an open-loop artefact, not a story difference).
* Story byte 10 (`$FF2A0A`) is masked in checkpoints: constant after boot, but `$01` in the harness / `--hle none` and `$D9`
  with the replacements (set during the intro from something timing dependent; not story state).
* `port/tools/hlebisect.py ROUTE` bisects the replacement list (`--hle all,-a,-b`) and prints the replacements whose removal
  restores agreement. `STORYTEST_BISECT=1` runs it from storytest; `STORYTEST_HLE=all,-vblank_main` overrides the "all" spec.

**Result (HLE state at `d1391bb`)**: `--hle none` completes every route on the port with the same checkpoints as the harness
route headers. `--hle all` diverges at the first item pickup, and bisect names the single replacement `vblank_main`
(`port/src/game/mainloop.c:90`, its `hle_call68k(0x005C8E)` = `verb_vblank_dma`). After `Take`, `inventory_refresh` (`$5CF6`) sets
`$FF09DC` bit 5 and spins at `$5D0E` until the VBlank clears it, but `$5C8E` only acts when the beam (`$C00008` high byte) is
in `$E1..$F6`; in the C VBlank the V counter apparently never gets there (not checked at register level), so the bit is never
cleared and the game stays at `$FF09DC` = `$E4` (action pending, verb Take selected; the PC alternates `$5D0E/$5D16` for
thousands of frames) and further clicks are ignored. More frames never help, so it is not a timing artefact. The fix belongs to
the vblank owner (e.g. let the HLE'd `$5C8E` ignore the beam test).
