#!/usr/bin/env python3
"""verify.py ROUTE [OUTDIR]: replay a flat route with the batch harness; print room/story/inventory at the end."""
import os, struct, subprocess, sys
route = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else "runs/verify"
os.makedirs(out, exist_ok=True)
sp = os.path.join(out, "script.txt")
with open(sp, "w") as f:
    f.write(open(route).read())
    f.write("\nrun 5\nram fin\nshot fin\n")
subprocess.run(["build/harness", "ext/gpgx/genesis_plus_gx_libretro.so", "rom/baserom.md", sp, out], capture_output=True)
r = open(os.path.join(out, "fin.ram"), "rb").read()
w = lambda a: struct.unpack(">H", r[a & 0xFFFF:(a & 0xFFFF) + 2])[0]
inv = []
for i in range(32):
    v = struct.unpack(">h", r[0xA1E + 2 * i:0xA20 + 2 * i])[0]
    if v < 0:
        break
    inv.append(v)
print("myst", w(0xFF06AA), "room", w(0xFF06AC), "pos", (w(0xFF04DC), w(0xFF04F4)), "inv", inv)
print("story", r[0x2A00:0x2A00 + 29].hex())
