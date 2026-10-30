"""List the LZSS / RLE streams of the ROM as difftest --poke variants (used by port/tools/dt-decomp).
usage: decomp_streams.py lzss|rle   -> one line of '--poke' arguments per stream"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import gfx

kind = sys.argv[1]
want = ('tiles-lzss', 'map-lzss') if kind == 'lzss' else ('tiles-rle', 'map-rle')
seen = []
for a, b, k, d in gfx.asset_list():
    if k in want:
        if 'u32 size prefix' in d:
            a += 4          # the loader skips the u32 packed length
        seen.append((a, b - a))
if kind == 'lzss':
    seen.sort(key=lambda s: -s[1])   # long streams first
n = 0
for i, (a, ln) in enumerate(seen):
    outs = ['FF4000', 'FF4001', 'FF7000'] if kind == 'lzss' else ['FF4000', 'FF4001', 'FF6000']
    o = outs[i % 3]
    extra = ''
    if kind == 'lzss' and i % 4 == 3:
        extra = ',A1=0xFF3400'      # ring elsewhere
    if kind == 'rle':
        extra = ',B:FF09ED=%d' % (8 if i % 2 == 0 else 0)
    print('--poke "A0=0x%X,A2=0x%s%s"' % (a, o, extra))
