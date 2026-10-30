#!/usr/bin/env python3
"""Compare a gfx.py reconstruction PNG against a harness screenshot (PPM).

  cmpshot.py RECON.png SHOT.ppm --y0 16 --x0 0 [--diff OUT.png]
  cmpshot.py A.ppm B.ppm [--diff OUT.png]     exact comparison of two emulator screenshots

Both images are reduced to Genesis 3-bit-per-channel levels before comparing
(GPGX's RGB565 output and gfx.py's 8-bit levels differ only in scaling).
Reports the fraction of identical pixels in the overlapping region; mismatches
are typically sprites (characters, animated objects), which the background
reconstruction deliberately omits.
"""
import argparse
import struct
import sys
import zlib
import os
sys.path.insert(0, os.path.dirname(__file__))
import gfx  # noqa: E402


def read_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    p = 8
    idat = b''
    while p < len(d):
        n, tag = struct.unpack_from('>I4s', d, p)
        body = d[p + 8:p + 8 + n]
        if tag == b'IHDR':
            w, h, depth, ctype = struct.unpack_from('>IIBB', body)
            assert depth == 8 and ctype == 2, 'only 8-bit RGB PNGs written by gfx.py'
        elif tag == b'IDAT':
            idat += body
        p += 12 + n
    raw = zlib.decompress(idat)
    img = []
    stride = 3 * w
    for y in range(h):
        line = raw[y * (stride + 1):(y + 1) * (stride + 1)]
        assert line[0] == 0, 'filtered PNG rows not supported'
        img.append([tuple(line[1 + 3 * x:4 + 3 * x]) for x in range(w)])
    return img


def read_ppm(path):
    d = open(path, 'rb').read()
    # harness writes exactly "P6\n<w> <h>\n255\n"
    magic, dims, maxv, px = d.split(b'\n', 3)
    w, h = map(int, dims.split())
    return [[tuple(px[3 * (y * w + x):3 * (y * w + x) + 3]) for x in range(w)] for y in range(h)]


def level(v, table):
    return min(range(8), key=lambda i: abs(table[i] - v))


# GPGX RGB565 output levels (r/b: 5-bit << 3, g: 6-bit << 2), measured
EMU_RB = [0, 32, 64, 96, 136, 168, 200, 232]
EMU_G = [0, 36, 68, 100, 136, 168, 204, 236]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('recon'); ap.add_argument('shot')
    ap.add_argument('--x0', type=int, default=0); ap.add_argument('--y0', type=int, default=0)
    ap.add_argument('--diff')
    a = ap.parse_args()
    shot = read_ppm(a.shot)
    if a.recon.endswith('.ppm'):
        # emulator screenshot vs emulator screenshot (e.g. port/ host vs tools/harness)
        ref = read_ppm(a.recon)
        h, w = len(ref), len(ref[0])
        if (h, w) != (len(shot), len(shot[0])):
            print(f'size differs: {w}x{h} vs {len(shot[0])}x{len(shot)}')
            sys.exit(1)
        diffs = [(x, y) for y in range(h) for x in range(w) if ref[y][x] != shot[y][x]]
        print(f'{w * h - len(diffs)}/{w * h} pixels identical ({100.0 * (w * h - len(diffs)) / (w * h):.2f}%)')
        if diffs:
            xs = [d[0] for d in diffs]
            ys = [d[1] for d in diffs]
            print(f'differences within x {min(xs)}-{max(xs)}, y {min(ys)}-{max(ys)}')
        if a.diff:
            gfx.write_png(a.diff, [[(255, 0, 255) if ref[y][x] != shot[y][x] else tuple(c // 3 for c in ref[y][x])
                                    for x in range(w)] for y in range(h)])
        sys.exit(1 if diffs else 0)
    rec = read_png(a.recon)
    same = tot = 0
    diff = []
    for y in range(len(rec)):
        sy = y + a.y0
        if sy >= len(shot):
            break
        row = []
        for x in range(len(rec[0])):
            sx = x + a.x0
            if sx >= len(shot[0]):
                break
            r = tuple(level(c, gfx.LEVELS) for c in rec[y][x])
            s = shot[sy][sx]
            s = (level(s[0], EMU_RB), level(s[1], EMU_G), level(s[2], EMU_RB))
            tot += 1
            if r == s:
                same += 1
                row.append(tuple(c // 3 for c in rec[y][x]))
            else:
                row.append((255, 0, 255))
        diff.append(row)
    print(f'{same}/{tot} pixels identical ({100.0 * same / tot:.2f}%)')
    if a.diff:
        gfx.write_png(a.diff, diff)


if __name__ == '__main__':
    main()
