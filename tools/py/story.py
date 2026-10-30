#!/usr/bin/env python3
"""High-level driver for walkthrough routes (run on the VM; needs a running `session.py serve DIR`).

    from story import Game
    g = Game("runs/sess")
    g.boot(mystery=0)                 # title -> first room (flat harness commands, recorded)
    g.walk_to(120, 150)               # D-pad walk, confirmed by RAM peeks
    g.verb_on("Look", obj)            # pointer mode: pick verb, click object (pointer poked, B pressed)
    g.write_route("tools/scripts/story/x.txt")

Everything that changes emulator state (run / press / poke) is appended to `g.route`, so the
finished route is a flat harness script that replays from power-on (savestates are not bit-exact
over long runs, so routes never contain load/save).  Observation (ram dumps, shots, peeks) is not recorded.
"""
import os, struct, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
import session

VERBS = {"Pull": 1, "Take": 2, "Open": 3, "Shut": 4, "Use": 5, "Give": 6, "Push": 7, "Talk": 8, "Eat": 9, "Look": 10}
# verb bar cell centres (pixel of the crosshair top-left that selects the cell), verb view
VERB_CELL = {2: (0, 0), 10: (1, 0), 3: (2, 0), 7: (3, 0), 9: (4, 0), 6: (0, 1), 8: (1, 1), 4: (2, 1), 1: (3, 1), 5: (4, 1)}

# game-state block ($FF2A00) byte names are filled in docs/walkthrough.md as they are discovered


POINTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts", "story")


