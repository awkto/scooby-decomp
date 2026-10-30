#!/usr/bin/env python3
"""Walkable-area ("zone map") decoder for Scooby-Doo Mystery rooms.

The third LZSS chunk of a room's map pack (RAM $FFE000, one word per 8x8 cell) is a
*collision name table*: like a VDP name table, it points at 4bpp 8x8 "collision tiles"
instead of graphics.  See docs/subsystems/walking.md for the full description; in short:

    zone word  z = map[(y>>3)*width + (x>>3)]
      bit 15        actor is drawn behind the foreground at this cell ($FF0ABB bit n)
      bits 0-14     index into COLL_NAMES ($02AF8E, 150 words); 0 = free cell
    COLL_NAMES[i]   bits 0-10 collision tile ($02B0BA, 32 bytes each), bit 11 hflip, bit 12 vflip
    pixel (x&7, y&7) of that tile: 0 = walkable, any other nibble = blocked

Pixel coordinates are room ("world") pixels, the same as Shaggy's position words
$FF04DC/$FF04F4 (integer part).  Outside 0..w*8-1 / 0..h*8-1 is blocked.

usage:
  walkmap.py stats [--world N]                        zone-word statistics of every room
  walkmap.py dump ROOM [--world N]                    cell map + room walk data as text
  walkmap.py png ROOM OUT [--world N] [--scale S] [--points FILE] [--view CAMX]
        room background with the blocked pixels tinted red, bit-15 cells tinted blue,
        Scooby's idle spots (yellow), spawn points (green), path waypoints (magenta);
        --points: text file of "x y" world pixels (e.g. positions observed in the
        emulator) drawn as white dots (cyan if the point is on a blocked pixel).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gfx  # noqa: E402

COLL_NAMES = 0x02AF8E      # 150 words
COLL_NAMES_N = 0x12C // 2
COLL_TILES = 0x02B0BA      # 20 tiles x 32 bytes
COLL_TILES_N = 0x280 // 32


class WalkMap:
    """Collision data of one room, decoded from ROM exactly as sub_0073D6 reads it."""

    def __init__(self, world, n):
        r = gfx.rom()
        self.room = gfx.World(world).room(n)
        self.w, self.h = self.room.map_w, self.room.map_h
        self.zone = self.room.maps()[2]
        self.r = r
        # room walk data after the zone map chunk ($FF0692 = room_data_ptr)
        d = self.room.after_maps
        self.depth_rows = [gfx.u16(r, d + 2 * i) for i in range(self.h)]  # $FF0692
        p = d + 2 * self.h                                                   # $FF0696
        self.spawns = [(gfx.u16(r, p + 4 * i), gfx.u16(r, p + 4 * i + 2)) for i in range(8)]
        q = p + 0x20                                                         # $FF069A
        self.idle_spots = []
        for i in range(8):
            if gfx.u32(r, q + 4 * i) == 0xFFFFFFFF:
                break
            self.idle_spots.append((gfx.u16(r, q + 4 * i), gfx.u16(r, q + 4 * i + 2)))
        wp = q + 0x20
        self.waypoints = [(gfx.u16(r, wp), gfx.u16(r, wp + 2)),
                          (gfx.u16(r, wp + 4), gfx.u16(r, wp + 6))]

    def zone_at(self, x, y):
        return self.zone[(y >> 3) * self.w + (x >> 3)]

    def blocked(self, x, y, mask=0x7FFF):
        """sub_0073D6 (mask $7FFF, D-pad walking) / sub_007476 (mask $7FF, auto-walk)."""
        if x < 0 or y < 0 or x > self.w * 8 - 1 or y > self.h * 8 - 1:
            return True
        z = self.zone_at(x, y) & mask
        if z == 0:
            return False
        name = gfx.u16(self.r, COLL_NAMES + 2 * z)
        tile = COLL_TILES + (name & 0x7FF) * 32
        px, py = x & 7, y & 7
        if name & 0x800:          # hflip
            px ^= 7
        if name & 0x1000:         # vflip
            py ^= 7
        b = self.r[tile + py * 4 + (px >> 1)]
        nib = (b >> 4) if (px & 1) == 0 else (b & 15)
        return nib != 0

    def depth(self, y):
        """Per-actor depth scale computed by sub_006D24 for an actor at world y (0 = nearest).
        Walking speed is (256 - depth) * $155 >> 8 (x) and * $AA >> 8 (y) per frame (8.8)."""
        row = y >> 3
        # previous defined row: a do-while over rows 0..row-1 (row 0 is consumed even
        # when the actor is on row 0); without one the 68k uses the stale D4 = width*2
        prev_v, prev_r = 0, self.w * 2
        i = 0
        while True:
            v = self.depth_rows[i]
            if v < 0x8000:
                prev_v, prev_r = v, i
            i += 1
            if i >= row:
                break
        nxt_v, nxt_r = 0, self.h
        while i < self.h:
            v = self.depth_rows[i]
            if v < 0x8000:
                nxt_v, nxt_r = v, i
                break
            i += 1
        d0 = nxt_r * 8 + 7 - y
        span = (nxt_r - prev_r) * 8
        num = ((nxt_v - prev_v) << 4) * d0
        q = int(num / span) if span else 0          # divs truncates toward zero
        return (nxt_v << 4) - q


def cmd_stats(world):
    w = gfx.World(world)
    names = set()
    for n in range(2, 2 + w.nrooms):
        m = WalkMap(world, n)
        vals = set(m.zone)
        hi = sorted({v & 0x7800 for v in vals} - {0})
        b15 = sum(1 for v in m.zone if v & 0x8000)
        names |= {v & 0x7FFF for v in vals}
        free = sum(1 for y in range(m.h * 8) for x in range(m.w * 8) if not m.blocked(x, y))
        print(f'room {n:2d} {m.room.name!r:32} {m.w:3d}x{m.h:<3d} max={max(v & 0x7FFF for v in vals):3d} '
              f'bit15 cells={b15:4d} bits11-14={hi} walkable px={free} '
              f'idle={len(m.idle_spots)} wp={m.waypoints}')
    print('zone indices used:', sorted(names))


def cmd_dump(world, n):
    m = WalkMap(world, n)
    print(f'{m.room.name} ({m.w}x{m.h} cells)')
    for y in range(m.h):
        row = ''
        for x in range(m.w):
            v = m.zone[y * m.w + x]
            c = '.' if (v & 0x7FFF) == 0 else chr(0x30 + (v & 0x7FFF) % 75)
            row += c.upper() if v & 0x8000 else c
        print(f'{y:2d} {row}  depth={m.depth_rows[y] if m.depth_rows[y] < 0x8000 else "-"}')
    print('spawn points (cells):', m.spawns)
    print('scooby idle spots (cells, top nibble = flags):',
          [(f'{x:04x}', y) for x, y in m.idle_spots])
    print('waypoints (cells):', m.waypoints)


def cmd_png(world, n, out, scale, points, view):
    m = WalkMap(world, n)
    img = gfx.render_room(m.room)
    H, W = len(img), len(img[0])

    def tint(c, t, a):
        return tuple(int(c[i] * (1 - a) + t[i] * a) for i in range(3))

    for y in range(H):
        for x in range(W):
            z = m.zone_at(x, y)
            if m.blocked(x, y):
                img[y][x] = tint(img[y][x], (255, 0, 0), 0.55)
            elif z & 0x8000:
                img[y][x] = tint(img[y][x], (0, 80, 255), 0.45)

    def dot(x, y, col, r=1):
        for yy in range(y - r, y + r + 1):
            for xx in range(x - r, x + r + 1):
                if 0 <= xx < W and 0 <= yy < H:
                    img[yy][xx] = col

    for cx, cy in m.spawns:
        if (cx, cy) != (0, 0):
            dot(cx * 8, cy * 8, (0, 255, 0), 2)
    for cx, cy in m.idle_spots:
        dot((cx & 0xFFF) * 8, cy * 8, (255, 255, 0), 2)
    for cx, cy in m.waypoints:
        if (cx, cy) != (0, 0):
            dot(cx * 8, cy * 8, (255, 0, 255), 2)
    if points:
        for line in open(points):
            f = line.split()
            if len(f) >= 2:
                x, y = int(f[0], 0), int(f[1], 0)
                dot(x, y, (0, 255, 255) if m.blocked(x, y) else (255, 255, 255), 0)
    if view is not None:
        img = [row[view:view + 256] for row in img]
    gfx.write_png(out, img, scale)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('stats'); p.add_argument('--world', type=int, default=0)
    p = sub.add_parser('dump'); p.add_argument('room', type=int); p.add_argument('--world', type=int, default=0)
    p = sub.add_parser('png'); p.add_argument('room', type=int); p.add_argument('out')
    p.add_argument('--world', type=int, default=0); p.add_argument('--scale', type=int, default=2)
    p.add_argument('--points'); p.add_argument('--view', type=int)
    a = ap.parse_args(argv)
    if a.cmd == 'stats':
        cmd_stats(a.world)
    elif a.cmd == 'dump':
        cmd_dump(a.world, a.room)
    else:
        cmd_png(a.world, a.room, a.out, a.scale, a.points, a.view)


if __name__ == '__main__':
    main()
