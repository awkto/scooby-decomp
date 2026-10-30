#!/usr/bin/env python3
"""Scooby-Doo Mystery (Genesis) graphics asset tool.

Pure-python (stdlib only).  ROM layout / formats are documented in docs/formats.md.

Usage (from repo root, ROM at rom/baserom.md):
  gfx.py lzss  ROMADDR OUT.bin            decompress one LZSS stream
  gfx.py tiles ROMADDR|raw:ADDR:LEN OUT.png [--pal ROMADDR] [--cols 16]
                                          dump 4bpp tiles (LZSS stream or raw)
  gfx.py pal   ROMADDR OUT.png [--n 16]   dump CRAM-format palette words as swatches
  gfx.py room  N OUT.png [--layer a|b|both] [--grid]
                                          reconstruct room N's background (N=2 is the Lobby)
  gfx.py rooms [--world W] [--png DIR]    list the room table (optionally render all rooms)
  gfx.py screen NAME OUT.png [--frame K]  rebuild an intro/title screen (sega, license,
                                          acclaim, sunsoft, illusions, title)
  gfx.py vram  DUMP.vram DUMP.cram OUT.png [--plane ADDR] [--w 64] [--h 32]
                                          render a nametable from a harness `vdp` dump
"""
import argparse
import os
import struct
import sys
import zlib

ROM_PATH = os.path.join(os.path.dirname(__file__), '..', '..', 'rom', 'baserom.md')

# ---------------------------------------------------------------------------
# ROM helpers

_rom = None


def rom():
    global _rom
    if _rom is None:
        with open(ROM_PATH, 'rb') as f:
            _rom = f.read()
    return _rom


def u16(b, o):
    return struct.unpack_from('>H', b, o)[0]


def u32(b, o):
    return struct.unpack_from('>I', b, o)[0]


# ---------------------------------------------------------------------------
# LZSS decompressor (68k routine $0097AA)
#
# Bitstream: MSB-first over big-endian bytes, starting at the stream address.
#   flag 1  -> 8-bit literal
#   flag 0  -> 12-bit ring position P; P == 0 terminates;
#              else 4-bit L, copy L+2 bytes from ring[P], ring[P+1], ...
# Ring buffer: 4096 bytes, write index starts at 1, every output byte is also
# written to the ring.  Initial ring contents are whatever is in RAM at the
# ring address (the game passes $FF3000); streams seen so far never read
# unwritten ring slots, so we use zeros.


def lzss_decompress(data, off=0, ring_init=None):
    """Return (output bytes, offset just past the last byte consumed)."""
    ring = bytearray(ring_init) if ring_init is not None else bytearray(4096)
    out = bytearray()
    r = 1
    bitpos = off * 8
    n = len(data)

    def bits(k):
        nonlocal bitpos
        v = 0
        for _ in range(k):
            byte = data[bitpos >> 3] if (bitpos >> 3) < n else 0
            v = (v << 1) | ((byte >> (7 - (bitpos & 7))) & 1)
            bitpos += 1
        return v

    while True:
        if bits(1):
            c = bits(8)
            out.append(c)
            ring[r] = c
            r = (r + 1) & 0xFFF
        else:
            p = bits(12)
            if p == 0:
                break
            ln = bits(4) + 2
            for i in range(ln):
                c = ring[(p + i) & 0xFFF]
                out.append(c)
                ring[r] = c
                r = (r + 1) & 0xFFF
    # the 68k reads the stream as one long followed by words
    used = (bitpos + 7) // 8 - off
    return bytes(out), off + used


# ---------------------------------------------------------------------------
# RLE decompressor (68k routine $009C64), a PackBits variant
#
#   u32 unpacked size, then records until that many bytes are produced:
#     c = 0x00..0x7F : copy the next c+1 bytes
#     c = 0x80..0xFF : repeat the next byte ((-c) & 0xFF) + 1 times  (0x80 -> 129)


