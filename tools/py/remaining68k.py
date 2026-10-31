#!/usr/bin/env python3
"""docs/remaining68k.md generator (GitLab #24 groundwork): the 68k code that still runs on the emulated CPU
when every C replacement is on (--hle all), measured over the gameplay routes, as a ranked checklist.

    port/tools/ipprof.sh                                   # on a VM: runs the routes, writes runs/ipprof/*.txt
    tools/vm/vmpull VM runs/ipprof/ runs/ipprof/
    .venv/bin/python -I tools/py/remaining68k.py [runs/ipprof] > docs/remaining68k.md

Inputs: the profiles written by src/ipprof.c (SCOOBY_IPPROF), runs/ipprof/hle-list.txt (`scoobyport
--list-hle`), data/functions.txt, data/exec_starts.txt (merged harness coverage) and docs/subsystems/*.md
(owner and the "stays 68k" reasons).  Only profiles of runs with the spec `all` (or the scoobyport default,
which is all) count as the interpreted profile; profiles of `--hle none` runs of the same gates give the
baseline the percentages are computed against.
"""
import glob
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
IDLE = ("wait_vblank", "wait_vblanks")        # the idle spin: instruction count depends on emulation speed only

# ------------------------------------------------------------------ the by-design classification
# category -> (title, why it stays 68k, how it disappears in a CPU-less build); used for the design section
# and for the "reason" column.  Entries below map function (name or hex address) -> category.
CAT = {
    "wait": "wait primitive",
    "hv": "HV-counter reader",
    "vector": "vector trampoline",
    "stack": "stack trick",
    "nostates": "no states / no route",
    "thin": "thin glue (calls only)",
    "spin": "spin on a flag",
    "dead": "dead / never runs",
    "notported": "not ported yet",
}
BY_DESIGN = {
    # wait primitives
    0x974C: "wait", 0x9742: "wait",
    # HV counter readers
    0x5C8E: "hv", 0xA248: "hv", 0xAF24: "hv", 0x1006C: "hv",
    # vector trampolines
    0x8AA: "vector", 0x8BC: "vector", 0x892: "dead",
    # lockout screen / region print: never runs on a correct region
    0x400: "dead",
}


# one-line notes on what a function is, for the table (address -> text); only for the ones that matter
NOTES = {
    0xE40C: "software scaled-bitmap blit into the tile buffer (depth masks), title bats zoom / menu pictures; pure computation",
    0xF272: "same blit, plane-A variant; pure computation",
    0x1006C: "HBlank verb-bar split: raster wait + plane/scroll register switch at line 167",
    0xA704: "HUD status meters (draw into VRAM)",
    0xFA78: "camera -> scroll registers, column/row refresh requests",
    0xA7CE: "palette cycling (5 ranges)",
}


def parse_functions():
    funcs = []
    for line in open(os.path.join(ROOT, "data/functions.txt")):
        if line.startswith("#") or not line.strip():
            continue
        main, _, comment = line.partition("#")
        f = main.split()
        entry, size, name = int(f[0], 16), int(f[1]), f[2]
        m = re.match(r"\s*(\d+) insns(.*)", comment)
        ninsn = int(m.group(1)) if m else 0
        ranges = [(entry, entry + size)]
        if m:
            for p in re.finditer(r"\+([0-9a-f]+):(\d+)", m.group(2)):
                ranges.append((int(p.group(1), 16), int(p.group(1), 16) + int(p.group(2))))
        funcs.append(dict(entry=entry, name=name, size=sum(b - a for a, b in ranges), ninsn=ninsn, ranges=ranges))
    return funcs


def build_index(funcs):
    idx = []
    for i, f in enumerate(funcs):
        for a, b in f["ranges"]:
            idx.append((a, b, i))
    idx.sort()
    starts = [x[0] for x in idx]
    import bisect

    def find(addr):
        k = bisect.bisect_right(starts, addr) - 1
        if k >= 0 and idx[k][0] <= addr < idx[k][1]:
            return idx[k][2]
        return -1
    return find


def parse_hle_list(path):
    """`$0097AA  lzss_decompress  on  cyc=0  /path/decomp.c` -> [(addr, name, file)]"""
    defs = []
    if not os.path.exists(path):
        return defs
    for line in open(path):
        m = re.match(r"\$([0-9A-Fa-f]+)\s+(\S+)\s+(on|off)\s+cyc=\S+\s+(\S+)", line)
        if m:
            defs.append((int(m.group(1), 16), m.group(2), os.path.basename(m.group(4))))
    return defs