class Game:
    strict = False          # strict: no savestates, no scans (object points come from points.json); route stays replayable

    def __init__(self, d="runs/sess", strict=False):
        self.strict = strict
        import json
        self.points = {}
        for m in (0, 1):
            pp = os.path.join(POINTS_DIR, f"points_w{m}.json")
            if os.path.exists(pp):
                self.points.update(json.load(open(pp)))
        self.d = d
        self.rpath = os.path.join(d, "route.txt")
        self.route = open(self.rpath).read().split("\n")[:-1] if os.path.exists(self.rpath) else []
        self._n = 0

    # ---- raw ----
    def send(self, *cmds):
        return session.request(self.d, list(cmds))

    def do(self, *cmds):
        """state-changing commands: recorded in the route"""
        self.route.extend(cmds)
        with open(self.rpath, "a") as f:
            f.write("".join(c + "\n" for c in cmds))
        return self.send(*cmds)

    def note(self, *cmds):
        """record commands in the route that were already executed with send() (time that passed during an observation)"""
        self.route.extend(cmds)
        with open(self.rpath, "a") as f:
            f.write("".join(c + "\n" for c in cmds))

    def run(self, n):
        if n > 0:
            self.do(f"run {n}")

    def press(self, b, n=4, after=0):
        self.do(f"press {b} {n}")
        if after:
            self.run(after)

    def poke(self, addr, w, v):
        self.do(f"poke {addr:X} {w} {v:X}")

    def ram(self):
        self._n += 1
        name = f"r{self._n % 4}"
        self.send(f"ram {name}")
        return open(os.path.join(self.d, name + ".ram"), "rb").read()

    def w(self, addr, ram=None):
        r = ram or self.ram()
        a = addr & 0xFFFF
        return struct.unpack(">H", r[a:a + 2])[0]

    def b(self, addr, ram=None):
        r = ram or self.ram()
        return r[addr & 0xFFFF]

    def shot(self, name="s", scale=2):
        self.send(f"shot {name}")
        ppm = os.path.join(self.d, name + ".ppm")
        png = os.path.join(self.d, name + ".png")
        subprocess.run(["convert", ppm, "-scale", f"{scale*100}%", png], check=True)
        return png

    # ---- state ----
    def state(self):
        r = self.ram()
        return dict(
            myst=self.w(0xFF06AA, r), room=self.w(0xFF06AC, r),
            x=self.w(0xFF04DC, r), y=self.w(0xFF04F4, r),
            ptr=(self.w(0xFF06DC, r), self.w(0xFF06DE, r)), target=self.w(0xFF06AE, r),
            verb=self.w(0xFF06C0, r), flags=self.b(0xFF09DC, r), inv=self.inventory(r),
            story=r[0x2A00:0x2A00 + 29].hex())

    def inventory(self, r=None):
        r = r or self.ram()
        out = []
        for i in range(32):
            v = struct.unpack(">h", r[0x0A1E + 2 * i:0x0A20 + 2 * i])[0]
            if v < 0:
                break
            out.append(v)
        return out

    def story(self, r=None):
        r = r or self.ram()
        return bytes(r[0x2A00:0x2A00 + 29])

    def objects(self, r=None):
        """list of 26-byte object records in $FF1200 (count at $FF06F4)"""
        r = r or self.ram()
        n = self.w(0xFF06F4, r)
        out = []
        for i in range(n):
            rec = r[0x1200 + 26 * i:0x1200 + 26 * (i + 1)]
            out.append(struct.unpack(">13H", rec))
        return out

    # ---- boot ----
    def boot(self, mystery=0):
        """title -> first room of the mystery, as in tools/scripts/natural_w{0,1}.txt"""
        self.run(1500)
        for _ in range(3):
            self.press("S", 4, 150)
        if mystery:
            self.press("D", 4, 20)
        for _ in range(4):
            self.press("S", 4, 150)
        self.run(120)

    def run_script(self, path, record=True):
        """replay the state-changing lines (run/press/poke) of a harness script file"""
        cmds = [l.strip() for l in open(path) if l.split()[:1] and l.split()[0] in ("run", "press", "poke")]
        pmin = int(os.environ.get("STORY_PRESS_MIN", "0"))
        if pmin:
            cmds = [f"press {c.split()[1]} {max(int(c.split()[2]), pmin)}" if c.startswith("press ") else c for c in cmds]
        slack = int(os.environ.get("STORY_SLACK", "1"))      # >1: stretch the long waits (menu/title timing differs under --hle)
        if slack > 1:
            def stretch(c):
                w = c.split()
                if w[0] == "run":
                    return f"run {int(w[1]) * slack if int(w[1]) >= 100 else max(int(w[1]), 8)}"
                if w[0] == "press":
                    return f"press {w[1]} {max(int(w[2]), 8)}"
                return c
            cmds = [stretch(c) for c in cmds]
        for i in range(0, len(cmds), 400):
            (self.do if record else self.send)(*cmds[i:i + 400])

    def boot_password(self, mystery, password):
        """title -> CONTINUE -> type PASSWORD (50 chars) -> game, via the password screen.
        see tools/py/mkindex.py for the cursor-walking generator; this reuses its output"""
        raise NotImplementedError

    # ---- walking ----
    def pos(self, r=None):
        r = r or self.ram()
        return self.w(0xFF04DC, r), self.w(0xFF04F4, r)

    def walkon_rects(self, myst, room, exclude=(), r=None):
        """hotspot rectangles of the room's WALK_ON objects (exits...) except `exclude`: the planner steers around them"""
        import json, hotspots
        if not hasattr(Game, "_walkon"):
            Game._walkon = {m: set(json.load(open(os.path.join(POINTS_DIR, f"walkon_w{m}.json")))) for m in (0, 1)}
        r = r or self.ram()
        out = []
        for i, rec in enumerate(self.objects(r)):
            tid = i + 3
            if rec[3] == room and tid in Game._walkon[myst] and tid not in exclude:
                rc = hotspots.rect_for_type(myst, rec[1])
                if rc:
                    out.append(rc)
        return out

    def walk_plan(self, goals, exclude=(), maxsteps=150, tol_px=0, stop_dist=0):
        """steer Shaggy into one of the world-pixel rectangles `goals` along the game's own walkable area (walkplan.py), staying
        out of other exits.  Returns True on arrival, False if the room changed or no route was found."""
        import walkplan
        r = self.ram()
        myst, room = self.w(0xFF06AA, r), self.w(0xFF06AC, r)
        pl = walkplan.Planner.get(myst, room)
        avoid = self.walkon_rects(myst, room, exclude, r)
        goals = [g for g in goals]
        # spawn points often lie on another exit's hotspot (porch door, hallway door...): step out of it the shortest way
        # first, before any step along the plan wanders inside it (every step in an exit zone fires its walk-on script)
        for _ in range(8):
            x, y = self.pos()
            inside = [rc for rc in avoid if rc[0] <= x < rc[2] and rc[1] <= y < rc[3]]
            if not inside:
                break
            rc = inside[0]
            # distance (in frames, y is half speed) to leave the rectangle by each side that stays inside the room
            cands = []
            if rc[0] > 0:
                cands.append((x - rc[0] + 1, "L"))
            if rc[2] < pl.W:
                cands.append((rc[2] - x, "R"))
            if rc[1] > 0:
                cands.append((2 * (y - rc[1] + 1), "U"))
            if rc[3] < pl.H:
                cands.append((2 * (rc[3] - y), "D"))
            if not cands:
                break
            self.press(min(cands)[1], 6)
            if self.w(0xFF06AC) != room:
                return False
        px, py = self.pos(r)

        def known(d):
            """pos (or a pixel within 4 px of it) has a route"""
            for dy in range(-4, 5):
                for dx in range(-4, 5):
                    x, y = px + dx, py + dy
                    if 0 <= x < pl.W and 0 <= y < pl.H and d[y * pl.W + x] >= 0:
                        return True
            return False

        d = None
        used = (avoid, 0)
        for av in (avoid, []):                       # no way round the other exits: allow them
            for margin in (2, 1, 0):                 # keep off the collision fence when there is room
                dd = pl.field(goals, av, margin)
                if known(dd):
                    d = dd
                    used = (av, margin)
                    break
            if d is not None:
                break
        if d is None:
            # the goal is not reachable on foot (e.g. an item in a part of the room Shaggy cannot enter): get as close as possible
            near = pl.nearest_reachable((px, py), goals, avoid)
            if near is None or near == (px, py):
                return False
            goals = [(near[0] - 2, near[1] - 2, near[0] + 3, near[1] + 3)]
            d = pl.field(goals, avoid, 0)
        stuck = 0
        blocked = []
        for _ in range(maxsteps):
            x, y = self.pos()
            if self.w(0xFF06AC) != room:
                return False
            if any(g[0] <= x < g[2] and g[1] <= y < g[3] for g in goals):
                return True
            if stop_dist and 0 <= x < pl.W and 0 <= y < pl.H and 0 <= d[y * pl.W + x] <= stop_dist:
                return True                          # close enough (approach only: do not step on an exit)
            path = pl.path(d, (x, y), 16)
            if not path:
                return False
            p0 = path[0]
            m0 = (path[0][0] - x, path[0][1] - y)
            run = 1
            while run < len(path) and (path[run][0] - path[run - 1][0], path[run][1] - path[run - 1][1]) == m0:
                run += 1
            tx, ty = path[run - 1]
            b = walkplan.PAD.get(m0) or walkplan.pad_for(m0[0], m0[1])
            if not (0 <= x < pl.W and 0 <= y < pl.H) or d[y * pl.W + x] < 0:
                # off the model (e.g. a spawn point just outside the collision fence): only straight moves slide the probe
                # onto a free pixel, diagonals just stop; go for the nearest known pixel one axis at a time
                dx, dy = p0[0] - x, p0[1] - y
                if dx and dy:
                    b = ("R" if dx > 0 else "L") if stuck % 2 == 0 else ("D" if dy > 0 else "U")
                elif dx:
                    b = "R" if dx > 0 else "L"
                elif dy:
                    b = "D" if dy > 0 else "U"
            if stuck:
                alt = {"R": ["DR", "UR"], "L": ["DL", "UL"], "U": ["UR", "UL"], "D": ["DR", "DL"]}.get(b, ["U", "D", "L", "R"])
                b = alt[(stuck - 1) % len(alt)]
            if os.environ.get("STORY_DEBUG"):
                print(f"  plan: pos=({x},{y}) d={d[y * pl.W + x]} -> {(tx, ty)} press {b} stuck={stuck}")
            self.press(b, max(4, min(12, 2 * run)))
            stuck = stuck + 1 if self.pos() == (x, y) else 0
            if stuck == 3 and len(blocked) < 12:
                # the game's collision differs from the model here: treat the pixels in front of us as walls and re-plan
                blocked.append((p0[0] - 3, p0[1] - 3, p0[0] + 4, p0[1] + 4))
                d = pl.field(goals, list(used[0]) + blocked, used[1])
                stuck = 0
            if stuck > 6:
                return False
        return False

    def walk_to(self, tx, ty, tol=3, maxsteps=300, step=12, quiet=False):
        try:
            self.walk_plan([(tx - tol, ty - tol, tx + tol + 1, ty + tol + 1)])
            return self.pos()
        except Exception as e:                      # planner problems must not break the greedy fallback
            if os.environ.get("STORY_DEBUG"):
                print("walk_plan failed:", e)
        return self.walk_to_greedy(tx, ty, tol, maxsteps, step, quiet)

    def walk_to_greedy(self, tx, ty, tol=3, maxsteps=300, step=12, quiet=False):
        """walk Shaggy with the d-pad until within tol px of (tx,ty): axis-wise moves (the floor is
        irregular, diagonals get stuck), with a sidestep when both axes are blocked"""
        import random
        rng = random.Random(1)
        for _ in range(maxsteps):
            x, y = self.pos()
            dx, dy = tx - x, ty - y
            if abs(dx) <= tol and abs(dy) <= tol:
                break
            order = []
            if abs(dx) > tol: order.append(("L" if dx < 0 else "R", abs(dx)))
            if abs(dy) > tol: order.append(("U" if dy < 0 else "D", abs(dy)))
            order.sort(key=lambda t: -t[1])
            moved = False
            for b, dist in order:
                n = max(2, min(step, int(dist / 0.3)))
                self.press(b, n)
                if self.pos() != (x, y):
                    moved = True
                    break
            if not moved:
                b = rng.choice(["U", "D", "L", "R", "UL", "UR", "DL", "DR"])
                self.press(b, 20)
        return self.pos()

    # ---- pointer mode ----
    def pointer_mode(self):
        return bool(self.b(0xFF09DC) & 8)

    def enter_pointer(self):
        if not self.pointer_mode():
            self.press("A", 4, 4)
        return self.pointer_mode()

    def leave_pointer(self):
        if self.pointer_mode():
            self.press("A", 4, 4)

    def set_pointer(self, x, y, settle=8):
        self.poke(0xFF06DC, 2, x)
        self.poke(0xFF06DE, 2, y)
        self.run(settle)

    def select_verb(self, verb):
        vid = VERBS[verb] if isinstance(verb, str) else verb
        col, row = VERB_CELL[vid]
        self.enter_pointer()
        if self.b(0xFF09DC) & 0x10:      # inventory view -> back to verb view
            self.press("C", 3, 24)
        for attempt in range(4):
            self.set_pointer(16 + 40 * col + 12, 178 + 16 * row + attempt, settle=10 + 4 * attempt)
            self.press("B", 4 + 2 * attempt, 4)
            if self.w(0xFF06C0) == vid:
                return True
        return False

    def target_at(self, x, y):
        self.enter_pointer()
        self.set_pointer(x, y)
        return self.w(0xFF06AE)

    def busy(self):
        """True while an action/dialogue is running (bit 7 of $FF09DC) or a script is active"""
        r = self.ram()
        return bool(self.b(0xFF09DC, r) & 0x80)

    def settle(self, maxframes=4000, quiet=30):
        """run until no action/dialogue is pending for `quiet` frames (or maxframes)"""
        t = 0
        calm = 0
        while t < maxframes and calm < quiet:
            self.run(10)
            t += 10
            calm = calm + 10 if not self.busy() else 0
        return t

    def verb_on(self, verb, x, y, expect=None):
        """select verb, put the crosshair on (x,y) (top-left coords) and click with B"""
        ok = self.select_verb(verb)
        self.set_pointer(x, y)
        t = self.w(0xFF06AE)
        if expect is not None:
            # the main loop's hit test needs a pass or two after the crosshair moved; wait on the
            # RAM condition (up to ~60 frames) instead of trusting a fixed 8-frame settle
            for _ in range(15):
                if t == expect:
                    break
                self.run(4)
                t = self.w(0xFF06AE)
        if expect is not None and t != expect:
            return ("target-mismatch", t)
        self.press("B", 4, 10)
        self.settle()
        return ("ok" if ok else "verb-not-set", t)

    def write_route(self, path, header=""):
        with open(path, "w") as f:
            for ln in header.strip().split("\n"):
                if ln:
                    f.write("# " + ln + "\n")
            for ln in self.route:
                f.write(ln + "\n")

    def save_points(self):
        """write the scan cache: scripts/story/points_w{0,1}.json  (key myst:room:tid -> world x,y of a crosshair hit point)"""
        import json
        for m in (0, 1):
            pts = {k: v for k, v in self.points.items() if k.startswith(f"{m}:")}
            if not pts:
                continue
            fp = os.path.join(POINTS_DIR, f"points_w{m}.json")
            old = json.load(open(fp)) if os.path.exists(fp) else {}
            old.update(pts)
            with open(fp, "w") as f:
                json.dump(old, f, indent=0, sort_keys=True)

    def mark(self, name):
        """savestate + remember the route so far (exploration backtracking only)"""
        self.send(f"save {name}")
        open(os.path.join(self.d, name + ".route"), "w").write("".join(c + "\n" for c in self.route))

    def back(self, name):
        self.send(f"load {name}")
        self.route = open(os.path.join(self.d, name + ".route")).read().split("\n")[:-1]
        open(self.rpath, "w").write("".join(c + "\n" for c in self.route))


