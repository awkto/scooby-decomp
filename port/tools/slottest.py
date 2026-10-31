#!/usr/bin/env python3
"""Game-save gate (GitLab #28, #35).  For each route: replay it once (--hle none), storing the running game

  * in the 8 numbered save slots at 8 points along the way (script command `slot N`), and
  * in an extra save file every time the game has been in a new room for 60 frames (`autoslot`).

Then load every save the way the player's F8 menu does (scoobyport `loadslot`: the loader presses the game's
own buttons) and require

  1. progress: after the load and the mystery's start-up scripts the game is interactive -- no script running
     (ram_vblank_flags bit 7 clear), verb bar shown (story byte 0 bit 1 clear), and the D-pad walks Shaggy
     (his position changes for one of R/L/D/U; in a room whose script froze him, C + D-pad moves the
     pointer).  A hang (e.g. the bus error loop at $892, #35) freezes RAM and fails here.
  2. for the 8 numbered slots: the loaded game equals the same password typed in by hand, key by key (a
     harness script walking the 4x8 key grid with the D-pad exactly like a player): mystery, room, inventory
     and the 29 story bytes ($FF2A00) after the same settling time.  The typed run is a "warm" console: its
     script pokes ram_script_cb = script_run_frame first, as a game played since power-on leaves it (the
     cold-start cartridge bug of #35, docs/passwords.md; the loader does the same).

    port/tools/slottest.py [ROUTE...]       (run on a VM from the repo root, after building build/port)
default routes: w0_full w1_full (tools/scripts/story/ROUTE.txt).
SLOTTEST_POINTS=n   numbered slots per route (default 8)      SLOTTEST_ROOMS=0  skip the room-change saves
SLOTTEST_JOBS=n     parallel runs (default: CPU count)        SLOTTEST_HLE      replay mode (default none)"""
import concurrent.futures as cf
import os, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# SCOOBY_BIN may be a command with arguments: "port/tools/nativewrap.sh scoobyport" replays the route (--hle none)
# on the emulated build and loads the slots on the native build (#24)
BIN = (os.environ.get("SCOOBY_BIN") or os.path.join(ROOT, "build/port/scoobyport")).split()
ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!+#%*?"
OUT = os.path.join(ROOT, os.environ.get("SLOTTEST_OUT", "runs/slottest"))
ENV = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
SETTLE = 1600          # frames after the load before the state comparison (typed: 1750 after END)
INTERACT = 1400        # further frames until the interactive check
WARM = "poke FF0878 4 2406"   # ram_script_cb = script_run_frame (#35)
WALKS = ["R", "L", "D", "U"]


def run(args, **kw):
    return subprocess.run(BIN + args, env=ENV, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)


def read_slot(p):
    if not os.path.exists(p):
        return None
    return dict(l.strip().split("=", 1) for l in open(p) if "=" in l)


def typed_script(mystery, pw):
    """harness-format script: title -> mystery -> CONTINUE -> type pw with the D-pad + A -> END"""
    L = ["run 1500", WARM] + ["press S 6", "run 150"] * 3
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


def ram(path):
    return open(path, "rb").read()


def w16(r, a):
    return struct.unpack(">H", r[a & 0xFFFF:(a & 0xFFFF) + 2])[0]


def state(r):
    inv = []
    for i in range(32):
        v = struct.unpack(">h", r[0x0A1E + 2 * i:0x0A20 + 2 * i])[0]
        if v < 0:
            break
        inv.append(v)
    return dict(mystery=w16(r, 0xFF06AA), room=w16(r, 0xFF06AC), inv=inv, story=r[0x2A00:0x2A00 + 29].hex())


def shaggy(r):
    return (w16(r, 0xFF04DC), w16(r, 0xFF04F4), w16(r, 0xFF06AC))


def pointer(r):
    return (w16(r, 0xFF06DC), w16(r, 0xFF06DE))


def load_and_play(d, name):
    """load save d/name.sav, settle, check it is interactive and Shaggy walks.  -> (state, problems, output)"""
    L = [f"loadslot {name}", f"run {SETTLE}", f"ram {name}_load", f"run {INTERACT}", f"ram {name}_pre"]
    for k in WALKS:
        L += [f"press {k} 40", "run 10", f"ram {name}_{k}"]
    # pointer mode (C), then move the crosshair: the check for rooms whose script freezes Shaggy (close-ups)
    L += ["press C 6", "run 20", f"ram {name}_c0", "press R 30", "press D 30", "run 5", f"ram {name}_c1", f"shot {name}_end"]
    sp = os.path.join(d, f"{name}_play.txt")
    open(sp, "w").write("\n".join(L) + "\n")
    for f in [f"{name}_load", f"{name}_pre", f"{name}_c0", f"{name}_c1"] + [f"{name}_{k}" for k in WALKS]:
        if os.path.exists(os.path.join(d, f + ".ram")):
            os.remove(os.path.join(d, f + ".ram"))
    res = run(["--savedir", d, "--outdir", d, "--script", sp, "--frames", "0"])
    probs = []
    if "LOADED SLOT" not in res.stdout:
        probs.append("loader did not confirm the password state")
    try:
        a = ram(os.path.join(d, f"{name}_load.ram"))
        pre = ram(os.path.join(d, f"{name}_pre.ram"))
        walks = [ram(os.path.join(d, f"{name}_{k}.ram")) for k in WALKS]
        c0, c1 = ram(os.path.join(d, f"{name}_c0.ram")), ram(os.path.join(d, f"{name}_c1.ram"))
    except OSError as e:
        return None, probs + [str(e)], res.stdout
    if pre[0x09DC] & 0x80:
        probs.append("a script is still running (ram_vblank_flags bit 7)")
    if pre[0x2A00] & 0x02:
        probs.append("verb bar hidden (story byte 0 bit 1)")
    if not pre[0x09ED] & 1:
        probs.append("no game running ($FF09ED bit 0)")
    if pre == walks[-1]:
        probs.append("RAM frozen (hang)")
    elif all(shaggy(w) == shaggy(pre) for w in walks):
        # a room script may freeze Shaggy (e.g. the Madame Zelda close-up): then the pointer must work
        frozen = pre[0x0ABF] & 1
        # the D-pad moves the crosshair if the room starts in pointer mode, else C switches to it first
        pointer_moves = any(pointer(w) != pointer(pre) for w in walks) or (c1[0x09DC] & 0x08 and pointer(c1) != pointer(c0))
        if not (frozen and pointer_moves):
            probs.append(f"Shaggy does not move (x,y,room {shaggy(pre)}, frozen {bool(frozen)}, pointer moves {bool(pointer_moves)})")
    return state(a), probs, res.stdout


