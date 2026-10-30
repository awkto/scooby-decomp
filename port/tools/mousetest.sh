#!/bin/sh
# Headless mouse-control tests (GitLab #23, docs/mouse.md).  Run on a VM after building
# build/port:   port/tools/mousetest.sh
# Each test script in port/scripts/mouse/ starts from the Lobby state (lobby.txt route),
# drives the mouse with the script commands mouse/click/wheel, and peeks RAM; the checks
# below read the peek logs.  Every test runs with all C replacements and with none.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
out=runs/mousetest
rm -rf "$out"; mkdir -p "$out"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
P=build/port/scoobyport
fail=0

build/port/mousemap_test || fail=1

{ cat port/scripts/lobby.txt; echo "save $out/lobby.state"; } > "$out/route.txt"
$P --hle none --script "$out/route.txt" --outdir "$out" --frames 0 >/dev/null 2>&1
[ -f "$out/lobby.state" ] || { echo "FAIL no Lobby state"; exit 1; }
# the Cafe with 9 carried items (story route w0_cafe_items + 6 poked items, 3 inventory rows)
{ cat tools/scripts/story/w0_cafe_items.txt port/scripts/inv_items.txt; echo "save $out/inv.state"; } > "$out/route_inv.txt"
$P --hle none --script "$out/route_inv.txt" --outdir "$out" --frames 0 >/dev/null 2>&1
[ -f "$out/inv.state" ] || { echo "FAIL no inventory state"; exit 1; }

for hle in all none; do
	for t in walk look exit bar; do
		d="$out/$hle/$t"
		mkdir -p "$d"
		{ echo "load $out/lobby.state"; cat "port/scripts/mouse/$t.txt"; } > "$d/script.txt"
		SCOOBY_MOUSE_DEBUG=1 $P --hle "$hle" --script "$d/script.txt" --outdir "$d" --frames 0 >"$d/log.txt" 2>&1
	done
	# inventory: page arrows, wheel, item clicks, Use X on Y with both objects in the inventory
	d="$out/$hle/inventory"
	mkdir -p "$d"
	{ echo "load $out/inv.state"; cat port/scripts/mouse/inventory.txt; } > "$d/script.txt"
	SCOOBY_MOUSE_DEBUG=1 $P --hle "$hle" --script "$d/script.txt" --outdir "$d" --frames 0 >"$d/log.txt" 2>&1
	# Use an inventory item on a room object (second object picked in the room view)
	d="$out/$hle/inventory2"
	mkdir -p "$d"
	{ echo "load $out/inv.state"; cat port/scripts/mouse/inventory2.txt; } > "$d/script.txt"
	SCOOBY_MOUSE_DEBUG=1 $P --hle "$hle" --script "$d/script.txt" --outdir "$d" --frames 0 >"$d/log.txt" 2>&1
	# --no-mouse: the same clicks do nothing
	d="$out/$hle/nomouse"
	mkdir -p "$d"
	{ echo "load $out/lobby.state"; cat port/scripts/mouse/exit.txt; } > "$d/script.txt"
	$P --hle "$hle" --no-mouse --script "$d/script.txt" --outdir "$d" --frames 0 >"$d/log.txt" 2>&1
	# soak: 60 random left clicks on the room view (walks, Looks at things, exits, text)
	d="$out/$hle/soak"
	mkdir -p "$d"
	{ echo "load $out/lobby.state"; python3 -c "
import random
r = random.Random(${MOUSE_SOAK_SEED:-23})
for i in range(60):
    print('click L %d %d' % (r.randrange(256), r.randrange(16, 168)))
    print('run %d' % r.choice((60, 200, 400)))
print('peek room FF06AC')"; } > "$d/script.txt"
	SCOOBY_MOUSE_DEBUG=1 $P --hle "$hle" --script "$d/script.txt" --outdir "$d" --frames 0 >"$d/log.txt" 2>&1
	# title -> Lobby by clicking, then the host cursor in a window shot (--scale 3)
	d="$out/$hle/title"
	mkdir -p "$d"
	$P --hle "$hle" --script port/scripts/mouse/title.txt --outdir "$d" --frames 0 --scale 3 \
		--window-shot "$d/window.ppm" >"$d/log.txt" 2>&1
done

python3 - "$out" <<'EOF' || fail=1
import sys
out = sys.argv[1]
fails = 0

def peeks(path):
    d = {}
    try:
        for line in open(path):
            f, lab, v = line.split()
            d[lab] = int(v, 16)
    except FileNotFoundError:
        pass
    return d

def check(name, cond, info):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ": " + info)
    if not cond:
        fails += 1