def _hex(s):
    return int(s, 16)


def scan_objects(self, step=4, y0=16, y1=164, x1=250, slot_names=True):
    """Sweep the crosshair over the room area (state is restored afterwards) and return
    {target_id: dict(name, default_verb, box=(x0,y0,x1,y1) of crosshair top-left positions, n)}.
    target ids: 1 Shaggy, 2 Scooby, n>=3 = object slot n-3."""
    self.enter_pointer()
    self.mark("_scan")
    cmds = []
    pts = []
    for y in range(y0, y1, step):
        for x in range(0, x1, step):
            pts.append((x, y))
            cmds += [f"poke FF06DC 2 {x:X}", f"poke FF06DE 2 {y:X}", "run 2", "peek t FF06AE 2", "peek v FF06C4 2"]
    log = self.send(*cmds)
    res = {}
    t = None
    vals = [l.split() for l in log if l.split()[1] in ("t", "v")]
    for i, (x, y) in enumerate(pts):
        tt = _hex(vals[2 * i][2])
        dv = _hex(vals[2 * i + 1][2])
        if tt == 0:
            continue
        r = res.setdefault(tt, dict(default_verb=dv, box=[x, y, x, y], n=0, pt=(x, y), hits=[]))
        r["hits"].append((x, y))
        r["box"] = [min(r["box"][0], x), min(r["box"][1], y), max(r["box"][2], x), max(r["box"][3], y)]
        r["n"] += 1
    if slot_names:
        for tt, r in res.items():
            hx = sum(p[0] for p in r["hits"]) / len(r["hits"])
            hy = sum(p[1] for p in r["hits"]) / len(r["hits"])
            r["pt"] = min(r["hits"], key=lambda p: (p[0] - hx) ** 2 + (p[1] - hy) ** 2)
            cx = (r["box"][0] + r["box"][2]) // 2
            cy = (r["box"][1] + r["box"][3]) // 2
            # pick a point that really hits tt, closest to the centre
            self.send(f"poke FF06DC 2 {r['pt'][0]:X}", f"poke FF06DE 2 {r['pt'][1]:X}", "run 3")
            r["name"] = obj_name(self.w(0xFF06AA), tt)
    self.back("_scan")
    r = self.ram()
    cx, cy, rm, my = self.w(0xFF06CA, r), self.w(0xFF06CE, r), self.w(0xFF06AC, r), self.w(0xFF06AA, r)
    for tt, o in res.items():
        if tt >= 3:
            self.points[f"{my}:{rm}:{tt}"] = [o["pt"][0] + cx, o["pt"][1] + cy]
    return res


