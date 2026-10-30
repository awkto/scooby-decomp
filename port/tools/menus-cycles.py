#!/usr/bin/env python3
"""Compare the cycles the C menu code is charged with what the original 68k code costs (#21).

Run the tours with SCOOBY_HLE_TRACE=1 and --hle all; this script averages the "+N charged" of every
dispatched menu function and prints it next to the original's average from the difftest logs
(runs/dt-menus/*.log, "68k cycles min/avg/max").   Usage on a VM after `port/tools/dt-menus`:

    port/tools/menus-cycles.py
"""
import os, re, subprocess, sys, collections

root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.chdir(root)
env = dict(os.environ, SCOOBY_HLE_TRACE="1", SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
args = ["build/port/scoobyport", "--hle", "all", "--frames", "0", "--outdir", "runs/cyc"]
for n in (1, 2, 3):
    args += ["--script", f"port/scripts/menus/tour{n}.txt"]
os.makedirs("runs/cyc", exist_ok=True)
p = subprocess.run(args, env=env, capture_output=True, text=True, timeout=1800)
tot = collections.defaultdict(lambda: [0, 0])
for line in p.stderr.splitlines():
    m = re.match(r"hle (\S+)\s.*\(\+(\d+) charged\)", line)
    if m:
        t = tot[m.group(1)]
        t[0] += 1
        t[1] += int(m.group(2)) / 7      # the trace prints master-clock cycles (68k cycles * 7)
orig = {}
for f in os.listdir("runs/dt-menus"):
    s = open(os.path.join("runs/dt-menus", f)).read()
    m = re.search(r"^(\S+) \$\w+: .*68k cycles min/avg/max (\d+)/(\d+)/(\d+)", s, re.M)
    if m:
        orig[m.group(1)] = int(m.group(3))
print(f"{'function':28} {'calls':>7} {'charged avg':>12} {'orig avg':>10} {'ratio':>6}")
for name in sorted(orig):
    if name in tot:
        n, c = tot[name]
        a = c / n
        print(f"{name:28} {n:7d} {a:12.0f} {orig[name]:10d} {a / max(orig[name], 1):6.2f}")
    else:
        print(f"{name:28} {'-':>7} {'-':>12} {orig[name]:10d}")