def s16(v):
    return v - 0x10000 if v & 0x8000 else v

for hle in ("all", "none"):
    p = peeks(f"{out}/{hle}/walk/peek.log")
    # click (66,66) / (40,150): world = screen + camera, y - 16 (room view starts at line 16)
    ok = True
    info = []
    for i, (sx, sy) in ((1, (66, 66)), (2, (40, 150))):
        tx, ty = sx + p.get(f"camx{i}", 0), sy - 16 + p.get(f"camy{i}", 0)
        x, y = s16(p.get(f"sx{i}", 0)), s16(p.get(f"sy{i}", 0))
        good = abs(x - tx) <= 3 and abs(y - ty) <= 3 and not (p.get(f"stop{i}", 1) & 1)
        ok &= good
        info.append(f"walk {i} to ({tx},{ty}) ended at ({x},{y}){'' if good else ' <-'}")
    ok &= bool(p.get("moving1", 0) & 1) and p.get("room") == 16
    check(f"[{hle}] click-to-walk across the Lobby", ok, "; ".join(info))

    p = peeks(f"{out}/{hle}/look/peek.log")
    check(f"[{hle}] hover opens pointer mode", bool(p.get("open", 0) & 8), f"$FF09DC={p.get('open', 0):02x}")
    check(f"[{hle}] click on LOOK selects the verb", p.get("verb") == 10, f"$FF06C0={p.get('verb')}")
    ran = any(p.get(k, 0) & 0x80 for k in ("act1", "act2", "act3"))
    check(f"[{hle}] click on the window runs LOOK", ran and p.get("actverb") == 10,
          f"$FF09DC={p.get('act1', 0):02x}/{p.get('act2', 0):02x}/{p.get('act3', 0):02x} $FF06BE={p.get('actverb')}")
    check(f"[{hle}] click closes the text", not (p.get("after", 0x80) & 0x80), f"$FF09DC={p.get('after', 0):02x}")
    ract = [p.get(f"ract{i}", 0) for i in range(1, 5)]
    check(f"[{hle}] right click = LOOK at the rug", any(v & 0x80 for v in ract) and p.get("ractverb") == 10
          and p.get("rtarget") == 35 + 3,
          f"$FF09DC={'/'.join('%02x' % v for v in ract)} $FF06BE={p.get('ractverb')} $FF06AE={p.get('rtarget')}")

    p = peeks(f"{out}/{hle}/exit/peek.log")
    check(f"[{hle}] click on the top door walks into the Hallway", p.get("room") == 9 and bool(p.get("moving", 0) & 1),
          f"room {p.get('room')}")

    p = peeks(f"{out}/{hle}/bar/peek.log")
    check(f"[{hle}] arrow icon -> inventory, left arrow -> verbs",
          bool(p.get("inv1", 0) & 0x10) and not (p.get("inv2", 0x10) & 0x10),
          f"$FF09DC {p.get('inv1', 0):02x} -> {p.get('inv2', 0):02x}")
    check(f"[{hle}] D-pad takes over from the mouse", bool(p.get("ptr", 0) & 8) and not (p.get("kptr", 8) & 8)
          and s16(p.get("x1", 0)) > s16(p.get("x0", 0)) + 10,
          f"pointer {p.get('ptr', 0):02x} -> {p.get('kptr', 0):02x}, x {s16(p.get('x0', 0))} -> {s16(p.get('x1', 0))}")
    check(f"[{hle}] a 1-px mouse bump does not take over", "bump" in p and not (p["bump"] & 8),
          f"$FF09DC={p.get('bump', 0):02x}")

    p = peeks(f"{out}/{hle}/inventory/peek.log")
    check(f"[{hle}] inventory view, hover reports the item under the pointer",
          bool(p.get("inv1", 0) & 0x10) and p.get("hover1") == 124 + 3 and p.get("hover2") == 121 + 3,
          f"$FF09DC={p.get('inv1', 0):02x} hover {p.get('hover1')} {p.get('hover2')}")
    check(f"[{hle}] page arrows: down, down, down (stays), up",
          [p.get(k) for k in ("page0", "page1", "page2", "page3", "page4")] == [0, 1, 2, 2, 1],
          "pages " + " ".join(str(p.get(k)) for k in ("page0", "page1", "page2", "page3", "page4")))
    check(f"[{hle}] paging moves one row: slots show other items, nothing past the end",
          p.get("hover3") == 96 + 3 and p.get("hover4") == 0, f"hover {p.get('hover3')} {p.get('hover4')}")
    check(f"[{hle}] wheel pages the inventory",
          [p.get(k) for k in ("wpage1", "wpage2", "wpage3", "wpage4")] == [2, 1, 0, 0],
          "pages " + " ".join(str(p.get(k)) for k in ("wpage1", "wpage2", "wpage3", "wpage4")))
    check(f"[{hle}] click on an item runs its default verb (LOOK) on it",
          bool(p.get("act1", 0) & 0x80) and p.get("actverb") == 10 and p.get("acttarget") == 124 + 3,
          f"$FF09DC={p.get('act1', 0):02x} $FF06BE={p.get('actverb')} $FF06AE={p.get('acttarget')}")
    check(f"[{hle}] Use Can Opener on Chili: first object waits for the second",
          p.get("verb") == 5 and bool(p.get("wait", 0) & 1) and p.get("first") == 124 + 3,
          f"verb {p.get('verb')} $FF09DE={p.get('wait', 0):02x} first {p.get('first')}")
    inv = [p.get(f"inv{i}") for i in range(4)]
    check(f"[{hle}] second object (also an item) runs the action; the Chili is used up",
          p.get("second") == 100 + 3 and bool(p.get("act2", 0) & 0x80) and p.get("actverb2") == 5
          and 100 not in inv and not (p.get("done", 0x80) & 0x80),
          f"$FF06B6={p.get('second')} verb {p.get('actverb2')} list {inv}")

    p = peeks(f"{out}/{hle}/inventory2/peek.log")
    check(f"[{hle}] Use Pot on a room object: the item waits, the room click is the second object",
          p.get("verb") == 5 and bool(p.get("inv", 0) & 0x10) and bool(p.get("wait", 0) & 1)
          and p.get("first") == 121 + 3 and p.get("second") == 125 and p.get("hover2") == 125
          and p.get("actverb") == 5 and bool(p.get("act", 0) & 0x80)
          and not (p.get("done", 0x80) & 0x80) and not (p.get("wait2", 1) & 1),
          f"verb {p.get('verb')} first {p.get('first')} second {p.get('second')} $FF06BE={p.get('actverb')} "
          f"$FF09DC {p.get('act', 0):02x}->{p.get('done', 0):02x} $FF09DE {p.get('wait', 0):02x}->{p.get('wait2', 0):02x}")

    p = peeks(f"{out}/{hle}/nomouse/peek.log")
    check(f"[{hle}] --no-mouse ignores the mouse", p.get("room") == 16 and not (p.get("moving", 0) & 1),
          f"room {p.get('room')}")

    # soak: every walk that ends by itself ends on its goal; none is lost
    import re
    ev = {}
    bad = []
    for line in open(f"{out}/{hle}/soak/log.txt"):
        m = re.match(r"mouse \d+: walk (\w+(?: room)?) at \((-?\d+),(-?\d+)\), goal \((-?\d+),(-?\d+)\)", line)
        if m:
            k = m.group(1)
            ev[k] = ev.get(k, 0) + 1
            x, y, gx, gy = map(int, m.groups()[1:])
            if k == "arrived" and (abs(x - gx) > 2 or abs(y - gy) > 2):
                bad.append(line.strip())
    check(f"[{hle}] soak: 60 random clicks", ev.get("arrived", 0) >= 10 and not bad and not ev.get("superseded"),
          f"walk events {ev}" + (f"; off target: {bad[:3]}" if bad else ""))

    p = peeks(f"{out}/{hle}/title/peek.log")
    check(f"[{hle}] title -> Lobby with clicks", p.get("room") == 16 and bool(p.get("ptr", 0) & 8),
          f"room {p.get('room')}, $FF09DC={p.get('ptr', 0):02x}")
    # window 960x672, game image 768x672 at x 96 (3x): arrow tip at game (60,185) is black,
    # (61,187) is inside the arrow (white)
    try:
        d = open(f"{out}/{hle}/title/window.ppm", "rb").read()
        hdr = d.split(b"\n", 3)
        w, h = map(int, hdr[1].split())
        pix = hdr[3]
        def px(x, y):
            i = 3 * (y * w + x)
            return tuple(pix[i:i + 3])
        tip, fill = px(96 + 3 * 60 + 1, 3 * 185 + 1), px(96 + 3 * 61 + 1, 3 * 187 + 1)
        check(f"[{hle}] host cursor drawn at the mouse (window shot)", tip == (0, 0, 0) and min(fill) > 240,
              f"tip {tip} fill {fill}")
    except (OSError, ValueError, IndexError) as e:
        check(f"[{hle}] host cursor drawn at the mouse (window shot)", False, str(e))
sys.exit(1 if fails else 0)
EOF
[ $fail = 0 ] && echo "mousetest: all passed" || echo "mousetest: FAILURES"
exit $fail