def status_text(self):
    r = self.ram()
    s = r[0x08BC:0x08BC + 40]
    s = s.split(b"\0")[0]
    return s.decode("latin1")


Game.scan_objects = scan_objects
Game.status_text = status_text

import objects as _objects
_NAMES = {}


def obj_name(myst, tid):
    if myst not in _NAMES:
        _NAMES[myst] = {o["tid"]: o["name"] for o in _objects.load(myst)}
    return {1: "Shaggy", 2: "Scooby"}.get(tid) or _NAMES[myst].get(tid, "?")


def room_objects(self, r=None):
    """objects whose room word (+6) equals the current room: list of (tid, name, record words)"""
    r = r or self.ram()
    room = self.w(0xFF06AC, r)
    myst = self.w(0xFF06AA, r)
    out = []
    for i, rec in enumerate(self.objects(r)):
        if rec[3] == room:
            out.append((i + 3, obj_name(myst, i + 3), rec))
    return out


_TEXT = {}


def _text_index():
    """start address -> string, for every NUL-terminated ASCII string in the text areas"""
    if _TEXT:
        return _TEXT
    rom = _objects.ROM
    names = {o["name"] for m in (0, 1) for o in _objects.load(m)}
    for lo, hi in ((0x141000, 0x1B3500), (0x2FEC0, 0x30000)):
        a = lo
        while a < hi:
            e = rom.find(b"\0", a, hi)
            if e < 0:
                break
            s = rom[a:e]
            if len(s) >= 2 and all(32 <= c < 127 or c in (1, 2) for c in s) and s.decode("latin1") not in names:
                _TEXT[a] = s.decode("latin1")
            a = e + 1
    return _TEXT


def said(self, fn, frames=0):
    """run fn() with the harness trace on; return the list of game strings the 68k read (in order)"""
    tp = os.path.join(self.d, "t.trace")
    self.send("trace t")
    fn()
    if frames:
        self.run(frames)
    self.send("untrace")
    idx = _text_index()
    out = []
    seen_frame = {}
    if not os.path.exists(tp):          # backend without a bus trace (the native port): no text, closed-loop uses RAM only
        return []
    for ln in open(tp):
        p = ln.split()
        if p[0] != "R":
            continue
        a = int(p[3], 16)
        if a < 0x10200 and (0x259e <= a < 0x25a0 or 0x272e <= a < 0x2730 or 0x286e <= a < 0x2880):
            with open(os.path.join(self.d, "..", "overlap_hits.txt"), "a") as hf:
                hf.write(f"{ln.strip()} route_len={len(self.route)}\n")
        if a in idx:
            if out and out[-1][1] == a:
                continue
            out.append((int(p[1]), a))
    return [(f, idx[a], a) for f, a in out]


def story_diff(a, b):
    """list of (byte, bit-mask-changes) between two 29-byte story states"""
    out = []
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            out.append(f"{i:02x}:{x:02x}->{y:02x}")
    return out


def try_action(self, verb, tid, pt, wait=100, restore=True):
    """perform `verb` on the object at crosshair point pt; report (said, story diff, inventory diff, room change)"""
    if restore:
        self.mark("_try")
    s0 = self.story()
    i0 = self.inventory()
    room0 = self.w(0xFF06AC)
    res = {}
    def act():
        res["r"] = self.verb_on(verb, pt[0], pt[1], expect=tid)
    texts = [(f, t) for f, t, a in self.said(act, frames=wait) if a >= 0x141000]
    res["dbg"] = (self.state()["flags"], self.state()["ptr"], self.state()["verb"])
    r = self.ram()
    out = dict(res=res.get("r"), dbg=res.get("dbg"), said=[t for _, t in texts], story=story_diff(s0, self.story(r)),
               inv=(i0, self.inventory(r)) if self.inventory(r) != i0 else None,
               room=(room0, self.w(0xFF06AC, r)) if self.w(0xFF06AC, r) != room0 else None)
    if restore:
        self.back("_try")
    return out


def explore_room(self, verbs=("Look", "Take", "Open", "Push", "Pull", "Talk", "Eat", "Shut"), wait=100):
    objs = self.scan_objects()
    report = []
    for tid, o in sorted(objs.items()):
        if tid < 3:
            continue
        for v in verbs:
            r = self.try_action(v, tid, o["pt"], wait)
            report.append((tid, o["name"], v, r))
            tag = f"{tid:3d} {o['name']:20s} {v:5s}"
            print(tag, ("|".join(r["said"])[:160] or "-"), r["story"] or "", r["inv"] or "", r["room"] or "", "" if r["res"] and r["res"][0] == "ok" else r["res"], flush=True)
    return report