def parse_profile(path):
    hle = None
    frames = 0
    top, nest = defaultdict(int), defaultdict(int)
    callers = defaultdict(lambda: defaultdict(int))     # addr -> caller -> n
    with open(path) as fh:
        first = fh.readline()
        if not first.startswith("# ipprof"):
            return None
        for line in fh:
            f = line.split()
            if f[0] == "hle":
                hle = f[1]
            elif f[0] == "frames":
                frames = int(f[1])
            elif f[0] == "T":
                top[int(f[1], 16)] += int(f[2])
            elif f[0] == "N":
                nest[int(f[1], 16)] += int(f[2])
            elif f[0] == "C":
                callers[int(f[2], 16)][f[1]] += int(f[3])
    return dict(hle=hle, frames=frames, top=top, nest=nest, callers=callers)


def load_profiles(d):
    runs = []
    for p in sorted(glob.glob(os.path.join(d, "*.txt"))):
        pr = parse_profile(p)
        if pr:
            pr["gate"] = os.path.basename(p).split(".")[0]
            runs.append(pr)
    return runs


def scan_docs():
    """address -> list of (doc, line) over docs/subsystems/*.md, plus the 'stays 68k' sentences per address."""
    idx = defaultdict(list)
    for p in sorted(glob.glob(os.path.join(ROOT, "docs/subsystems/*.md"))):
        doc = os.path.basename(p)[:-3]
        for line in open(p):
            for m in re.finditer(r"\$([0-9A-Fa-f]{4,6})\b", line):
                idx[int(m.group(1), 16)].append((doc, line.strip()))
    return idx


STAY = re.compile(r"stays? 68k|stay in 68k|stays? in 68k|not ported|no states|not registered|never runs|dead code|"
                  r"cannot be difftested|nothing but calls|HV rule|wait primitives|remaining 68k|still 68k", re.I)


def load_rom():
    p = os.path.join(ROOT, "rom", "baserom.md")
    return open(p, "rb").read() if os.path.exists(p) else None


def residue(rom, addrs):
    """What the interpreted instructions of a function are made of (capstone over the ROM), as a set of features:
    wait (calls wait_vblank/wait_vblanks), call (other bsr/jsr), stack (movem / push / pop / pea / lea n(sp)),
    sr (writes SR), hv (reads the HV counter), vdp (VDP ports), jump (jmp (an)/ rte / tail to the reset vector)."""
    feats = set()
    if rom is None:
        return None
    try:
        from capstone import Cs, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_000
    except ImportError:
        return None
    md = residue.md = getattr(residue, "md", None) or Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_000)
    for a in addrs:
        for ins in md.disasm(rom[a:a + 10], a, 1):
            m, o = ins.mnemonic, ins.op_str.lower()
            if m.startswith(("bsr", "jsr")):
                feats.add("wait" if ("0x974c" in o or "0x9742" in o) else "call")
            elif m.startswith("movem") or m.startswith("pea") or "-(a7)" in o or "(a7)+" in o or o.startswith("(a7)") \
                    or ", a7" in o or "(0x" in o and "(a7)" in o:
                feats.add("stack")
            if "sr" in o.split(",")[-1:] or o.endswith(" sr") or o == "sr" or o.endswith(", sr"):
                if m.startswith(("move", "andi", "ori", "eori")):
                    feats.add("sr")
            if "0xc00008" in o:
                feats.add("hv")
            elif "0xc00000" in o or "0xc00004" in o:
                feats.add("vdp")
            if m.startswith(("rte", "jmp")):
                feats.add("jump")
    return feats


FEAT_TEXT = {
    "wait": "calls wait_vblank (blocks)", "call": "calls other 68k routines",
    "stack": "stack push/pop", "sr": "writes SR (interrupt mask)", "hv": "reads the HV counter",
    "vdp": "writes VDP ports", "jump": "rte / jmp",
}


