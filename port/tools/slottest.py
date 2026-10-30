#!/usr/bin/env python3
"""Game-save gate (GitLab #28).  For each route: replay it once, storing the running game in the 8 save slots at
8 points along the way (script command `slot N`).  Then for every slot, compare

    A. the game loaded from the slot            (scoobyport --load-slot N: the loader presses the game's own buttons)
    B. the password typed in by hand, key by key (a harness script generated here from the slot's password,
                                                 walking the 4x8 key grid with the D-pad exactly like a player)

after the same settling time: mystery, room, inventory and the 29 story bytes ($FF2A00) must be identical.
    port/tools/slottest.py [ROUTE...]       (run on a VM from the repo root, after building build/port)
default routes: w0_full w1_full (tools/scripts/story/ROUTE.txt).  SLOTTEST_POINTS=n limits the slots tested per route."""
import os, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.environ.get("SCOOBY_BIN") or os.path.join(ROOT, "build/port/scoobyport")
ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!+#%*?"
OUT = os.path.join(ROOT, os.environ.get("SLOTTEST_OUT", "runs/slottest"))
ENV = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")


def run(args, **kw):
    return subprocess.run([BIN] + args, env=ENV, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)


def read_slot(d, n):
    p = os.path.join(d, f"slot{n}.sav")
    if not os.path.exists(p):
        return None
    return dict(l.strip().split("=", 1) for l in open(p) if "=" in l)


def typed_script(mystery, pw):
    """harness-format script: title -> mystery -> CONTINUE -> type pw with the D-pad + A -> END"""
    L = ["run 1500"] + ["press S 6", "run 150"] * 3
    if mystery:
        L += ["press D 6", "run 20"]
    L += ["press S 6", "run 150", "press D 6", "run 20", "press S 6", "run 300"]
    r = c = 0

    def goto(tr, tc):
        nonlocal r, c
        while r != tr:
            fwd = (tr - r) % 5
            L.extend(["press D 6" if fwd <= 2 else "press U 6", "run 8"])
            r = (r + (1 if fwd <= 2 else -1)) % 5
        while c != tc:
            fwd = (tc - c) % 8
            L.extend(["press R 6" if fwd <= 4 else "press L 6", "run 8"])
            c = (c + (1 if fwd <= 4 else -1)) % 8
    for ch in pw:
        i = ALPHA.index(ch)
        goto(i // 8, i % 8)
        L.extend(["press A 6", "run 10"])
    goto(4, 5)
    L.append("press A 6")
    return L


def state(path):
    r = open(path, "rb").read()
    w = lambda a: struct.unpack(">H", r[a & 0xFFFF:(a & 0xFFFF) + 2])[0]
    inv = []
    for i in range(32):
        v = struct.unpack(">h", r[0x0A1E + 2 * i:0x0A20 + 2 * i])[0]
        if v < 0:
            break
        inv.append(v)
    return dict(mystery=w(0xFF06AA), room=w(0xFF06AC), inv=inv, story=r[0x2A00:0x2A00 + 29].hex())


def main():
    routes = sys.argv[1:] or ["w0_full", "w1_full"]
    npts = int(os.environ.get("SLOTTEST_POINTS", "8"))
    fail = 0
    for route in routes:
        d = os.path.join(OUT, route)
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):
            os.remove(os.path.join(d, f))
        lines = [l.rstrip("\n") for l in open(os.path.join(ROOT, "tools/scripts/story", route + ".txt"))]
        cmd = [i for i, l in enumerate(lines) if l.split()[:1] in (["run"], ["press"])]
        marks = {cmd[int(len(cmd) * (0.15 + 0.85 * k / npts))]: k + 1 for k in range(npts)}
        out = []
        for i, l in enumerate(lines):
            out.append(l)
            if i in marks:
                out.append(f"slot {marks[i]}")
        open(os.path.join(d, "route.txt"), "w").write("\n".join(out) + "\n")
        res = run(["--savedir", d, "--outdir", d, "--script", os.path.join(d, "route.txt"), "--frames", "0"])
        print(f"{route}: route replayed ({len(lines)} lines)")
        for n in range(1, npts + 1):
            sl = read_slot(d, n)
            if not sl:
                print(f"  slot {n}: not stored (not in a mystery at that point?)")
                fail += 1
                continue
            pw = sl["password"]
            open(os.path.join(d, f"typed{n}.txt"), "w").write("\n".join(typed_script(int(sl["mystery"]), pw) + ["run 1750"]) + "\n")
            run(["--savedir", d, "--outdir", d, "--script", os.path.join(d, f"typed{n}.txt"), "--frames", "0", "--dump-ram", os.path.join(d, f"typed{n}.ram")])
            lr = run(["--savedir", d, "--load-slot", str(n), "--frames", "1600", "--dump-ram", os.path.join(d, f"load{n}.ram")])
            try:
                a, b = state(os.path.join(d, f"load{n}.ram")), state(os.path.join(d, f"typed{n}.ram"))
            except OSError as e:
                print(f"  slot {n}: FAIL ({e}) {lr.stdout[-200:]}")
                fail += 1
                continue
            ok = a == b and a["room"] == int(sl["room"]) and a["mystery"] == int(sl["mystery"])
            print(f"  slot {n}: {'PASS' if ok else 'FAIL'}  myst={a['mystery']} room={a['room']:2d} ({sl['roomname']}) inv={len(a['inv'])} items  story={a['story']}")
            if not ok:
                print("     loaded:", a, "\n     typed: ", b)
                fail += 1
    print("slottest:", "FAIL" if fail else "PASS")
    return 1 if fail else 0


sys.exit(main())