Game.try_action = try_action
Game.explore_room = explore_room
def ensure_visible(self, tid):
    """if the cached world point of `tid` is off screen, walk Shaggy towards it so the camera scrolls there"""
    r = self.ram()
    k = f"{self.w(0xFF06AA, r)}:{self.w(0xFF06AC, r)}:{tid}"
    if k not in self.points:
        return
    wx, wy = self.points[k]
    room0 = self.w(0xFF06AC, r)
    for _ in range(4):
        r = self.ram()
        if self.w(0xFF06AC, r) != room0:
            return                      # walked through an exit on the way (timing differs between backends)
        sx, sy = wx - self.w(0xFF06CA, r), wy - self.w(0xFF06CE, r)
        if 8 <= sx <= 236 and 24 <= sy <= 152:
            return
        self.leave_pointer()
        self.walk_to(wx + 8, wy - 8, tol=6)


Game.ensure_visible = ensure_visible


def hot_rect(self, tid, r=None):
    """live hotspot rectangle (world pixels) of a non-actor object: record +2 (changed by set_hotspot) -> overlay table"""
    import hotspots
    if tid < 3:
        return None
    r = r or self.ram()
    if self.b(0xFF1200 + 26 * (tid - 3) + 0x18, r) & 2:          # actors have no hotspot rectangle (the hit area is the sprite)
        return None
    return hotspots.rect_for_type(self.w(0xFF06AA, r), self.w(0xFF1200 + 26 * (tid - 3) + 2, r))


def probe_batch(self, tid, pts, addr):
    """move the crosshair over `pts` (3 frames each) and report whether the hit test at `addr` ever shows `tid`; the first hit is
    left in self._probe_hit.  Everything that moves the game (pointer pokes, frames) is recorded in the flat route exactly
    (only the peeks are not): the pointer position during the probe is game input and a replay must see the same one."""
    cmds = []
    for x, y in pts:
        cmds += [f"poke FF06DC 2 {x:X}", f"poke FF06DE 2 {y:X}", "run 3", f"peek t {addr:X} 2"]
    self.note(*[c for c in cmds if not c.startswith("peek")])
    log = self.send(*cmds)
    vals = [_hex(l.split()[2]) for l in log if l.split()[1] == "t"]
    for p, v in zip(pts, vals):
        if v == tid:
            self._probe_hit = p
            return True
    return False


Game.probe_batch = probe_batch


def probe_rect(self, tid, rect, addr=0xFF06AE):
    """crosshair point (screen coords) inside `rect` for which the game's hit test reports `tid`, or None.
    All candidates are tried in one batch (poke pointer, run 2, peek $FF06AE)."""
    r = self.ram()
    cx, cy = self.w(0xFF06CA, r), self.w(0xFF06CE, r)
    # the hit test looks at world cell ((px+8+camera_x)>>3, (py-8+camera_y)>>3)  (docs/subsystems/walking.md section 2)
    x0, y0, x1, y1 = rect[0] - cx - 8, rect[1] - cy + 8, rect[2] - cx - 8, rect[3] - cy + 8
    x0, x1, y0, y1 = max(x0, 0), min(x1, 248), max(y0, 16), min(y1, 164)
    if x0 >= x1 or y0 >= y1:
        return None
    mx, my = (x0 + x1) // 2, (y0 + y1) // 2
    allpts = sorted(((x, y) for y in range(y0, y1, 4) for x in range(x0, x1, 2)), key=lambda p: (p[0] - mx) ** 2 + (p[1] - my) ** 2)
    self.enter_pointer()
    # nearest to the middle first; Shaggy / Scooby / other objects may cover part of the rectangle (they win the hit test),
    # so widen to the whole rectangle in batches
    for lo in range(0, len(allpts), 12):
        pts = allpts[lo:lo + 12]
        if self.probe_batch(tid, pts, addr):
            return self._probe_hit
    return None


def probe_actor(self, tid, addr=0xFF06AE):
    """crosshair point on an actor (Bellhop, Uncle Blake, the Clown...): the hit area is a box around its sprite; probe the
    neighbourhood of the position kept in its object record (+$12/+$14 = world x/y of the feet)"""
    if tid in (1, 2):                                # wait until the follower has stopped moving (hit box = sprite)
        last = None
        for _ in range(80):                          # Scooby trails Shaggy: wait until he is on screen and standing still
            r = self.ram()
            cur = (self.w(0xFF04DC + 4 * (tid - 1), r), self.w(0xFF04F4 + 4 * (tid - 1), r))
            sx = cur[0] - self.w(0xFF06CA, r)
            if cur == last and 8 <= sx <= 240:
                break
            last = cur
            self.run(15)
    r = self.ram()
    cx, cy = self.w(0xFF06CA, r), self.w(0xFF06CE, r)
    if tid in (1, 2):                                # Shaggy / Scooby: position arrays $FF04DC.. (x) and $FF04F4.. (y), 16.16
        ax, ay = self.w(0xFF04DC + 4 * (tid - 1), r) - cx, self.w(0xFF04F4 + 4 * (tid - 1), r) - cy + 16
    else:
        rec = self.objects(r)[tid - 3]
        ax, ay = rec[9] - cx, rec[10] - cy + 16
    pts = sorted(((x, y) for y in range(ay - 44, ay + 12, 4) for x in range(ax - 24, ax + 25, 4) if 0 <= x < 248 and 16 <= y < 164),
                 key=lambda p: (p[0] - ax) ** 2 + (p[1] - ay + 16) ** 2)
    if os.environ.get("STORY_DEBUG"):
        print(f"  probe_actor tid={tid} at ({ax},{ay}) {len(pts)} window points, 09DE={self.w(0xFF09DE):#x}")
    if not pts:
        return None
    self.enter_pointer()
    for lo in range(0, len(pts), 12):
        if self.probe_batch(tid, pts[lo:lo + 12], addr):
            return self._probe_hit
    # the actor moved (or its record is stale): sweep the whole room view
    allp = [(x, y) for y in range(16, 164, 6) for x in range(0, 248, 6)]
    for lo in range(0, len(allp), 24):
        if self.probe_batch(tid, allp[lo:lo + 24], addr):
            return self._probe_hit
    return None


Game.probe_actor = probe_actor