def clean(s, n=150):
    s = re.sub(r"\s+", " ", s.replace("|", "/")).strip(" /-")
    return s if len(s) <= n else s[:n - 1].rstrip() + "..."


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "runs/ipprof")
    funcs = parse_functions()
    find = build_index(funcs)
    defs = parse_hle_list(os.path.join(d, "hle-list.txt"))
    runs = load_profiles(d)
    docs = scan_docs()

    allp = [r for r in runs if r["hle"] in ("all", "default")]
    nonep = [r for r in runs if r["hle"] == "none"]
    if not allp:
        sys.exit("no --hle all profiles in %s" % d)

    # which functions have C inside (entry or a range)
    c_in = defaultdict(list)           # func idx -> [(addr, name, file)]
    for a, n, fl in defs:
        i = find(a)
        if i >= 0:
            c_in[i].append((a, n, fl))

    # ---------------------------------------------------------------- aggregate
    top, nest = defaultdict(int), defaultdict(int)
    callers = defaultdict(lambda: defaultdict(int))
    per_gate = defaultdict(lambda: dict(top=0, nest=0, idle=0, frames=0, runs=0))
    func_gates = defaultdict(set)
    fn_top, fn_nest = defaultdict(int), defaultdict(int)
    fn_addrs = defaultdict(set)
    unattr = [0]
    for r in allp:
        g = per_gate[r["gate"]]
        g["runs"] += 1
        g["frames"] += r["frames"]
        for kind, tab, ftab in (("top", r["top"], fn_top), ("nest", r["nest"], fn_nest)):
            for a, n in tab.items():
                i = find(a)
                (top if kind == "top" else nest)[a] += n
                g[kind] += n
                if i < 0:
                    unattr[0] += n
                    continue
                ftab[i] += n
                fn_addrs[i].add(a)
                func_gates[i].add(r["gate"])
                if funcs[i]["name"] in IDLE:
                    g["idle"] += n
        for a, cs in r["callers"].items():
            for c, n in cs.items():
                callers[a][c] += n

    # ---------------------------------------------------------------- baseline (--hle none) per gate
    base = defaultdict(lambda: dict(n=0, idle=0, frames=0))
    for r in nonep:
        b = base[r["gate"]]
        b["frames"] += r["frames"]
        for tab in (r["top"], r["nest"]):
            for a, n in tab.items():
                b["n"] += n
                i = find(a)
                if i >= 0 and funcs[i]["name"] in IDLE:
                    b["idle"] += n

    out = []
    w = out.append
    tot_top, tot_nest = sum(top.values()), sum(nest.values())
    idle_all = sum(g["idle"] for g in per_gate.values())
    frames = sum(g["frames"] for g in per_gate.values())
    w("# The 68k that still runs with `--hle all` (GitLab #24)\n")
    w("Generated by `tools/py/remaining68k.py` from the profiles of `port/tools/ipprof.sh` "
      "(`SCOOBY_IPPROF`, `port/src/ipprof.c`); do not edit by hand. Regenerate:\n")
    w("```\nSCOOBY_RENDERER=native port/tools/ipprof.sh            # on a VM, ~10 min: every route with --hle all (and none)\n"
      "tools/vm/vmpull $vm runs/ipprof/ runs/ipprof/\n"
      ".venv/bin/python -I tools/py/remaining68k.py runs/ipprof > docs/remaining68k.md\n```\n")
    w("**Status (#24 done, docs/native.md):** the waits, the vector trampolines, the glue between C ranges and the "
      "last functions with no C are C blocks, and the native build `scooby` runs every route with no 68k core. "
      "This report is the measurement of phase 1: with `--hle all` on the emulated build, the tables below should "
      "show no interpreted instruction on any gate route. Anything listed is a gap the native build would stop at "
      "(\"no C block\"). Routes that start from a `--hle none` savestate (the mouse gate's states) can resume in "
      "the middle of 68k code; port/tools/ipprof.sh makes those states with --hle all.\n")
    w("Goal of #24: a build without the 68k CPU. Everything counted here is code that would still need a C "
      "(or native) replacement, or a design change, first. The counts are **executed 68k instructions** (every "
      "instruction the emulated CPU runs, per address, attributed to its function in `data/functions.txt`). "
      "*Top level* = the CPU runs on its own (the main loop code that is not yet C, interrupt vectors and "
      "everything they call that is not yet C). *Nested* = 68k called from a C function through "
      "`hle_call68k` / `hle_run68k_until`.\n")

    # ---------------------------------------------------------------- 1. summary
    w("## 1. How much is still interpreted\n")
    w("Routes (each with `--hle all`; instructions are summed over all of them):\n")
    w("| gate | routes / profiles | frames | top level | nested | total | of which idle wait spin |")
    w("|---|---|---:|---:|---:|---:|---:|")
    core = [g for g in per_gate if not g.startswith("extra")]
    for g in sorted(per_gate, key=lambda g: (g.startswith("extra"), g)):
        s = per_gate[g]
        w("| %s | %d | %s | %s | %s | %s | %s |" % (g, s["runs"], f"{s['frames']:,}", f"{s['top']:,}", f"{s['nest']:,}",
                                                    f"{s['top'] + s['nest']:,}", f"{s['idle']:,}"))
    w("")
    core_top = sum(per_gate[g]["top"] for g in core)
    core_nest = sum(per_gate[g]["nest"] for g in core)
    core_idle = sum(per_gate[g]["idle"] for g in core)
    core_work = core_top + core_nest - core_idle
    w("`extra_*` are other recorded routes (lobby, boot, verbs, inventory, pointer, ...) that are not part of the "
      "#24 route set; they are in the function table but not in the headline numbers below.\n")
    w("**Idle wait spin.** `wait_vblank` / `wait_vblanks` (`$974C`, `$9742`) are a 3-instruction poll loop. How "
      "many times it runs depends on how long the C code takes in wall-clock-free emulated time, not on game "
      "logic, so it is reported separately and left out of \"work\" below. In a CPU-less build it is zero.\n")
    w("| | instructions | share of work |")
    w("|---|---:|---:|")
    w("| top level (excluding idle spin) | %s | %.1f %% |" % (f"{core_top - core_idle:,}", 100.0 * (core_top - core_idle) / max(core_work, 1)))
    w("| nested in C functions | %s | %.1f %% |" % (f"{core_nest:,}", 100.0 * core_nest / max(core_work, 1)))
    w("| **work total** | **%s** | |" % f"{core_work:,}")
    w("| idle wait spin (excluded) | %s | |" % f"{core_idle:,}")
    w("")
    # percentage of the original's work: all vs none per gate
    w("**Share of the original's executed instructions that is still interpreted.** The same gates also ran with "
      "`--hle none` (pure 68k); the ratio is `all / none`, both without the idle spin:\n")
    w("| gate | none (instructions) | all (instructions) | still interpreted | all: top / nested |")
    w("|---|---:|---:|---:|---|")
    sn = sa = 0
    for g in sorted(core):
        if g == "mouse" or g not in base or not base[g]["n"]:
            continue
        a = per_gate[g]
        wa = a["top"] + a["nest"] - a["idle"]
        wn = base[g]["n"] - base[g]["idle"]
        sn += wn
        sa += wa
        w("| %s | %s | %s | %.2f %% | %s / %s |" % (g, f"{wn:,}", f"{wa:,}", 100.0 * wa / max(wn, 1),
                                                    f"{a['top'] - a['idle']:,}", f"{a['nest']:,}"))
    if sn:
        w("| **all gates above** | %s | %s | **%.2f %%** | |" % (f"{sn:,}", f"{sa:,}", 100.0 * sa / sn))
    w("")
    if sn:
        big = [i for i, f in enumerate(funcs) if f["entry"] in (0xE40C, 0xF272)]
        bn = 0
        for r in allp:
            if r["gate"] in core and r["gate"] != "mouse":
                for a, n in r["nest"].items():
                    if find(a) in big:
                        bn += n
        w("The two scaled-bitmap blit routines `sub_00E40C` / `sub_00F272` (title bats and menu pictures, called "
          "every frame of the menu screens; pure computation) are %s of those %s interpreted instructions. "
          "Without them the remainder is **%.2f %%** of the original's instructions.\n" % (
              "%.1f %%" % (100.0 * bn / sa), f"{sa:,}", 100.0 * (sa - bn) / sn))
    w("Caveats: the `none` and `all` runs of a gate execute the same inputs, but a C function costs about zero "
      "emulated time, so closed-loop routes (story) and the main loop (which runs as often as time allows) do a "
      "different amount of main-loop work. `mouse` is left out of the ratio because its `none` side includes the "
      "Lobby set-up run. Treat the ratio as an order of magnitude, the table in section 3 as the ranking.\n")

    # ---------------------------------------------------------------- 2. per-function table
    rows = []
    for i, f in enumerate(funcs):
        t, n = fn_top.get(i, 0), fn_nest.get(i, 0)
        if not (t or n):
            continue
        rows.append((t + n, i, t, n))
    rows.sort(reverse=True)

    rom = load_rom()
    work_all = sum(fn_top.values()) + sum(fn_nest.values()) - sum(
        fn_top.get(i, 0) + fn_nest.get(i, 0) for i, f in enumerate(funcs) if f["name"] in IDLE)

    def doc_text(e):
        for doc, line in docs.get(e, []):
            if STAY.search(line):
                cells = [c for c in line.split("|") if STAY.search(c)]
                return clean(cells[0] if cells else line, 120)
        return ""

    def classify(i):
        """-> (class, status, reason, owner)"""
        f = funcs[i]
        e = f["entry"]
        cs = c_in.get(i, [])
        feats = residue(rom, fn_addrs[i])
        ft = ", ".join(FEAT_TEXT[k] for k in ("wait", "stack", "sr", "hv", "vdp", "jump", "call") if feats and k in feats)
        owner = ""
        if cs:
            owner = sorted({c[2] for c in cs})[0].replace(".c", "")
        else:
            cnt = defaultdict(int)
            for doc, _ in docs.get(e, []):
                cnt[doc] += 1
            if cnt:
                owner = max(cnt, key=cnt.get)
        dt = doc_text(e)
        if f["name"] in IDLE:
            return "idle", "stays 68k by design: wait primitive", "blocks until VBlank; the spin is pure idle time", owner
        cat = BY_DESIGN.get(e)
        if cat:
            why = {"hv": "reads the HV counter (beam position / RNG)", "vector": "interrupt vector trampoline",
                   "dead": "never runs on a correct run"}[cat]
            return "design-" + cat, "stays 68k by design: " + CAT[cat], why, owner
        if cs:
            why = ("what stays 68k between its C ranges: " + ft) if ft else "residue between C ranges"
            if dt:
                why = dt
            return "partial", "partly C (%d entries)" % len(cs), why, owner
        hard = feats & {"wait", "stack", "sr", "hv"} if feats is not None else set()
        if hard:
            return "noC-hard", "no C: blocks / stack / interrupt state", ft or dt, owner
        if feats is not None and "vdp" in feats:
            return "noC-vdp", "no C: writes VDP ports", (dt + "; " if dt else "") + ft, owner
        why = dt or ("pure computation: no waits, stack tricks or hardware" if feats is not None and not feats - {"call"}
                     else ft or "-")
        return "noC-easy", "no C", why, owner

    classes = defaultdict(lambda: [0, 0, 0])      # class -> [top, nested, functions]
    info = {}
    for tot, i, t, n in rows:
        info[i] = classify(i)
        c = classes[info[i][0]]
        c[0] += t
        c[1] += n
        c[2] += 1
    CLASS_TEXT = [
        ("idle", "idle wait spin (not work)", "section 5.1"),
        ("noC-easy", "no C yet, no hardware / waits / stack tricks: ordinary decompilation", "port it, difftest, done"),
        ("noC-vdp", "no C yet, writes VDP ports (DMA, planes)", "port with the DMA bracket rules (porting.md)"),
        ("noC-hard", "no C yet, blocks / pushes / reads HV or SR", "needs the waiting model first (5.1)"),
        ("partial", "partly C: what stays 68k between its ranges", "disappears with the waiting model (5.1)"),
        ("design-hv", "by design: HV-counter readers", "sections 5.2 / 5.3"),
        ("design-vector", "by design: interrupt vector trampolines", "section 5.4"),
        ("design-dead", "dead / never runs on a correct run", "delete"),
    ]
    w("## 2. Where the remaining work is, by kind\n")
    w("Every function that executes interpreted is put in one class (rules in `classify()`; the features come from "
      "disassembling the interpreted instructions themselves). Over all routes incl. extras, `--hle all`:\n")
    w("| kind | functions | top | nested | % of work (idle excluded) | what to do |")
    w("|---|---:|---:|---:|---:|---|")
    for key, text, todo in CLASS_TEXT:
        c = classes.get(key)
        if not c:
            continue
        pct = "idle" if key == "idle" else "%.2f %%" % (100.0 * (c[0] + c[1]) / max(work_all, 1))
        w("| %s | %d | %s | %s | %s | %s |" % (text, c[2], f"{c[0]:,}", f"{c[1]:,}", pct, todo))
    w("")

    w("## 3. Remaining functions, ranked by executed instructions\n")
    w("Over the #24 route set plus extras (all routes with `--hle all`). *Top* / *nested* = instructions executed at "
      "top level / inside a C function. *Cum* = cumulative share of the work. *Body* = distinct instructions of the "
      "function executed interpreted / instructions in the function (a partly-C function shows only what is left). "
      "*Called from* = the C functions that call it (nested part, most instructions first). *Owner* = the C file "
      "that replaces part of it, else the subsystem doc that describes it. The idle spin of `wait_vblank` is "
      "ranked first because it is the biggest count but it is not work.\n")
    w("| # | function | addr | top | nested | % | cum % | body | owner | status | why it is still 68k | called from (nested) |")
    w("|--:|---|---|---:|---:|---:|---:|---|---|---|---|---|")
    cum = 0.0
    for rank, (tot, i, t, n) in enumerate(rows, 1):
        f = funcs[i]
        e = f["entry"]
        cls, status, why, owner = info[i]
        idle = cls == "idle"
        pct = 100.0 * (t + n) / max(work_all, 1)
        if not idle:
            cum += pct
        cl = defaultdict(int)
        for a in fn_addrs[i]:
            for c, k in callers.get(a, {}).items():
                cl[c] += k
        cl_s = ", ".join("%s (%s)" % (c, f"{k:,}") for c, k in sorted(cl.items(), key=lambda x: -x[1])[:3])
        body = "%d/%d" % (len(fn_addrs[i]), f["ninsn"]) if f["ninsn"] else "%d/?" % len(fn_addrs[i])
        note = NOTES.get(e)
        if note:
            why = note + ("; " + why if why and why != "-" and not why.startswith("pure computation") else "")
        w("| %d | `%s` | $%06X | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            rank, f["name"], e, f"{t:,}" if t else "", f"{n:,}" if n else "",
            "idle" if idle else "%.2f" % pct, "" if idle else "%.1f" % cum, body, owner, status, clean(why, 150), cl_s))
    w("")
    w("(%d functions execute interpreted in at least one route.)\n" % len(rows))

    # ---------------------------------------------------------------- 3. static view
    covered = set()
    ex_path = os.path.join(ROOT, "data/exec_starts.txt")
    for line in open(ex_path):
        if line.startswith("#") or not line.strip():
            continue
        covered.add(int(line.split()[0], 16))
    cov_by_fn = defaultdict(int)
    for a in covered:
        i = find(a)
        if i >= 0:
            cov_by_fn[i] += 1
    noC = [i for i in range(len(funcs)) if i not in c_in]
    run_exec = {i for i in noC if fn_top.get(i, 0) or fn_nest.get(i, 0)}
    never_routes = [i for i in noC if i not in run_exec]
    never_any = [i for i in never_routes if not cov_by_fn.get(i)]
    only_other = [i for i in never_routes if cov_by_fn.get(i)]
    w("## 4. Static view: functions with no C at all\n")
    w("`data/functions.txt` has **%d** functions. **%d** have a C replacement (entry or range: %d HLE entries "
      "registered), **%d** have none.\n" % (len(funcs), len(c_in), len(defs), len(noC)))
    w("| of the %d functions with no C | count |" % len(noC))
    w("|---|---:|")
    w("| execute interpreted on the #24/extra routes | %d |" % len(run_exec))
    w("| never execute on these routes, but did in the merged harness coverage (`data/exec_starts.txt`: password "
      "warps, crawler, random play) | %d |" % len(only_other))
    w("| never execute on any route or in the merged coverage (**coverage gaps**) | %d |" % len(never_any))
    w("")
    w("Code size of the 'no C' functions: %d bytes; of the gaps: %d bytes.\n" % (
        sum(funcs[i]["size"] for i in noC), sum(funcs[i]["size"] for i in never_any)))

    def owner_of(i):
        e = funcs[i]["entry"]
        cnt = defaultdict(int)
        for doc, _ in docs.get(e, []):
            cnt[doc] += 1
        if cnt:
            return max(cnt, key=cnt.get)
        # no doc mentions it: take the owner of the nearest function before it (same code block), marked as a guess
        for j in range(i - 1, max(i - 4, -1), -1):
            if funcs[j]["entry"] > funcs[i]["entry"] - 0x600:
                c2 = defaultdict(int)
                for doc, _ in docs.get(funcs[j]["entry"], []):
                    c2[doc] += 1
                if c2:
                    return max(c2, key=c2.get) + " (adjacent)"
        return "-"

    w("### 4a. Never executed anywhere (coverage gaps)\n")
    w("These cannot be tested by any route yet, so a C port of them has no ground truth; most are script "
      "sub-operations that need a story state (docs/coverage.md) or debug editors.\n")
    w("| function | addr | bytes | insns | subsystem doc |")
    w("|---|---|---:|---:|---|")
    for i in sorted(never_any, key=lambda i: -funcs[i]["size"]):
        f = funcs[i]
        w("| `%s` | $%06X | %d | %d | %s |" % (f["name"], f["entry"], f["size"], f["ninsn"], owner_of(i)))
    w("")
    w("### 4b. Executed in the merged harness coverage, not on the #24 routes\n")
    w("| function | addr | bytes | subsystem doc |")
    w("|---|---|---:|---|")
    for i in sorted(only_other, key=lambda i: -funcs[i]["size"]):
        f = funcs[i]
        w("| `%s` | $%06X | %d | %s |" % (f["name"], f["entry"], f["size"], owner_of(i)))
    w("")

    # ---------------------------------------------------------------- 4. design
    w(DESIGN_HEAD)
    # measured numbers for the by-design items
    def meas(e):
        i = find(e)
        if i < 0:
            return 0, 0
        return fn_top.get(i, 0), fn_nest.get(i, 0)
    w("Measured on the routes above (`--hle all`):\n")
    w("| item | addr | top | nested | % of work |")
    w("|---|---|---:|---:|---:|")
    for name, e in DESIGN_ITEMS:
        t, n = meas(e)
        pct = ("%.2f" % (100.0 * (t + n) / max(work_all, 1))) if name not in IDLE else "idle"
        w("| `%s` | $%06X | %s | %s | %s |" % (name, e, f"{t:,}", f"{n:,}", pct))
    w("")
    w(DESIGN_BODY)
    sys.stdout.write("\n".join(out) + "\n")



