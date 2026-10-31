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
`port/tools/slottest.sh` saves 8 slots along each full walkthrough and compares the loaded game against the same password typed
key by key (mystery, room, inventory, story bytes).
