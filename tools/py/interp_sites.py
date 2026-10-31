#!/usr/bin/env python3
"""List every 68k instruction that still runs interpreted with --hle all (GitLab #24), as straight-line
blocks with their disassembly, the function they belong to and how often they ran.

    .venv/bin/python -I tools/py/interp_sites.py [runs/ipprof] [--min N] [--func NAME] [--exclude PROFILE_PREFIX]

Input: the profiles of port/tools/ipprof.sh (only runs whose spec is `all`).  A block is a maximal run of
executed instructions that are adjacent in the ROM.  This is the work list of #24 phase 1: every block
listed here needs a C block (HLE function / range) before the native runner can run that route.
"""
import glob
import os
import sys
from collections import defaultdict

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def load_profiles(d, exclude=()):
    top, nest = defaultdict(int), defaultdict(int)
    for f in glob.glob(os.path.join(d, "*.txt")):
        if f.endswith("hle-list.txt") or any(os.path.basename(f).startswith(x) for x in exclude):
            continue
        spec = None
        rows = []
        for ln in open(f):
            if ln.startswith("hle "):
                spec = ln.split(None, 1)[1].strip()
            elif ln[:2] in ("T ", "N "):
                k, a, n = ln.split()
                rows.append((k, int(a, 16), int(n)))
        if spec not in ("all", None) and not spec.startswith("all"):
            continue
        if spec and spec != "all" and spec.startswith("all,-"):
            continue
        for k, a, n in rows:
            (top if k == "T" else nest)[a] += n
    return top, nest


def load_funcs():
    import bisect
    fs = []
    for ln in open(os.path.join(ROOT, "data/functions.txt")):
        if ln.startswith("#") or not ln.strip():
            continue
        p = ln.split()
        a, sz, name = int(p[0], 16), int(p[1]), p[2]
        fs.append((a, a + sz, name))
        for piece in p[3:]:
            if piece.startswith("+") and ":" in piece:
                s, z = piece[1:].split(":")
                fs.append((int(s, 16), int(s, 16) + int(z), name))
    fs.sort()
    starts = [f[0] for f in fs]

    def owner(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and fs[i][0] <= addr < fs[i][1]:
            return fs[i][2]
        return "?"
    return owner


def main():
    args = sys.argv[1:]
    d = "runs/ipprof"
    mn, only, excl = 0, None, []
    i = 0
    while i < len(args):
        if args[i] == "--min":
            mn = int(args[i + 1]); i += 2
        elif args[i] == "--exclude":
            excl.append(args[i + 1]); i += 2
        elif args[i] == "--func":
            only = args[i + 1]; i += 2
        else:
            d = args[i]; i += 1
    top, nest = load_profiles(d, excl)
    from capstone import Cs, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_000
    md = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_000)
    rom = open(os.path.join(ROOT, "rom/baserom.md"), "rb").read()
    owner = load_funcs()
    addrs = sorted(set(top) | set(nest))
    ins = {}
    for a in addrs:
        for x in md.disasm(rom[a:a + 10], a):
            ins[a] = (x.size, f"{x.mnemonic} {x.op_str}".strip())
            break
        else:
            ins[a] = (2, "??")
    blocks = []
    cur = []
    for a in addrs:
        if cur and cur[-1] + ins[cur[-1]][0] == a and owner(a) == owner(cur[-1]):
            cur.append(a)
        else:
            if cur:
                blocks.append(cur)
            cur = [a]
    if cur:
        blocks.append(cur)
    total = 0
    nb = 0
    for b in blocks:
        f = owner(b[0])
        if only and f != only:
            continue
        cnt = max(top.get(a, 0) + nest.get(a, 0) for a in b)
        if cnt < mn:
            continue
        nb += 1
        total += len(b)
        nn = sum(nest.get(a, 0) for a in b)
        print(f"== ${b[0]:06X} {f}  {len(b)} insns  max {cnt}{'  NESTED ' + str(nn) if nn else ''}")
        for a in b:
            print(f"   {a:06X}  {top.get(a, 0) + nest.get(a, 0):>10}  {ins[a][1]}")
    print(f"# {nb} blocks, {total} instructions", file=sys.stderr)


if __name__ == "__main__":
    main()