def locate(self, tid, rounds=4):
    """crosshair point (screen coords) that hits object tid.  Static first: the object's hotspot rectangle, probed with the
    live hit test, walking Shaggy towards it when it is off screen (the camera follows).  Fallback: a crosshair scan (non-strict)
    or the points.json cache (strict) for actors, whose hit area is the sprite."""
    room0 = self.w(0xFF06AC)
    for _ in range(rounds):
        rect = self.hot_rect(tid)
        if rect is None:
            if tid > 2:                              # an actor on a part of the room that is not on screen: walk towards it first
                r = self.ram()
                rec = self.objects(r)[tid - 3]
                sx = rec[9] - self.w(0xFF06CA, r)
                if rec[3] == self.w(0xFF06AC, r) and not (16 <= sx <= 236):
                    self.leave_pointer()
                    self.walk_to(rec[9], rec[10], tol=24)
                    self.run(20)
            pt = self.probe_actor(tid)
            if pt:
                return pt
            if tid > 2 and self.w(0xFF06AC) == room0 and _ < rounds - 1:
                continue
            break
        pt = self.probe_rect(tid, rect)
        if pt:
            return pt
        if self.w(0xFF06AC) != room0:
            return None
        self.leave_pointer()
        try:
            self.walk_plan([rect], stop_dist=7)      # bring it on screen without stepping onto it (it may be an exit)
        except Exception:
            self.walk_to((rect[0] + rect[2]) // 2, (rect[1] + rect[3]) // 2 - 8, tol=6)
    self.ensure_visible(tid)
    if self.w(0xFF06AC) != room0:
        return None
    if self.strict:
        r = self.ram()
        k = f"{self.w(0xFF06AA, r)}:{self.w(0xFF06AC, r)}:{tid}"
        if k not in self.points:
            raise KeyError(f"no cached point for {k}; explore it first (non-strict)")
        wx, wy = self.points[k]
        return (wx - self.w(0xFF06CA, r), wy - self.w(0xFF06CE, r))
    objs = self.scan_objects()
    return objs[tid]["pt"] if tid in objs else None


Game.hot_rect = hot_rect
Game.probe_rect = probe_rect
Game.locate = locate


def go_to(self, tid, dy=-16, tries=3, verbose=True):
    """walk Shaggy onto the object `tid` (D-pad), return True when the walking hit-test reports it"""
    self.leave_pointer()
    rect = self.hot_rect(tid)
    if rect is not None:
        room0 = self.w(0xFF06AC)
        for attempt in range(2):
            ok = self.walk_plan([rect], exclude=(tid,))
            self.run(10)
            if self.w(0xFF06AC) != room0 or self.w(0xFF06AE) == tid:
                return self.w(0xFF06AC) != room0 or self.w(0xFF06AE) == tid
            if not ok:
                # the model finds no way (exits just outside the collision fence are reached by the game's wall-slide, which the
                # planner does not model): fall back to the greedy axis-wise walker with sidesteps
                self.walk_to_greedy((rect[0] + rect[2]) // 2, (rect[1] + rect[3]) // 2, tol=4)
                self.run(10)
                if self.w(0xFF06AC) != room0 or self.w(0xFF06AE) == tid:
                    return True
    pt = self.locate(tid)
    if pt is None:
        return False
    self.leave_pointer()
    r = self.ram()
    cx, cy = self.w(0xFF06CA, r), self.w(0xFF06CE, r)
    px, py = pt
    for _ in range(tries):
        tx, ty = px + 8 + cx, py + 8 + cy + dy
        self.walk_to(tx, ty, tol=4)
        self.run(10)
        if self.w(0xFF06AE) == tid:
            return True
    return False


def exit_via(self, tid, push=None):
    """walk to exit-like object `tid` and keep walking through it; True if the room changed"""
    import walkplan
    room = self.w(0xFF06AC)
    for attempt in range(3):
        self.go_to(tid)
        if self.w(0xFF06AC) != room:
            break
        rect = self.hot_rect(tid)
        pushes = [push] if push else []
        if rect:                                     # step on towards the middle of the hotspot first
            x, y = self.pos()
            cx, cy = (rect[0] + rect[2]) // 2, (rect[1] + rect[3]) // 2
            pushes.append(walkplan.pad_for(cx - x, 2 * (cy - y)) or "U")
        pushes += ["D", "U", "L", "R"]
        for b in pushes:
            if self.w(0xFF06AC) != room:
                break
            self.press(b, 40, 30)
        if self.w(0xFF06AC) != room:
            break
    self.run(60)
    return self.w(0xFF06AC) != room


Game.exit_via = exit_via


def act(self, verb, tid, quiet=False, wait=30):
    """verb on object `tid` (by scanning the crosshair hit points); recorded in the route"""
    for attempt in range(3):
        self.leave_pointer()
        pt = self.locate(tid)
        if pt is None:
            print(f"!! {self.name(tid)} ({tid}) not under the crosshair")
            return None
        r = self.try_action(verb, tid, pt, wait=wait, restore=False)
        if not (r["res"] and r["res"][0] == "target-mismatch"):
            break
        self.run(30)            # the room was still moving (camera pan / actor); locate again
    if not quiet:
        print(f"{verb} {self.name(tid)}: {' | '.join(r['said']) or '-'}", r["story"] or "", r["inv"] or "", r["room"] or "", r["res"][0] if r["res"] and r["res"][0] != "ok" else "")
    return r


def pick_item(self, item, res=None, addr=0xFF06AE):
    """inventory view is showing: scroll to the page holding `item` (hit-test id) and click it with B.
    List index i sits in slot i-4*page (0..7, 4 per row); page = $FF06EC; the arrows are in the right-most cell.
    Returns True when the hit test reported `item` at the click."""
    idx = self.inventory().index(item - 3)
    want = idx // 4
    for _ in range(14):
        page = self.w(0xFF06EC)
        if page == want:
            break
        self.set_pointer(220, 176 if page > want else 200)
        self.press("B", 4, 8)
        self.run(8)
    slot = idx - 4 * self.w(0xFF06EC)
    row, col = divmod(slot, 4)
    ok = False
    for attempt in range(4):
        self.run(12)                                 # let the previous click (B edge) clear before the item click
        self.set_pointer(40 * (col + 1) + 20 + 2 * attempt, 176 if row == 0 else 200)
        if res is not None:
            res["tgt1"] = self.w(0xFF06AE)
        for _ in range(8):                           # hit test lags the crosshair by a few frames
            if self.w(addr) == item:
                ok = True
                break
            self.run(3)
        if ok:
            break
    self.press("B", 4, 6)
    return ok


def talk_choices(self, r=None):
    """texts of the TALK menu entries currently collected ($FF080E count, $FF0838 text pointers)"""
    r = r or self.ram()
    n = self.w(0xFF080E, r)
    out = []
    for i in range(min(n, 5)):
        a = struct.unpack(">I", r[0x0838 + 4 * i:0x083C + 4 * i])[0]
        e = _objects.ROM.find(b"\0", a)
        out.append(_objects.ROM[a:e].decode("latin1"))
    return out


def talk_choose(self, text, wait=30):
    """TALK menu is open: move the highlight to the entry whose text starts with `text`, pick it with B, wait for the reply"""
    ch = self.talk_choices()
    used = self.w(0xFF0874)                               # bit n = choice n already picked (not listed any more)
    idx = next((i for i, c in enumerate(ch) if c.startswith(text) and not used >> i & 1), None)
    if idx is None:
        print(f"!! talk choice {text!r} not in {ch} (used {used:#x})")
        return False
    idx -= sum(1 for i in range(idx) if used >> i & 1)    # position among the listed choices
    for _ in range(idx):
        self.press("D", 6, 12)
    self.press("B", 6, 12)
    self.settle()
    self.run(wait)
    return True


Game.talk_choices = talk_choices
Game.talk_choose = talk_choose


def talk_to(self, tid, *choices):
    """TALK verb on `tid`; with a menu open, pick the given choices in turn (each by its text prefix)"""
    self.leave_pointer()
    pt = self.locate(tid)
    if pt is None:
        print(f"!! {self.name(tid)} ({tid}) not under the crosshair")
        return None
    self.select_verb("Talk")
    self.set_pointer(*pt)
    for _ in range(15):
        if self.w(0xFF06AE) == tid:
            break
        self.run(4)
    self.press("B", 4, 10)
    for c in choices:
        for _ in range(120):                              # the menu is up when talk_menu_begin hid the pointer ($FF06EE = $FFFF)
            self.run(10)
            if self.w(0xFF080E) and self.w(0xFF06EE) == 0xFFFF:
                break
        self.run(40)
        self.talk_choose(c)
    self.settle()
    self.leave_pointer()


Game.talk_to = talk_to


def use_inv(self, item1, item2, verb="Use", wait=30):
    """two inventory items combined (Battery + Light Bulb...): verb, inventory view, click item1, click item2 (second target)"""
    self.leave_pointer()
    inv = self.inventory()
    if (item1 - 3) not in inv or (item2 - 3) not in inv:
        print(f"!! {self.name(item1)} / {self.name(item2)} not both in inventory {inv}")
        return None
    s0, i0 = self.story(), inv
    for attempt in range(3):
        self.select_verb(verb)
        self.press("C", 3, 24)
        self.pick_item(item1)
        for _ in range(12):
            if self.w(0xFF09DE) & 1:
                break
            self.run(3)
        if self.w(0xFF09DE) & 1:                     # two-object mode started
            break
        self.press("C", 3, 24) if self.b(0xFF09DC) & 0x10 else None
        self.run(30)
    if (item2 - 3) in self.inventory():
        self.pick_item(item2, addr=0xFF06B6)
        self.run(8)
    self.settle()
    self.run(wait)
    if self.b(0xFF09DC) & 0x10:
        self.press("C", 3, 24)
    r = self.ram()
    out = dict(story=story_diff(s0, self.story(r)), inv=(i0, self.inventory(r)) if self.inventory(r) != i0 else None)
    print(f"{verb} {self.name(item1)} on {self.name(item2)} (inventory):", out["story"] or "", out["inv"] or "")
    return out


Game.use_inv = use_inv


def act_inv(self, verb, item, wait=30):
    """one-object verb on an inventory item (Eat, Look, Open...): verb, inventory view, click the item"""
    self.leave_pointer()
    if (item - 3) not in self.inventory():
        print(f"!! {self.name(item)} not in inventory {self.inventory()}")
        return None
    s0, i0, room0 = self.story(), self.inventory(), self.w(0xFF06AC)
    self.select_verb(verb)
    self.press("C", 3, 24)
    self.pick_item(item)
    self.settle()
    self.run(wait)
    self.press("C", 3, 24) if self.b(0xFF09DC) & 0x10 else None
    r = self.ram()
    out = dict(story=story_diff(s0, self.story(r)), inv=(i0, self.inventory(r)) if self.inventory(r) != i0 else None,
               room=(room0, self.w(0xFF06AC, r)) if self.w(0xFF06AC, r) != room0 else None)
    print(f"{verb} {self.name(item)} (inventory):", out["story"] or "", out["inv"] or "", out["room"] or "")
    return out


Game.pick_item = pick_item
Game.act_inv = act_inv


def use(self, item, tid, verb="Use", wait=30, restore=False, dbg=False):
    """Use/Give inventory item `item` (hit-test id = object index+3, e.g. Battery 135) on object `tid`.
    Flow: pick the verb in the verb view, slide the bar to the inventory (C), click the item with B,
    slide back, click the target (room objects are clickable in either view)."""
    inv = self.inventory()
    idx = inv.index(item - 3) if (item - 3) in inv else None
    if idx is None:
        print(f"!! item {self.name(item)} not in inventory {inv}")
        return None
    self.leave_pointer()
    pt = self.locate(tid)
    if pt is None:
        print(f"!! {self.name(tid)} ({tid}) not under the crosshair")
        return None
    if restore:
        self.mark("_try")
    s0, i0, room0 = self.story(), self.inventory(), self.w(0xFF06AC)
    res = {}

    def act_():
        for attempt in range(3):
            self.select_verb(verb)
            self.press("C", 3, 24)                   # inventory view
            self.pick_item(item, res)
            for _ in range(12):                      # the main loop sets the waiting flag a few frames after the click
                if self.w(0xFF09DE) & 1:
                    break
                self.run(3)
            res["wait2"] = self.w(0xFF09DE) & 1
            if os.environ.get("STORY_DEBUG"):
                print("  use: after item click 09DE", hex(self.w(0xFF09DE)), "attempt", attempt)
            if res["wait2"]:                         # two-object mode started: now waiting for the target
                break
            self.press("C", 3, 24) if self.b(0xFF09DC) & 0x10 else None
            self.run(30)
        self.press("C", 3, 24)                       # back to the verb view
        if os.environ.get("STORY_DEBUG"):
            print("  use: after C 09DE", hex(self.w(0xFF09DE)))
        cur = pt
        if tid <= 2 or self.hot_rect(tid) is None:       # actors: find the sprite hit point in second-target mode right away
            p2 = self.probe_actor(tid, 0xFF06B6)
            if p2:
                cur = p2
        for attempt in range(3):
            self.set_pointer(*cur)
            for _ in range(8):
                if self.w(0xFF06B6) == tid:
                    break
                self.run(3)
            if self.w(0xFF06B6) != tid:                  # the second-target hit test: probe it in its own mode
                rc = self.hot_rect(tid)
                p2 = self.probe_rect(tid, rc, 0xFF06B6) if rc else self.probe_actor(tid, 0xFF06B6)
                if os.environ.get("STORY_DEBUG"):
                    print("  use: second target probe", cur, "->", p2, "09DE", hex(self.w(0xFF09DE)), "verbview", self.b(0xFF09DC) & 0x10)
                if p2:
                    cur = p2
                    self.set_pointer(*cur)
                    self.run(8)
            res["tgt2"] = self.w(0xFF06B6)
            self.press("B", 4, 10)
            if not (self.w(0xFF09DE) & 1):           # the click was consumed
                break
        self.settle()

    texts = [(f, t) for f, t, a in self.said(act_, frames=wait) if a >= 0x141000]
    r = self.ram()
    out = dict(said=[t for _, t in texts], story=story_diff(s0, self.story(r)), res=res,
               inv=(i0, self.inventory(r)) if self.inventory(r) != i0 else None,
               room=(room0, self.w(0xFF06AC, r)) if self.w(0xFF06AC, r) != room0 else None)
    print(f"{verb} {self.name(item)} on {self.name(tid)}: {' | '.join(out['said']) or '-'}", out["story"] or "", out["inv"] or "", out["room"] or "", res if dbg else "")
    if restore:
        self.back("_try")
    return out


def use_on(self, tid1, tid2, verb="Use", wait=30):
    """two-object verb where both objects are in the room (Use hose with gas...): verb, click tid1, click tid2"""
    s0, i0, room0 = self.story(), self.inventory(), self.w(0xFF06AC)
    done = False
    for attempt in range(3):
        self.leave_pointer()
        p1 = self.locate(tid1)
        if p1 is None:
            print(f"!! {self.name(tid1)} ({tid1}) not under the crosshair")
            return None
        self.select_verb(verb)
        self.set_pointer(*p1)
        for _ in range(15):
            if self.w(0xFF06AE) == tid1:
                break
            self.run(4)
        if self.w(0xFF06AE) != tid1:
            self.run(30)
            continue
        self.press("B", 4, 6)
        for _ in range(10):                          # two-object mode ($FF09DE bit 0) starts a few frames after the click
            if self.w(0xFF09DE) & 1:
                break
            self.run(3)
        if not self.w(0xFF09DE) & 1:
            continue
        rc = self.hot_rect(tid2) if tid2 > 2 else None
        p2 = self.probe_rect(tid2, rc, 0xFF06B6) if rc else self.probe_actor(tid2, 0xFF06B6)
        if p2 is None:
            print(f"!! {self.name(tid2)} ({tid2}) not found as second object")
            self.leave_pointer()
            return None
        for click in range(3):
            self.set_pointer(*p2)
            for _ in range(15):
                if self.w(0xFF06B6) == tid2:
                    break
                self.run(4)
            self.press("B", 4, 10)
            if not self.w(0xFF09DE) & 1:
                done = True
                break
        break
    self.settle()
    self.run(wait)
    r = self.ram()
    out = dict(story=story_diff(s0, self.story(r)), inv=(i0, self.inventory(r)) if self.inventory(r) != i0 else None,
               room=(room0, self.w(0xFF06AC, r)) if self.w(0xFF06AC, r) != room0 else None)
    if not done and not (out["story"] or out["inv"] or out["room"]):
        print(f"!! {verb} {self.name(tid1)} on {self.name(tid2)}: no effect (second click not consumed?)")
    print(f"{verb} {self.name(tid1)} on {self.name(tid2)}:", out["story"] or "", out["inv"] or "", out["room"] or "")
    return out


Game.use_on = use_on


def brute_use(self, verbs=("Use", "Give"), items=None, targets=None):
    """try every inventory item on every visible object; print only effective combinations"""
    inv = [i + 3 for i in self.inventory()]
    self.leave_pointer()
    objs = self.scan_objects()
    hits = []
    for it in (items or inv):
        for tid in (targets or sorted(objs)):
            if tid < 3 or tid == it:
                continue
            for v in verbs:
                import io, contextlib
                buf = io.StringIO()
                with contextlib.redirect_stdout(buf):
                    r = self.use(it, tid, verb=v, restore=True)
                if r and (r["said"] or r["story"] or r["inv"] or r["room"]):
                    print(buf.getvalue().strip(), flush=True)
                    hits.append((v, it, tid, r))
    return hits


Game.act = act
Game.use = use
Game.brute_use = brute_use


Game.go_to = go_to
Game.said = said
Game.room_objects = room_objects
Game.name = lambda self, tid: obj_name(self.w(0xFF06AA), tid)


# ---- closed-loop checkpoints (port/tools/storytest.sh): after every high-level step record room/inventory/story ----
# story byte 10 ($FF2A0A) is seeded from the RNG during the intro (value depends on how many frames the 68k spent:
# $01 in the harness / --hle none, $D9 with the HLE replacements); it is not story state, so checkpoints mask it
VOLATILE_STORY_BYTES = (10,)


def checkpoint(self, label):
    r = self.ram()
    self.cp = getattr(self, "cp", [])
    st = bytearray(self.story(r))
    raw = st.hex()
    for i in VOLATILE_STORY_BYTES:
        st[i] = 0
    self.cp.append(dict(label=label, room=self.w(0xFF06AC, r), inv=self.inventory(r), story=st.hex(), raw=raw))


def _wrap(name):
    f = getattr(Game, name)

    def w(self, *a, **k):
        res = f(self, *a, **k)
        self.checkpoint(f"{name} {a}")
        return res
    setattr(Game, name, w)


Game.checkpoint = checkpoint
for _n in ("act", "talk_to", "use", "use_on", "use_inv", "act_inv", "exit_via", "boot"):
    _wrap(_n)