def rle_decompress(data, off=0):
    """Return (output bytes, offset just past the stream)."""
    size = u32(data, off)
    p = off + 4
    out = bytearray()
    while len(out) < size:
        c = data[p]
        p += 1
        if c < 0x80:
            out += data[p:p + c + 1]
            p += c + 1
        else:
            out += bytes((data[p],)) * (((-c) & 0xFF) + 1)
            p += 1
    return bytes(out[:size]), p


# ---------------------------------------------------------------------------
# Colours / palettes

# GPGX maps 3-bit channel levels linearly (0..7 -> 0..255 via x*255/7 in its
# 8-bit path; its RGB565 path expands 3->5/6 bits).  We emit the "normal mode"
# levels used by most tools.
LEVELS = [0, 36, 73, 109, 146, 182, 219, 255]


def color(w):
    """Hardware CRAM word 0000BBB0GGG0RRR0 -> (r, g, b)."""
    return (LEVELS[(w >> 1) & 7], LEVELS[(w >> 5) & 7], LEVELS[(w >> 9) & 7])


def palette_words(b, off, n=16):
    return [u16(b, off + 2 * i) for i in range(n)]


def palette(b, off, n=16):
    return [color(w) for w in palette_words(b, off, n)]


# ---------------------------------------------------------------------------
# Tiles and nametables


def tile_pixels(t, off):
    """32-byte 4bpp tile -> list of 64 colour indices (row-major)."""
    px = []
    for i in range(32):
        v = t[off + i]
        px.append(v >> 4)
        px.append(v & 15)
    return px


def render_tiles(tiles, pal, cols=16):
    ntiles = len(tiles) // 32
    rows = (ntiles + cols - 1) // cols
    w, h = cols * 8, max(rows, 1) * 8
    img = [[(0, 0, 0)] * w for _ in range(h)]
    for t in range(ntiles):
        px = tile_pixels(tiles, t * 32)
        tx, ty = (t % cols) * 8, (t // cols) * 8
        for i, c in enumerate(px):
            img[ty + i // 8][tx + i % 8] = pal[c % len(pal)]
    return img


def render_nametable(names, tiles, pals, w, h, transparent=None, base=None, prio=None):
    """names: list of w*h nametable words; tiles: VRAM-like bytes indexed by tile
    number*32; pals: 4 lists of 16 rgb tuples.  Colour 0 of each palette is the
    transparent colour; when `transparent` is given those pixels are left as
    that value (to composite layers), otherwise palette entry 0 is drawn."""
    img = base if base is not None else [[(0, 0, 0)] * (w * 8) for _ in range(h * 8)]
    for cy in range(h):
        for cx in range(w):
            e = names[cy * w + cx]
            if prio is not None and (e >> 15) != prio:
                continue
            tno = e & 0x7FF
            hf = (e >> 11) & 1
            vf = (e >> 12) & 1
            pal = pals[(e >> 13) & 3]
            toff = tno * 32
            if toff + 32 > len(tiles):
                continue
            px = tile_pixels(tiles, toff)
            for y in range(8):
                sy = 7 - y if vf else y
                row = img[cy * 8 + y]
                for x in range(8):
                    sx = 7 - x if hf else x
                    c = px[sy * 8 + sx]
                    if c == 0 and transparent is not None:
                        continue
                    row[cx * 8 + x] = pal[c]
    return img


# ---------------------------------------------------------------------------
# PNG writer (RGB, 8-bit)


def write_png(path, img, scale=1):
    h = len(img)
    w = len(img[0]) if h else 0
    raw = bytearray()
    for row in img:
        line = bytearray()
        for (r, g, b) in row:
            line += bytes((r, g, b)) * scale
        for _ in range(scale):
            raw.append(0)
            raw += line

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(bytes(raw), 9)))
        f.write(chunk(b'IEND', b''))


# ---------------------------------------------------------------------------
# Game data structures (see docs/formats.md)

WORLD_TABLE = 0x031AF2      # 2 longs: per-mystery header pointers (index = $FF06AA)
UI_PALETTES = 0x032480      # 32 words -> CRAM palettes 0-1 (shared UI/sprite palettes)


