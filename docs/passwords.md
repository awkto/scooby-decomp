# Passwords (CONTINUE screen)

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
