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

for hle in all none; do
	for t in walk look exit bar; do
		d="$out/$hle/$t"
		mkdir -p "$d"
		{ echo "load $out/lobby.state"; cat "port/scripts/mouse/$t.txt"; } > "$d/script.txt"
		SCOOBY_MOUSE_DEBUG=1 $P --hle "$hle" --script "$d/script.txt" --outdir "$d" --frames 0 >"$d/log.txt" 2>&1
	done
	# --no-mouse: the same clicks do nothing
	d="$out/$hle/nomouse"
	mkdir -p "$d"
	{ echo "load $out/lobby.state"; cat port/scripts/mouse/exit.txt; } > "$d/script.txt"
	$P --hle "$hle" --no-mouse --script "$d/script.txt" --outdir "$d" --frames 0 >"$d/log.txt" 2>&1
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
    check(f"[{hle}] right click = LOOK at the rug", bool(p.get("ract", 0) & 0x80) and p.get("ractverb") == 10
          and p.get("rtarget") == 35 + 3, f"$FF09DC={p.get('ract', 0):02x} $FF06BE={p.get('ractverb')} $FF06AE={p.get('rtarget')}")

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

    p = peeks(f"{out}/{hle}/nomouse/peek.log")
    check(f"[{hle}] --no-mouse ignores the mouse", p.get("room") == 16 and not (p.get("moving", 0) & 1),
          f"room {p.get('room')}")
sys.exit(1 if fails else 0)
EOF
[ $fail = 0 ] && echo "mousetest: all passed" || echo "mousetest: FAILURES"
exit $fail
