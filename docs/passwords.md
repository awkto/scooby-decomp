# Passwords (CONTINUE screen)

C port (GitLab #21): the codec as plain C is `port/src/game/password.c` (`password_encode` / `password_decode`, tested
against `tools/py/password.py` by `port/tools/password_test.sh`); the screens are `menu_password.c`. See
`docs/subsystems/menus.md`.

Title menu -> PLAY BLAKE'S HOTEL / PLAY HA HA CARNIVAL -> sub-menu START / **CONTINUE** / CANCEL. CONTINUE opens
"ENTER PASSWORD". The game encodes the whole story state (first 29 bytes of the state block `$FF2A00`) plus the
current room into a 50-character password, so a password can place the player in any room with any flag set.
`tools/py/password.py` implements the codec; the harness uses it to warp into all 51 rooms
(`states/INDEX.md`).

Code: menu `$008FB8` (Blake) / `$008FC2` (Carnival) -> `$008FCA`; entry loop `$009008-$0091CA`; verify
`loc_009298`; accept path `$009328` -> `loc_008CD6`; encoder (used to show a password) `loc_008660`; the byte
scrambler `loc_00876C` (decode) / `loc_00873E` (encode).

## Entry screen

* Grid of 4 rows x 8 columns of the 32-character alphabet `ABCDEFGHIJKLMNOPQRSTUVWXYZ!+#%*?` (`$00871E`), plus a 5th
  row of icons: col 0 = cursor left, 1 = cursor right, 2/3 = switch password row, 4 = clear, 5 = **END** (verify),
  7 = cancel back to the title.
* The password has 2 rows x 25 characters, shown as 5 groups of 5. Buffer `$FF08BC`: 31 bytes per row, 5 characters then a
  space (`$20`) per group, NUL at the end of the row. Cursor: `d7` = grid row, `d6` = column, `d4` = position in the
  row, `d5` = row.
* D-pad moves the cursor (wraps); **A, B or Start** (any of pad bits `$D0`) types the highlighted character or runs the
  icon. A press must last at least 5 frames to be seen reliably (the loop does heavy sprite work; 3-4 frame presses are
  sometimes lost). The buffer is pre-filled with a garbage password.

## Format

Characters -> 5-bit symbols, packed MSB-first into 32 bytes `r[0..31]` (50 symbols = 250 bits; `r[31]` and the last two bits
are ignored). Decoding (`$009298` + `$00876C`):

    room  = (r[1] ^ r[0]) & 0x1F          # start room, as in $FF06AC (2..22 Blake, 2..31 Carnival)
    s[0]  = r[2] ^ r[0];   s[i] = r[2+i] ^ r[1+i]   for i = 1..28      # 29 state bytes
    check: c = room; X = 0; c = roxl(c) twice (through X); for each s[i]: c = roxl(c); c ^= s[i];  valid iff c == r[1]

