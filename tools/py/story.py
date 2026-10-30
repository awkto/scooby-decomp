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

    def walk_to(self, tx, ty, tol=3, maxsteps=300, step=12, quiet=False):
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

    def settle(self, maxframes=900, quiet=30):
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
    for _ in range(4):
        r = self.ram()
        sx, sy = wx - self.w(0xFF06CA, r), wy - self.w(0xFF06CE, r)
        if 8 <= sx <= 236 and 24 <= sy <= 152:
            return
        self.leave_pointer()
        self.walk_to(wx + 8, wy - 8, tol=6)


Game.ensure_visible = ensure_visible


def locate(self, tid):
    """crosshair point (screen coords) that hits object tid: from a scan, or in strict mode from points.json"""
    self.ensure_visible(tid)
    if self.strict:
        r = self.ram()
        k = f"{self.w(0xFF06AA, r)}:{self.w(0xFF06AC, r)}:{tid}"
        if k not in self.points:
            raise KeyError(f"no cached point for {k}; explore it first (non-strict)")
        wx, wy = self.points[k]
        return (wx - self.w(0xFF06CA, r), wy - self.w(0xFF06CE, r))
    objs = self.scan_objects()
    return objs[tid]["pt"] if tid in objs else None


Game.locate = locate


def go_to(self, tid, dy=-16, tries=3, verbose=True):
    """walk Shaggy onto the object `tid` (D-pad), return True when the walking hit-test reports it"""
    self.leave_pointer()
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
    room = self.w(0xFF06AC)
    self.go_to(tid)
    for b in ([push] if push else ["D", "U", "L", "R"]):
        if self.w(0xFF06AC) != room:
            break
        self.press(b, 40, 30)
    self.run(60)
    return self.w(0xFF06AC) != room


Game.exit_via = exit_via


def act(self, verb, tid, quiet=False, wait=30):
    """verb on object `tid` (by scanning the crosshair hit points); recorded in the route"""
    self.leave_pointer()
    pt = self.locate(tid)
    if pt is None:
        print(f"!! {self.name(tid)} ({tid}) not under the crosshair")
        return None
    r = self.try_action(verb, tid, pt, wait=wait, restore=False)
    if not quiet:
        print(f"{verb} {self.name(tid)}: {' | '.join(r['said']) or '-'}", r["story"] or "", r["inv"] or "", r["room"] or "", r["res"][0] if r["res"] and r["res"][0] != "ok" else "")
    return r


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
        self.select_verb(verb)
        self.press("C", 3, 24)                       # inventory view
        page, slot = divmod(idx, 8)
        row, col = divmod(slot, 4)
        self.set_pointer(40 * (col + 1) + 20, 176 if row == 0 else 200)
        res["tgt1"] = self.w(0xFF06AE)
        self.press("B", 4, 6)
        res["wait2"] = self.w(0xFF09DE) & 1
        self.press("C", 3, 24)                       # back to the verb view
        self.set_pointer(*pt)
        res["tgt2"] = self.w(0xFF06B6)
        self.press("B", 4, 10)
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
