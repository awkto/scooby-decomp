#!/usr/bin/env python3
"""Scooby-Doo Mystery password codec + harness input generator.

Format (reverse-engineered from the CONTINUE screen code at $008FE4-$0093F8, checker $009298):
  * 2 rows x 25 characters, shown in 5 groups of 5.  Alphabet (32 chars, 5 bits each, MSB first):
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ!+#%*?"  ($00871E).  Total 50 symbols = 250 bits packed into r[0..31].
  * r1 = checksum, r0 ^ r1 (low 5 bits) = start room number (as in $FF06AC, 2..22 / 2..31).
  * r[2+i] = s[i] ^ r[1+i] (i = 0..28; for i=0 the previous byte is r0).  s[0..28] are the first 29 bytes of the
    game-state block $FF2A00 (story flags + inventory); the loader zeroes $FF2A00..+$100 then copies s there.
  * checksum: c = room; X = 0; rotate c left through X twice; then for each s[i]: c = roxl(c,1); c ^= s[i].
    The password is valid iff c == r1.
CLI:
  password.py encode ROOM [HEX29]        -> 50 chars (hex state default all zero)
  password.py decode PASSWORD50          -> room, state hex, valid
  password.py script ROOM [HEX29] [--world N] -> harness commands: from the title-menu savestate 'titlemenu'
"""
import sys

ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!+#%*?"


def checksum(room, s):
    c, x = room & 0xFF, 0
    def rox(c, x):
        n = (c >> 7) & 1
        return ((c << 1) & 0xFF) | x, n
    c, x = rox(c, x)
    c, x = rox(c, x)
    for b in s:
        c, x = rox(c, x)
        c ^= b
    return c


def encode(room, s=None):
    s = list(s) if s else [0] * 29
    assert len(s) == 29
    r1 = checksum(room, s)
    r0 = (r1 ^ room) & 0x1F
    r = [r0, r1]
    prev = r0
    for b in s:
        prev = b ^ prev
        r.append(prev)
    r.append(0)
    bits = "".join(f"{b:08b}" for b in r)
    return "".join(ALPHA[int(bits[i:i + 5], 2)] for i in range(0, 250, 5))


def decode(pw):
    bits = "".join(f"{ALPHA.index(ch):05b}" for ch in pw) + "0" * 6
    r = [int(bits[i:i + 8], 2) for i in range(0, 256, 8)]
    room = (r[1] ^ r[0]) & 0x1F
    s, d1 = [], r[0]
    for i in range(29):
        s.append(r[2 + i] ^ d1)
        d1 = r[2 + i]
    return room, s, checksum(room, s) == r[1]


def typing_script(pw, gap=5, hold=5):
    """Cursor starts at row 0 col 0 of an 8-col grid (index = row*8+col); A enters a char.
    Row 4 = commands (col 5 = END)."""
    out = []
    row = col = 0

    def go(r, c):
        nonlocal row, col
        while row != r:
            if (r - row) % 5 <= 2:
                out.append(f"press D {hold}"); row = (row + 1) % 5
            else:
                out.append(f"press U {hold}"); row = (row - 1) % 5
            out.append(f"run {gap}")
        while col != c:
            if (c - col) % 8 <= 4:
                out.append(f"press R {hold}"); col = (col + 1) % 8
            else:
                out.append(f"press L {hold}"); col = (col - 1) % 8
            out.append(f"run {gap}")

    for ch in pw:
        i = ALPHA.index(ch)
        go(i // 8, i % 8)
        out += [f"press A {hold}", f"run {gap}"]
    # dump the 2x31-byte entry buffer ($FF08BC) for verification before pressing END
    out += [f"peek pwbuf{i} {0xFF08BC + 4 * i:X} 4" for i in range(16)]
    go(4, 5)
    out += [f"press A {hold}", "run 10"]
    return out


def full_script(world, room, state=None, tail=400):
    """From the savestate 'titlemenu': enter the password for (room, state) and let the room load."""
    pw = encode(room, state)
    return (title_to_password_screen(world) + typing_script(pw)
            + [f"run {tail}"] + ["press S 4", "run 150"] * 3 + ["run 200", "peek room FF06AC 2", "peek myst FF06AA 2", "peek state FF2A00 4"])


def buffer_expect(pw):
    """Expected $FF08BC contents (62 bytes): rows of 25 chars with a separator byte after each group."""
    b = bytearray(64)
    for r in range(2):
        for k in range(25):
            b[31 * r + k + k // 5] = ord(pw[25 * r + k])
    return b


def title_to_password_screen(world):  # from the savestate at the title menu
    s = []
    if world:
        s += ["press D 3", "run 10"]
    s += ["press S 4", "run 60", "press D 3", "run 10", "press S 4", "run 200"]
    return s


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "encode":
        st = bytes.fromhex(a[2]) if len(a) > 2 else None
        print(encode(int(a[1]), st))
    elif a[0] == "decode":
        room, s, ok = decode(a[1])
        print(room, bytes(s).hex(), "valid" if ok else "INVALID")
    elif a[0] == "script":
        world = int(a[a.index("--world") + 1]) if "--world" in a else 0
        st = bytes.fromhex(a[2]) if len(a) > 2 and not a[2].startswith("--") else None
        pw = encode(int(a[1]), st)
        print("\n".join(full_script(world, int(a[1]), st)))