DESIGN_ITEMS = [
    ("wait_vblank", 0x974C), ("wait_vblanks", 0x9742),
    ("hud_dma", 0x5C8E), ("actor_dma", 0xAF24), ("game_init", 0xA248), ("hblank_verb_bar", 0x1006C),
    ("VBlank", 0x8BC), ("HBlank", 0x8AA),
]

DESIGN_HEAD = """## 5. "Stays 68k by design": how each disappears in a CPU-less build

(The design notes of the #24 groundwork, kept for the record. How it was done: docs/native.md. The table measures
what of these items still runs interpreted: nothing, after #24.)

These are the functions the porting docs (`docs/porting.md`, `docs/subsystems/*.md`) deliberately left in 68k
because the C replacement cannot reproduce them exactly under the HLE layer. Almost all of the reasons are
properties of *running C inside an emulated CPU* (no emulated time of its own, no way to block, a VDP beam
that only moves when the CPU clock does). A CPU-less build removes the reason together with the CPU, so none of
them needs a clever port; each needs a decision about what replaces the emulated clock and beam.
"""

DESIGN_BODY = """
### 5.1 Wait primitives (`wait_vblank` `$974C`, `wait_vblanks` `$9742`, spins on flags and countdowns)

*Why 68k now:* the original blocks: set `VBF_WAITING`, spin until the VBlank handler clears it. C code cannot
block inside the HLE layer, so every function that waits is cut into ranges at the call (`HLE_RANGE`, "Waiting
for VBlank" in docs/porting.md) and the call itself stays a 68k `bsr wait_vblank`. The same holds for the spins
on `ram_frame_countdown`, the DMA-idle spins (`$4836`, `$44BA`, `$3026`) and `text_wait`'s pacing loops.
Every `bsr wait_vblank` left in Reset, the menus and the script ops is one of these.

*In a CPU-less build:* the native frame loop is the scheduler.
1. `wait_vblank()` becomes `host_yield_until_vblank()`: run one frame of the native world (VBlank hook, pad read,
   renderer, audio) and return. In a coroutine build (each blocking game routine is a coroutine / a thread with a
   handoff) that is a plain call, so Reset and the script ops become straight-line C and the range chains
   (`*_range`, `_after_wait`) collapse back into the original function bodies. In a state-machine build the
   same point is the `case` boundary that the ranges already are (docs/subsystems/mainloop.md lists them).
2. Spins on a flag the VBlank hook clears (`VBF_WAITING`, `VBF_HUD_DMA`, `ram_frame_countdown`, the
   `$FF0AB1/2` meter countdowns) become the same yield inside a `while (flag) yield();`, and since the hook runs
   in the native frame loop the flag is cleared by C. No instruction-level timing is involved.
3. Idle-spin cost disappears (it is the bulk of the idle count in section 1), and `wait_vblanks(n)`
   is `n` yields.
Prerequisite: the VBlank hook (`vblank_main` etc.) must already be C (it is) and must run **between** game-code
slices, never inside one, which is exactly the frame-loop contract.

### 5.2 HV-counter readers (`hud_dma` `$5C8E`, `actor_dma` `$AF24`, `game_init` `$A248`, `hblank_verb_bar` `$1006C`)

*Why 68k now (docs/porting.md, "HV counter reads", #33):* they read `$C00008` and decide on the beam position
or feed it into game state. C has no notion of its own cost, so it cannot reproduce the value the 68k would
read; keeping the read inside a nested 68k call lets the clock and V counter run on across it.

*In a CPU-less build:* replace the beam by a deterministic model, per reader.
- `hud_dma`, `actor_dma`: the HV reads only ask "is there still time in this VBlank to do a DMA" (lines `$E1`-`$F6`,
  window `$E000`-`$F000`). With a native renderer there is no DMA budget: the upload is a memcpy into the
  native VRAM/texture mirror. **Done (#25)** with `--renderer native`: `hud_dma` (frameloop.c) and `actor_dma`
  (sprite.c) are C in the HLE group `native` and read the frame-loop HV model `frameloop_hv()` = `$E100` (the
  VBlank hook runs at the start of the blank, every DMA-window test passes; docs/subsystems/render.md). The
  one-frame DMA shift the original has when the window is missed is a timing artefact, not game state.
- `game_init`: one HV read seeds the RNG (HV -> `rng_next` -> X flag). Replace it by a deterministic model: a
  fixed seed for tests and replays, wall-clock or a frame counter for play. Record the seed in the savestate
  so determinism tests (difftest, storytest) stay reproducible. Decision needed: whether to keep "same as the
  hardware on a given power-on" (impossible, it depends on the power-on phase of the beam) or just seeded.
- `hblank_verb_bar`: the HBlank hook busy-waits to line 167 and then points planes A/B at the verb bar. See 5.3:
  the native renderer replaces the split, so the hook and its beam wait both go (**done, #25**).

### 5.3 The HBlank split (`hblank_verb_bar` `$1006C`, `$00FC0A`-`$010104` raster code, `vblank_main` register restore)

*Why 68k now:* the verb bar is drawn by switching the plane A/B nametable pointers, scroll and window registers
mid-frame at line 167 (an HBlank interrupt with a raster wait). The emulated VDP renders lines as the CPU clock
advances; C cannot be at line 167 at the right moment.

*In a CPU-less build:* the native renderer (#13 gives it the bulk data path as semantic ops) draws the frame
in two bands: lines 0-166 with the game-world registers and lines 167+ with the verb-bar registers. The band
boundary is data (line 167, the register set it applies), not an interrupt, so `hblank_verb_bar` becomes a
`renderer_set_split(167, verb_bar_regs)` call made by whoever enables the verb bar, and `vblank_main`'s "undo
the split" register restore (`$A65C` step 1) becomes "start the frame with the world registers". `hblank_nop`
and the HBlank vector go away. Raster-code at `$FC0A`-`$10104` that the verb bar uses for the scroll helpers is
ordinary computation and can be C without the beam.

**Done (#25), `--renderer native`:** the C `hblank_verb_bar` (group `native`, frameloop.c) records a `render_band`
(from line 168: planes A/B at `$A000`, v-scroll 0, h-scroll `ram_hscroll_verb`, window `ram_verb_bar_window`) and
the native renderer (`port/src/render/`) draws it; no VDP access, no beam wait. The HBlank interrupt and its
trampoline still run in the emulated build (they are what calls the hook). docs/subsystems/render.md.

### 5.4 Vector trampolines (`VBlank` `$8BC`, `HBlank` `$8AA`, `BusError` `$892`, the reset vector)

*Why 68k now:* five instructions each (`movem.l d0-a6,-(sp)` / `movea.l ($FF0000),a0` / `jsr (a0)` / `movem.l` /
`rte`): they are the CPU's way to enter the RAM hook, and there is nothing to port, they exist because the
hardware needs an entry point.

*In a CPU-less build:* the hook pointers `$FF0000` / `$FF0004` become C function pointers in the native frame
loop: at the end of every frame call `ram_vblank_hook` (C), and call `ram_hblank_hook` only if a split is
registered (5.3). `movem`/`rte` are the calling convention. `BusError` (colour-cycling hang) is dead code on
a correct run; replace it with an `abort()` that prints the fault. The reset vector becomes `main()`.

### 5.5 Stack tricks and "no states" leftovers

*Stack tricks:* a `push` before a wait and the matching `pop` after it straddle the wait, so no `HLE_RANGE`
can contain them (the range must leave with the SP it entered with): the menu item-table push/pop
(`$00857A`..`$0085DA`, `$008D36`), the password handlers' `tst.l (sp)+` / `lea 4(sp),sp`, `$008C60`..`$008C7E`,
and the reset itself (`movea.l (0),sp / jmp (4)`, which reloads the SP). *In a CPU-less build* the stack is C's:
the pushed values are locals, the table dispatch `lsl/movea/jmp (a0)` is an indexed call, the reset is a
`longjmp` to `main`'s start. These are mechanical once 5.1 gives the code a place to wait.

*No states / long sequences:* sub-ops `$05/$0C` fullscreen_picture, `$1E` ending_credits, `$27` minigame_throw, `$32`
minigame_tug, the Shaggy pose waits in `script_op_say`, and the never-executed functions of section 4a. No route
reaches them (or no savestate exists to difftest them), so they cannot yet be verified; they are plain
decompilation work (long, wait-heavy) plus a route that reaches them. Section 4a is the list.

### 5.6 What this means for the checklist

1. The first item is **not** a function: decide the waiting model (coroutine / state machine) so that 5.1 is a
   mechanical rewrite. Until then every wait stays a 68k `bsr`, and everything that sits next to one stays 68k
   (this is most of the table in section 3 that is not an HV reader).
2. HV readers and the HBlank split (5.2, 5.3) depend on the native renderer's frame contract; they are small
   in instructions but block the "no CPU" switch because they need the beam.
3. The trampolines (5.4) are free once 5.1-5.3 exist.
4. Everything else in section 3 is ordinary decompilation: take it from the top of the table (it is sorted by
   instructions executed on the gameplay routes), with section 4a as the list of functions with no ground truth.
"""

if __name__ == "__main__":
    main()