def check_numbered(d, n, sl):
    pw = sl["password"]
    open(os.path.join(d, f"typed{n}.txt"), "w").write(
        "\n".join(typed_script(int(sl["mystery"]), pw) + ["run 1750", f"ram typed{n}"]) + "\n")
    run(["--savedir", d, "--outdir", d, "--script", os.path.join(d, f"typed{n}.txt"), "--frames", "0"])
    a, probs, out = load_and_play(d, f"slot{n}")
    try:
        b = state(ram(os.path.join(d, f"typed{n}.ram")))
    except OSError as e:
        return False, f"  slot {n}: FAIL typed run: {e}"
    if a is None:
        return False, f"  slot {n}: FAIL {'; '.join(probs)} {out[-200:]}"
    if a != b:
        probs.append(f"loaded != typed\n     loaded: {a}\n     typed:  {b}")
    if a["mystery"] != int(sl["mystery"]):
        probs.append("wrong mystery")
    ok = not probs
    return ok, (f"  slot {n}: {'PASS' if ok else 'FAIL'}  myst={a['mystery']} saved room {int(sl['room']):2d} ({sl['roomname']}), "
                f"room after settling {a['room']:2d}, {len(a['inv'])} items, story={a['story']}"
                + ("" if ok else "\n     " + "\n     ".join(probs)))


def check_room(d, name, sl, frame):
    a, probs, out = load_and_play(d, name)
    ok = a is not None and not probs
    return ok, (f"  {name} (frame {frame:6d}, room {int(sl['room']):2d} {sl['roomname']}): " + ("PASS" if ok else "FAIL " + "; ".join(probs)))


def main():
    routes = sys.argv[1:] or ["w0_full", "w1_full"]
    npts = int(os.environ.get("SLOTTEST_POINTS", "8"))
    rooms = os.environ.get("SLOTTEST_ROOMS", "1") != "0"
    jobs = int(os.environ.get("SLOTTEST_JOBS", str(os.cpu_count() or 2)))
    fail = total = 0
    for route in routes:
        d = os.path.join(OUT, route)
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):
            os.remove(os.path.join(d, f))
        lines = [l.rstrip("\n") for l in open(os.path.join(ROOT, "tools/scripts/story", route + ".txt"))]
        cmd = [i for i, l in enumerate(lines) if l.split()[:1] in (["run"], ["press"])]
        marks = {cmd[int(len(cmd) * (0.15 + 0.85 * k / npts))]: k + 1 for k in range(npts)}
        out = ["autoslot rc 60"] if rooms else []
        for i, l in enumerate(lines):
            out.append(l)
            if i in marks:
                out.append(f"slot {marks[i]}")
        open(os.path.join(d, "route.txt"), "w").write("\n".join(out) + "\n")
        # the committed routes are open-loop recordings made on the plain emulator: replay them on the pure 68k code
        # (--hle none, SLOTTEST_HLE to change) so they reach the same story points; the loaded / typed runs use the default C code
        run(["--hle", os.environ.get("SLOTTEST_HLE", "none"), "--savedir", d, "--outdir", d,
             "--script", os.path.join(d, "route.txt"), "--frames", "0"])
        rc = []
        if rooms and os.path.exists(os.path.join(d, "rc.idx")):
            rc = [l.split() for l in open(os.path.join(d, "rc.idx")) if l.strip()]
        print(f"{route}: route replayed ({len(lines)} lines), {npts} numbered saves, {len(rc)} room-change saves")
        with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
            futs = []
            for n in range(1, npts + 1):
                sl = read_slot(os.path.join(d, f"slot{n}.sav"))
                if not sl:
                    futs.append(None)
                    print(f"  slot {n}: FAIL not stored (not in a mystery at that point?)")
                    fail += 1
                    total += 1
                    continue
                futs.append(ex.submit(check_numbered, d, n, sl))
            for idx, frame, _room in rc:
                sl = read_slot(os.path.join(d, f"rc_{idx}.sav"))
                futs.append(ex.submit(check_room, d, f"rc_{idx}", sl, int(frame)))
            for f in futs:
                if f is None:
                    continue
                ok, msg = f.result()
                print(msg, flush=True)
                total += 1
                fail += not ok
    print(f"slottest: {total - fail}/{total} passed:", "FAIL" if fail else "PASS")
    return 1 if fail else 0


sys.exit(main())