class World:
    """Mystery ('world') header, 0x34+ bytes of absolute longs."""
    FIELDS = ['state_start', 'state_end', 'rooms', 'tiles_base', 'maps_base', 'palettes',
              'f18', 'f1c', 'names_base', 'f24', 'f28', 'f2c', 'start_room']

    def __init__(self, idx):
        r = rom()
        self.idx = idx
        self.addr = u32(r, WORLD_TABLE + 4 * idx)
        for i, n in enumerate(self.FIELDS):
            setattr(self, n, u32(r, self.addr + 4 * i))
        # room table runs up to the next table (tiles_base) in both worlds
        self.nrooms = (self.tiles_base - self.rooms) // 0x14

    def room(self, n):
        return Room(self, n)


class Room:
    """Room n (numbering as in $FF06AA/$FF06AC: first entry is room 2)."""

    def __init__(self, world, n):
        r = rom()
        self.world, self.n = world, n
        e = world.rooms + (n - 2) * 0x14
        self.entry = e
        self.name_off, self.tiles_off, self.maps_off, self.f0c, self.f10 = \
            [u32(r, e + 4 * i) for i in range(5)]
        end = r.index(b'\0', world.names_base + self.name_off)
        self.name = r[world.names_base + self.name_off:end].decode('latin-1')
        # tiles: [u32 packed size][LZSS stream] -> VRAM $0000
        self.tiles_addr = world.tiles_base + self.tiles_off + 4
        # map pack: u16 width, u16 height (cells), then 3 x [u32 packed size][LZSS stream]
        mp = world.maps_base + self.maps_off
        self.map_w, self.map_h = u16(r, mp), u16(r, mp + 2)
        self.chunks = []
        a = mp + 8
        for _ in range(3):
            self.chunks.append(a)
            a += u32(r, a - 4) + 4
        self.after_maps = a - 4     # $FF0692: room script/object data follows
        # palettes: 128 bytes per room; +0x40 = CRAM palettes 2-3
        self.pal_addr = world.palettes + (n - 2) * 128 + 0x40

    def tiles(self):
        return lzss_decompress(rom(), self.tiles_addr)[0]

    def maps(self):
        """(plane B names, plane A names, walk/zone map); each map_w x map_h words."""
        out = []
        for a in self.chunks:
            d = lzss_decompress(rom(), a)[0]
            out.append([u16(d, 2 * i) for i in range(len(d) // 2)])
        return out

    def palettes(self):
        r = rom()
        return [palette(r, UI_PALETTES), palette(r, UI_PALETTES + 32),
                palette(r, self.pal_addr), palette(r, self.pal_addr + 32)]


def render_room(room, layer='both', crop=None):
    """Room background from ROM data alone.  Layers are composited in VDP
    priority order (B low, A low, B high, A high; sprites omitted); backdrop is CRAM
    entry 0 (black), as set by the room code (VDP reg 7 = 0)."""
    tiles = room.tiles()
    b, a, _walk = room.maps()
    w, h = room.map_w, room.map_h
    pals = room.palettes()
    backdrop = pals[0][0]
    img = [[backdrop] * (w * 8) for _ in range(h * 8)]
    layers = [m for m, k in ((b, 'b'), (a, 'a')) if layer in (k, 'both')]
    for pr in (0, 1):                     # B low, A low, B high, A high
        for m in layers:
            render_nametable(m, tiles, pals, w, h, transparent=True, base=img, prio=pr)
    if crop:
        x0, w = crop
        img = [row[x0:x0 + w] for row in img]
    return img


def render_vram(vram, cram, plane, w, h):
    pals = [[color(u16(cram, 32 * p + 2 * i)) for i in range(16)] for p in range(4)]
    names = [u16(vram, plane + 2 * i) for i in range(w * h)]
    return render_nametable(names, vram, pals, w, h)


def compose(vram, cram, a_base=0xC000, b_base=0xE000, pw=64, ph=32, sw=256, sh=224,
            hscroll=(0, 0), vscroll=(0, 0), backdrop=0, layers='ab'):
    """Minimal VDP background compositor (no sprites, no window, full-screen
    scroll only): backdrop, B low, A low, B high, A high."""
    pals = [[color(u16(cram, 32 * p + 2 * i)) for i in range(16)] for p in range(4)]
    bd = pals[backdrop >> 4][backdrop & 15]
    img = [[bd] * sw for _ in range(sh)]
    tcache = {}

    def plane_px(base, hs, vs):
        out = [[None] * sw for _ in range(sh)]
        for y in range(sh):
            py = (y + vs) % (ph * 8)
            for x in range(sw):
                px_ = (x - hs) % (pw * 8)
                e = u16(vram, base + 2 * ((py >> 3) * pw + (px_ >> 3)))
                tno = e & 0x7FF
                if tno not in tcache:
                    tcache[tno] = tile_pixels(vram, tno * 32) if tno * 32 + 32 <= len(vram) else [0] * 64
                ty, tx = py & 7, px_ & 7
                if e & 0x1000:
                    ty = 7 - ty
                if e & 0x0800:
                    tx = 7 - tx
                c = tcache[tno][ty * 8 + tx]
                if c:
                    out[y][x] = (e >> 15, pals[(e >> 13) & 3][c])
        return out

    planes = []
    if 'b' in layers:
        planes.append(plane_px(b_base, hscroll[1], vscroll[1]))
    if 'a' in layers:
        planes.append(plane_px(a_base, hscroll[0], vscroll[0]))
    for prio in (0, 1):
        for pl in planes:
            for y in range(sh):
                row, src = img[y], pl[y]
                for x in range(sw):
                    v = src[x]
                    if v is not None and v[0] == prio:
                        row[x] = v[1]
    return img


# ---------------------------------------------------------------------------
# Intro / title screens.  Each is a list of load operations mirroring the 68k
# code that builds it (addresses are ROM offsets; see docs/formats.md):
#   ('lzss', src, vram)            LZSS stream -> VRAM
#   ('rle',  src, vram)            RLE stream -> VRAM
#   ('raw',  src, nbytes, vram)    uncompressed copy -> VRAM
#   ('rows', src, words, rows, vram, stride)  nametable rows (loc_00992C per row)
#   ('lzssrows', src, words, rows, vram, stride)  same, from an LZSS stream
#   ('rleptr', table, vram)        RLE stream picked from a pointer table (--frame)
#   ('cram', src, nwords[, first]) palette words -> CRAM (fade target)

SCREENS = {
    # code at $000952..$0009C6 (palette is colour-cycled at runtime)
    'sega': dict(ops=[('lzss', 0x023286, 0x0000), ('rows', 0x023226, 12, 4, 0xC594, 0x80),
                      ('cram', 0x0236A0, 64)], pw=64, sw=256),
    # code at $000A1A..$000A62: blue Acclaim/Hanna-Barbera licence card (H40 mode)
    'license': dict(ops=[('raw', 0x023720, 0x41C0, 0x0000), ('rows', 0x0278E0, 40, 28, 0xC000, 0x80),
                         ('cram', 0x0281A0, 64)], pw=64, sw=320),
    # code at $000A9A..$000AF6: Acclaim logo (slides in via hscroll, final hscroll 0)
    'acclaim': dict(ops=[('rows', 0x0219C0, 32, 6, 0xC480, 0x80), ('raw', 0x021B40, 0x300, 0xE480),
                         ('lzss', 0x021E40, 0x0000), ('cram', 0x02241A, 64)], pw=64, sw=256),
    # code at $000B4E..$000B9A
    'sunsoft': dict(ops=[('rows', 0x02249A, 26, 9, 0xC406, 0x80), ('lzss', 0x02266E, 0x0000),
                         ('cram', 0x0231A6, 64)], pw=64, sw=256),
    # code at $000BF4..$000C88: Illusions logo; 15 animation frames (LZSS $028220,
    # 20x5 names each) then the final 25x15 card (LZSS $02864C) shown here
    'illusions': dict(ops=[('lzss', 0x0287C8, 0x0000), ('lzssrows', 0x02864C, 25, 15, 0xC288, 0x80),
                           ('cram', 0x02967A, 64)], pw=64, sw=256),
    # code at $008196..$0083E4: title backdrop on plane B, animated plane A
    # frames from the 28-entry pointer table at $017064 (RLE, 32x28 names)
    'title': dict(ops=[('lzss', 0x01155A, 0x0000), ('rle', 0x016D38, 0xB900), ('rle', 0x016E6A, 0xBB00),
                       ('rle', 0x016F76, 0xBD00), ('raw', 0x0165B8, 0x700, 0xE000),
                       ('rleptr', 0x017064, 0xC000),
                       ('cram', 0x016CB8, 64), ('cram', 0x032482, 30, 1)], pw=32, sw=256),
}


def build_screen(name, frame=None):
    """Return (vram, cram, params) built from ROM data alone."""
    r = rom()
    s = SCREENS[name]
    vram = bytearray(0x10000)
    cram = bytearray(0x80)
    for op in s['ops']:
        k = op[0]
        if k == 'lzss':
            d = lzss_decompress(r, op[1])[0]
            vram[op[2]:op[2] + len(d)] = d
        elif k == 'rle':
            d = rle_decompress(r, op[1])[0]
            vram[op[2]:op[2] + len(d)] = d
        elif k == 'rleptr':
            fr = 27 if frame is None else frame
            d = rle_decompress(r, u32(r, op[1] + 4 * fr))[0]
            vram[op[2]:op[2] + len(d)] = d
        elif k == 'lzssrows':
            _, src, words, rows, dst, stride = op
            d = lzss_decompress(r, src)[0]
            for i in range(rows):
                vram[dst + i * stride:dst + i * stride + 2 * words] = d[i * 2 * words:(i + 1) * 2 * words]
        elif k == 'raw':
            vram[op[3]:op[3] + op[2]] = r[op[1]:op[1] + op[2]]
        elif k == 'rows':
            _, src, words, rows, dst, stride = op
            for i in range(rows):
                vram[dst + i * stride:dst + i * stride + 2 * words] = r[src + i * 2 * words:src + (i + 1) * 2 * words]
        elif k == 'cram':
            first = op[3] if len(op) > 3 else 0
            cram[2 * first:2 * (first + op[2])] = r[op[1]:op[1] + 2 * op[2]]
    return vram, cram, s


def render_screen(name, frame=None):
    vram, cram, s = build_screen(name, frame)
    return compose(vram, cram, pw=s['pw'], ph=32, sw=s['sw'])


# ---------------------------------------------------------------------------
# Asset map (data/assets.txt)

# Shared room-screen assets loaded by the room loader at $0075A8..$007780
UI_ASSETS = [
    (0x030016, 0x030316, 'map-raw', 'verb bar names, 6 rows x 64 (61 used) -> VRAM $AA80 (+tile base $FF0802)'),
    (0x030916, None, 'tiles-lzss', 'verb bar / UI font tiles -> VRAM after room tiles'),
    (0x031AFA, 0x031B7A, 'tiles-raw', 'UI tiles (4) loaded after the verb-bar font'),
    (0x031B7A, 0x031CFA, 'tiles-raw', 'UI tiles (12)'),
    (0x031CFA, 0x031DFA, 'tiles-raw', 'UI tiles (8)'),
    (0x032480, 0x0324C0, 'palette', 'CRAM palettes 0-1 (UI + characters), shared by all rooms'),
]

TITLE_EXTRA = [
    (0x017064, 0x0170D4, 'ptr-table', 'title plane-A animation: 28 longs -> RLE name frames'),
    (0x0085F8, 0x008660, 'sat', 'title sprite attribute table (raw, -> VRAM $FC00)'),
]


def asset_list():
    r = rom()
    out = []

    def lz(a, kind, desc, prefix=0):
        end = lzss_decompress(r, a)[1]
        out.append((a - prefix, end, kind, desc))

    def rl(a, kind, desc):
        out.append((a, rle_decompress(r, a)[1], kind, desc))

    out.append((WORLD_TABLE, WORLD_TABLE + 8, 'ptr-table', 'mystery header pointers (index $FF06AA)'))
    for wi in (0, 1):
        w = World(wi)
        out.append((w.addr, w.addr + 0x34, 'header', f'world {wi} header (see docs/formats.md)'))
        out.append((w.rooms, w.rooms + 0x14 * w.nrooms, 'room-table', f'world {wi}: {w.nrooms} x 20-byte room entries'))
        out.append((w.palettes, w.palettes + 128 * w.nrooms, 'palette',
                    f'world {wi}: per-room 128 bytes; +0x40 = CRAM palettes 2-3'))
        for n in range(2, 2 + w.nrooms):
            rm = w.room(n)
            tag = f'world {wi} room {n} {rm.name!r}'
            lz(rm.tiles_addr, 'tiles-lzss', f'{tag} tiles -> VRAM $0000 (u32 size prefix)', 4)
            mp = w.maps_base + rm.maps_off
            out.append((mp, mp + 4, 'map-hdr', f'{tag} map pack header {rm.map_w}x{rm.map_h} cells'))
            for a, what in zip(rm.chunks, ('plane B names', 'plane A names', 'walk/zone map')):
                lz(a, 'map-lzss', f'{tag} {what} (u32 size prefix)', 4)
    for name, s in SCREENS.items():
        for op in s['ops']:
            k = op[0]
            if k == 'lzss':
                lz(op[1], 'tiles-lzss', f'{name} screen tiles -> VRAM ${op[2]:04X}')
            elif k == 'lzssrows':
                lz(op[1], 'map-lzss', f'{name} screen names {op[2]}x{op[3]} -> VRAM ${op[4]:04X}')
            elif k == 'rle':
                rl(op[1], 'tiles-rle', f'{name} screen tiles -> VRAM ${op[2]:04X}')
            elif k == 'raw':
                kind = 'map-raw' if op[3] >= 0xC000 else 'tiles-raw'
                out.append((op[1], op[1] + op[2], kind, f'{name} screen -> VRAM ${op[3]:04X}'))
            elif k == 'rows':
                _, src, words, rows, dst, _st = op
                out.append((src, src + 2 * words * rows, 'map-raw', f'{name} screen names {words}x{rows} -> VRAM ${dst:04X}'))
            elif k == 'cram' and len(op) == 3:   # partial loads reuse UI_PALETTES
                out.append((op[1], op[1] + 2 * op[2], 'palette', f'{name} screen CRAM ({op[2]} words)'))
            elif k == 'rleptr':
                for i in range(28):
                    rl(u32(r, op[1] + 4 * i), 'map-rle', f'{name} plane-A frame {i} (32x28 names) -> VRAM ${op[2]:04X}')
    out += [(a, b if b else lzss_decompress(r, a)[1], k, d) for a, b, k, d in UI_ASSETS + TITLE_EXTRA]
    # the Illusions animation frames and the companion stream decompressed with them
    lz(0x028220, 'map-lzss', 'illusions logo animation: 15 frames x 20x5 names (-> $FF4000, DMA to VRAM $C410)')
    # dedupe (shared screens/rooms) and sort
    seen = {}
    for a, b, k, d in out:
        seen.setdefault((a, b, k), d)
    return sorted((a, b, k, d) for (a, b, k), d in seen.items())


# ---------------------------------------------------------------------------
# CLI


def parse_int(s):
    return int(s, 0) if not s.startswith('$') else int(s[1:], 16)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('lzss'); p.add_argument('addr'); p.add_argument('out')
    p = sub.add_parser('tiles'); p.add_argument('src'); p.add_argument('out')
    p.add_argument('--pal'); p.add_argument('--cols', type=int, default=16)
    p.add_argument('--scale', type=int, default=1)
    p = sub.add_parser('pal'); p.add_argument('addr'); p.add_argument('out')
    p.add_argument('--n', type=int, default=16)
    p = sub.add_parser('room'); p.add_argument('n', type=int); p.add_argument('out')
    p.add_argument('--world', type=int, default=0)
    p.add_argument('--layer', choices=['a', 'b', 'both'], default='both')
    p.add_argument('--view', type=int, metavar='CAMX',
                   help='crop to the 256-px viewport at camera x (Lobby start: 160)')
    p.add_argument('--scale', type=int, default=1)
    p = sub.add_parser('rooms'); p.add_argument('--world', type=int, default=0)
    p.add_argument('--png', metavar='DIR', help='also render every room to DIR/w<world>_r<n>.png')
    p = sub.add_parser('vram'); p.add_argument('vram'); p.add_argument('cram'); p.add_argument('out')
    p.add_argument('--plane', default='0xc000'); p.add_argument('--w', type=int, default=64)
    p.add_argument('--h', type=int, default=32)
    sub.add_parser('assets', help='print the asset map (data/assets.txt)')
    p = sub.add_parser('screen'); p.add_argument('name', choices=sorted(SCREENS)); p.add_argument('out')
    p.add_argument('--frame', type=int, help='title: plane-A animation frame 0..27 (default 27)')
    p.add_argument('--scale', type=int, default=1)
    p.add_argument('--dump', metavar='PREFIX', help='also write PREFIX.vram / PREFIX.cram')
    args = ap.parse_args(argv)
    r = rom()

    if args.cmd == 'lzss':
        d, end = lzss_decompress(r, parse_int(args.addr))
        with open(args.out, 'wb') as f:
            f.write(d)
        print(f'{len(d):#x} bytes, stream ends at {end:#08x}')
    elif args.cmd == 'tiles':
        if args.src.startswith('raw:'):
            _, a, n = args.src.split(':')
            data = r[parse_int(a):parse_int(a) + parse_int(n)]
        else:
            data = lzss_decompress(r, parse_int(args.src))[0]
        pal = palette(r, parse_int(args.pal)) if args.pal else \
            [(v * 17, v * 17, v * 17) for v in range(16)]
        write_png(args.out, render_tiles(data, pal, args.cols), args.scale)
        print(f'{len(data) // 32} tiles')
    elif args.cmd == 'pal':
        cols = palette(r, parse_int(args.addr), args.n)
        img = [[cols[x // 16] for x in range(16 * len(cols))] for _ in range(16)]
        write_png(args.out, img)
    elif args.cmd == 'rooms':
        w = World(args.world)
        print(f'world {args.world} header {w.addr:#08x}: {w.nrooms} rooms')
        for n in range(2, 2 + w.nrooms):
            rm = w.room(n)
            print(f'  room {n:2d} {rm.name!r:28} {rm.map_w:3d}x{rm.map_h:<3d} tiles {rm.tiles_addr:#08x} '
                  f'maps {rm.chunks[0]:#08x}/{rm.chunks[1]:#08x}/{rm.chunks[2]:#08x} pal {rm.pal_addr:#08x}')
            if args.png:
                write_png(os.path.join(args.png, f'w{args.world}_r{n:02d}.png'), render_room(rm))
    elif args.cmd == 'room':
        rm = World(args.world).room(args.n)
        crop = (args.view, 256) if args.view is not None else None
        write_png(args.out, render_room(rm, args.layer, crop), args.scale)
        print(f'room {args.n} {rm.name!r} -> {args.out}')
    elif args.cmd == 'vram':
        vram = open(args.vram, 'rb').read()
        cram = open(args.cram, 'rb').read()
        write_png(args.out, render_vram(vram, cram, parse_int(args.plane), args.w, args.h))
    elif args.cmd == 'assets':
        print('# ROM asset map generated by `tools/py/gfx.py assets` (start end kind description;')
        print('# hex, end exclusive). Kinds: tiles-lzss/tiles-rle/tiles-raw (4bpp tiles),')
        print('# map-* (VDP nametable words), palette (CRAM words), header/room-table/ptr-table.')
        print('# LZSS/RLE ends are where the decompressor stops consuming input.')
        for a, b, k, d in asset_list():
            print(f'{a:06x} {b:06x} {k:11} {d}')
    elif args.cmd == 'screen':
        vram, cram, s = build_screen(args.name, args.frame)
        if args.dump:
            open(args.dump + '.vram', 'wb').write(vram)
            open(args.dump + '.cram', 'wb').write(cram)
        write_png(args.out, compose(vram, cram, pw=s['pw'], sw=s['sw']), args.scale)


if __name__ == '__main__':
    main()
