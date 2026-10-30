# Story walkthrough routes (issue #29)

Status, honestly: **neither mystery is solved.** What exists is (a) the tooling that drives the game through its
real verb UI and observes the story state, (b) six verified flat routes that make real story progress (items,
puzzle states), and (c) a static table of every Use/Give combination the scripts accept, from which the full
puzzle chains can be planned. Each route replays from power-on with the plain batch harness and ends in the state
recorded in its header (checked, see "Verification").

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

Not done: connecting the rooms into one run (kitchen is behind the Cook, the Lobby's Outside Door is not reachable with
the greedy walker, the Carnival start room's other exits were not mapped), the cutscene/ending chains, the Carnival
puzzles.

## Story state ($FF2A00, 29 bytes)

The password carries exactly these 29 bytes. Mostly *one bit per event/item/door*; observed transitions (byte:bits,
Blake unless `(C)` = Carnival):

| byte | bit (value) | meaning |
|---|---|---|
| `00` | 2, 3 | runtime scratch bits written by script ops `1C`/`1D` (copies of `$FF0AB7`/`$FF0ABC` bits); not story |
| `01` | 6 (C: `0x40`) | Boat taken (C); 2 = Coffin opened (C) |
| `02` | 4 (`10`) | Beads taken; 5 (`20`) Chili taken; 1 (`02`) Heater taken; 0 (`01`) Scissors taken |
| `03` | 6 (`40`) | Can Opener taken; (C) 1 = Boots taken |
| `04` | 1 (`02`) | Bottle of Oil taken; 2 (`04`) Battery taken; 3 (`08`) Screwdriver taken; (C) 0 Sign taken, 7 Bottle taken, 4 Bottle opened |
| `05` | 7 (`80`) | Shovel taken |
| `07` | 1,4,5,6 | Mine wheels collected: `12` (one) -> `36` (two) -> `7e` (three) |
| `08` | 4 (`10`) | Mine Car repaired (`20 -> 30`); (C) Diving Bell hatch `06 -> 86` |
| `0D` | 6, 7... | `82` base; Drawer opened `->02`, Microwave opened `->8a`, Flour pushed (Peephole) `->c2`, Christmas Lights tried `->83` |
| `0E` | one bit per door/cabinet | `ff` all shut: Lobby Door bit0, Main Entrance bit1 (`fd`), Outside Door `fd`, Hallway doors `fb/f7/ef`, Cafe Cabinet bit7 (`7f`), Office Door bit0 |
| `0F` | 1 / 6 | Basement Locker opened `01 -> 03`; Office "missing book" seen `01 -> 41` |
| `10` | 0 / 1 | Cafe Radio turned (`01`), battery compartment opened (`03`) |

(Everything else is still unobserved; the table is meant to be extended from `crawl.py` output.)

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

## Verification

    tools/py/verify.py tools/scripts/story/w0_cafe_items.txt runs/x     # on a VM (needs build/harness)

prints `room`, `inv`, `story`; all six routes reproduce the header's end state. `make` still prints `build/scooby.md: OK`
(and `data/exec_starts.txt` gained 77 instruction starts, 83.1 % -> 83.6 %; see `docs/coverage.md`).

## Disassembler note: "8 traced instructions overlap ROM data reads"

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

Also fixed while here: `mergecov.py` now ignores the spurious exec start at `$000000` (the core reports it when a savestate is
loaded into a running session), and `covreport.py` now finds function entries through symbolic labels (it only knew
`loc_XXXXXX`, so the report was silently counting 1 function after the symbol renames). `loc_001C0C`, previously labelled "verb
PULL handler" in `docs/coverage.md`, is the TALK post-script handler (`action_execute`: `cmpi.w #8,d7`) and is now covered.