On success: `$FF2A00..+$100` is cleared, `s[0..28]` copied to `$FF2A00`, `$FF0A90 = room`, and the game starts in that room
(the normal intro cutscenes still play first; press Start to skip them). The Blake/Carnival choice is *not* in the
password, it comes from which menu entry was used. Any 29 bytes are accepted as long as the checksum matches, so
arbitrary flag/inventory combinations (the harness uses the world's initial state block, all-ones and random bytes) can
be loaded.

Example: `.venv/bin/python -I tools/py/password.py encode 9 [HEX29]` prints the 50 characters for room 9 (all-zero
state by default); `decode PASSWORD` reverses it and reports validity.

Note: Carnival room 8 (The Roller Coaster, 64x19) did not start from a password with the initial state (the game came up
in room 2); possibly the start room is overridden by a flag combination.

## Pause menu and the hidden DEBUG menu

Start in-game opens the same menu builder (`loc_00851E`, `$FF09ED` bit 0 set once a game started): the current password
is shown at the top (`loc_008660` generates it from `$FF06AC` + `$FF2A00`), then CONTINUE / QUIT MYSTERY. QUIT MYSTERY
asks for confirmation ("NO! DONT DO IT!" replaces CONTINUE; select with Start only).

Cheat code, checked in the menu input loop `loc_008DA6` against the 11-byte table at ROM `$010118`:
**A, B, A, C, A, B, Left, Right, A, B, Left** (each alone, released in between; hold at least ~8 frames). It sets bit 1
of `$FF09ED` and plays sound 3. Close (CONTINUE) and reopen the pause menu: it now has 4 items with "DEBUG" labels:
CONTINUE / QUIT MYSTERY / **ROOMS** (`loc_0088A2`: list of this mystery's rooms via `loc_0088D8`; D-pad moves the red bar, **B**
selects and returns to the pause menu, then CONTINUE loads the room: `$FF000A` -> `$FF06AC`) / **OBJECTS**
(`loc_008C40`: object editor, `loc_008AB4`). `tools/scripts/debug_menu_w0.txt` / `_w1.txt` do this from a warp state.
The first d-pad press after a savestate load is sometimes ignored; give the menu 250 frames to initialise first.

## In-game controls (observed)

D-pad walks Shaggy. **C** toggles pointer mode (arrow icons at the bottom corners); in pointer mode the d-pad moves a
crosshair, which highlights verbs when over the verb bar and names objects under it ("Outside Door"), and **B** clicks.

## Game saves in the port (#28)

`port/src/saveslot.c` stores a password state in 8 slots (`~/.local/share/scooby-decomp/slotN.sav`, text: `mystery`, `room`,
`roomname`, `date`, `playtime` in frames, `password`). Saving encodes `$FF06AC` + `$FF2A00..+28` with `password.c`, exactly as the pause
menu's password display does. Loading does not poke the game state: after `emu_reset()` it presses the game's own buttons
(Start x3 through the title, D for Carnival, Start = mystery, D = CONTINUE, Start = ENTER PASSWORD), waits for the entry screen, writes
the 2 x 31-byte display text into `$FF08BC` (what the entry loop redraws from), moves the grid cursor to END (Up, Left x3) and presses A.
The ROM's checker (`$009298`) and start code (`$00933E` -> `$008CD6`) then run; the loader is done when `$FF09ED` has
`MF_FROM_PASSWORD` set, `$FF0A90` equals the room and `$FF2A00..+28` equals the slot. If a menu step was missed it starts over
with 2x then 3x longer waits (3 attempts). The mystery's intro cut scene plays afterwards, as after typing a password.
Since #35 a save is only taken where the game's own pause menu could open (`slot_can_save`: in a mystery, no script
running -- `ram_vblank_flags` bit 7 clear -- and `$FF09E9` bit 4 clear, the main loop's own pause condition), so a save is
always a password the game itself would show. During a cut scene F8 answers "not during a cut scene"; a saved-mid-cut-scene
state such as the intro's Mystery Machine (room 26) loaded into the van with Shaggy frozen and nothing to do. The loader also sets `ram_script_cb` if it is still 0 (next section) -- the one value it writes that is not the
password text. `port/tools/slottest.sh` saves 8 slots along each full walkthrough and compares the loaded game against the
same password typed key by key (mystery, room, inventory, story bytes); it also stores a save every time the walkthrough has
been in a new room for 60 frames, and for every save checks that the loaded game becomes playable: no script running, verb
bar shown, and the D-pad walks Shaggy (a hang freezes RAM and fails). `SCOOBY_BIN="port/tools/nativewrap.sh scoobyport"` runs
the loads on the native build. Script commands for this: `slot N|NAME`, `autoslot PREFIX DELAY`, `loadslot N|NAME`
(`port/src/script.c`); `--load-slot` also takes a `.sav` path.

## Cold-start password bug (#35, a cartridge bug)

Some Ha Ha Carnival passwords hang the original game when typed in right after power-on: the screen keeps
cycling colour 0 (the bus error handler loop at `$892`). Cause:

* `game_start` (`$000CB8`) runs the mystery's two start-up scripts with a direct `bsr script_run_frame` and never sets
  `ram_script_cb` (`$FF0878`), the loop that `script_op_04_if_else` (`$00274A`) calls (`jsr (a0)`) for a true then-body.
* Only the verb code (`action_execute`, `$1A2A`/`$1A66`) and the TALK reply code (`$1F20`) leave a value there; the
  room scripts, object events and `script_run_nested` save and restore it. The boot code clears work RAM on a cold
  start (the soft reset of QUIT MYSTERY keeps it).
* Blake's Hotel's start-up scripts have no if/else. The Carnival's second one (`start2`, ROM `$1BB392`) has exactly one:
  `if F07.0 == 1: (if F07.1 == 1: Engine = 0 else Engine = 1) else: (if F07.1 == 1: Engine = 3)` at `$1BB488`
  (the bumper car engine). Story bit F07.0 is set once the engine has been dealt with (slots 4-8 of the w1_full
  walkthrough). With such a password and a cold console, the then-body is called through address 0: the 68000
  runs the vector table, takes an illegal-instruction exception into non-code and ends in the bus error loop.

Checked on the original code (`--hle none`), at 7 points of the w1_full walkthrough: the password the game itself shows
in the pause menu equals the slot's password except for the last character (its two low bits are the ignored bits 248-249,
which the game fills from uninitialised `ram_pw_raw+31`, the port's encoder with 0). Typing the game's own password after
power-on hangs for every state with F07.0 set (4 of 4) and works for the others; two of them (slots 6 and 8) typed after playing
and QUIT MYSTERY (soft reset, `ram_script_cb` = `script_run_frame` or `script_run` left by the last verb) load fine into
the saved room (both values tested). So the saved state is a state the real game produces and shows; the hang is the cartridge's.

The slot loader therefore makes the console "warm": just before pressing END it writes `ram_script_cb` = `script_run_frame`
(`$002406`, the execute-mode loop the then-body was written for, and what a game played since power-on usually leaves)
if the pointer is still 0 (`warm_script_cb` in `port/src/saveslot.c`). Nothing else differs from a hand-typed password
on a console that has been played. A password typed by hand into the port right after start-up still hangs, as on the
cartridge.

Result: slottest 149/149 saves load into a playable game on the emulated build and on `scooby` (16 numbered saves plus
55 + 78 room-change saves of w0_full / w1_full). With `warm_script_cb` and the typed run's poke disabled, w1_full slots 4-8
fail (the game sits in the intro's van, room 26, with a script never finishing), so the gate catches the hang.
