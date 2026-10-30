#!/usr/bin/env python3
"""Static cross-reference lookup over the generated data files.

    xref.py ADDR|NAME      who calls / jumps to / points at a ROM address or function,
                           or who reads/writes/takes the address of a RAM / I/O / ROM-data variable
    xref.py --top [N]      most-called functions and most-referenced RAM variables
    xref.py --list-ram     RAM variables ranked by static reference count

ADDR is hex ($ff09e0, 0xFF09E0 or ff09e0); NAME is a name from data/symbols.txt, data/ram.txt
or data/functions.txt.  A RAM address inside a named array/struct reports the containing variable.
Reads data/xrefs.txt + data/functions.txt + data/ram.txt (regenerate with `make disasm`).
Access kinds are inferred statically from the mnemonic (r = read, w = write, rw = read-modify-write,
lea = address taken); register-indirect accesses through pointers are invisible to this tool.
"""
import bisect
import os
import sys

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "data")


def rows(name):
    p = os.path.join(DATA, name)
    for line in open(p):
        if line.strip() and not line.startswith("#"):
            yield line


def load():
    funcs = []   # (start, end, name)
    names = {}
    for line in rows("functions.txt"):
        head, _, note = line.partition("#")
        a, size, name = head.split()
        a = int(a, 16)
        names[a] = name
        funcs.append((a, a + int(size), name))
        for tok in note.split():
            if tok.startswith("+"):
                s, z = tok[1:].split(":")
                funcs.append((int(s, 16), int(s, 16) + int(z), name))
    funcs.sort()
    ram = []
    for line in rows("ram.txt"):
        f = line.split("#", 1)[0].split()
        if len(f) >= 3:
            ram.append((int(f[0].lstrip("$"), 16), int(f[1], 0), f[2]))
    ram.sort()
    sym = {}
    for line in rows("symbols.txt"):
        f = line.split("#", 1)[0].split()
        if len(f) >= 2:
            sym[int(f[0].lstrip("$"), 16)] = f[1]
    xr = []
    for line in rows("xrefs.txt"):
        f = line.split()
        xr.append((int(f[0], 16), f[1], int(f[2], 16), f[3] if len(f) > 3 else ""))
    return funcs, names, ram, sym, xr


class DB:
    def __init__(self):
        self.funcs, self.names, self.ram, self.sym, self.xr = load()
        self.fstarts = [f[0] for f in self.funcs]

    def func_of(self, addr):
        i = bisect.bisect_right(self.fstarts, addr) - 1
        if i >= 0 and self.funcs[i][0] <= addr < self.funcs[i][1]:
            return self.funcs[i][2]
        return None

    def ram_name(self, addr):
        for a, s, n in self.ram:
            if a <= addr < a + max(s, 1):
                return n if addr == a else f"{n}+{addr - a}"
        return None

    def where(self, addr):
        f = self.func_of(addr)
        return f"{f}" if f else "?"

    def resolve(self, arg):
        for a, s, n in self.ram:
            if n == arg:
                return a, s
        for a, n in self.sym.items():
            if n == arg:
                return a, 1
        for a, n in self.names.items():
            if n == arg:
                return a, 1
        v = int(arg.lstrip("$").replace("0x", ""), 16)
        for a, s, n in self.ram:
            if a <= v < a + max(s, 1):
                return v, 1
        return v, 1


def query(db, arg):
    addr, size = db.resolve(arg)
    nm = db.ram_name(addr) or db.sym.get(addr) or db.names.get(addr)
    print(f"{addr:06X} {nm or ''}".rstrip())
    lo, hi = addr, addr + max(size, 1)
    # a name may denote a whole variable; an address query hits any reference overlapping it
    hits = [r for r in db.xr if lo <= r[2] < hi]
    if not hits:
        print("  (no static references)")
        return
    out = {}
    for src, kind, tgt, det in hits:
        out.setdefault(kind, []).append((src, tgt, det))
    labels = {"call": "called from", "jump": "tail-jumped / branched to from", "ind": "reached via indirect jsr/jmp at",
              "ptr": "pointer stored at", "mem": "accessed by", "imm": "address passed as immediate by"}
    for kind in ("call", "jump", "ind", "ptr", "imm", "mem"):
        if kind not in out:
            continue
        print(f"  {labels[kind]}:")
        for src, tgt, det in out[kind]:
            tn = f" [{db.ram_name(tgt) or ''}]" if (kind in ("mem", "imm") and (hi - lo > 1 or tgt != addr)) else ""
            print(f"    {src:06X}  {db.where(src):<16} {det}{tn}".rstrip())
    if "mem" in out:
        acc = {}
        for src, tgt, det in out["mem"]:
            k = det.split(".")[0]
            acc.setdefault(k, set()).add(db.where(src))
        print("  summary: " + "; ".join(f"{k}: {len(v)} fn" for k, v in sorted(acc.items())))


def top(db, n):
    calls = {}
    for src, kind, tgt, det in db.xr:
        if kind in ("call", "ind"):
            calls.setdefault(tgt, set()).add(src)
    print("most-called functions (distinct call sites):")
    for t, s in sorted(calls.items(), key=lambda kv: -len(kv[1]))[:n]:
        print(f"  {len(s):4d}  {t:06X} {db.names.get(t, '')}")
    list_ram(db, n)


def list_ram(db, n):
    cnt = {}
    for src, kind, tgt, det in db.xr:
        if kind in ("mem", "imm") and tgt >= 0xA00000:
            cnt[tgt] = cnt.get(tgt, 0) + 1
    print("most-referenced RAM/IO addresses (static refs):")
    for t, c in sorted(cnt.items(), key=lambda kv: -kv[1])[:n]:
        print(f"  {c:4d}  {t:06X} {db.ram_name(t) or ''}")


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    db = DB()
    if a[0] == "--top":
        top(db, int(a[1]) if len(a) > 1 else 40)
    elif a[0] == "--list-ram":
        list_ram(db, int(a[1]) if len(a) > 1 else 200)
    else:
        for x in a:
            query(db, x)
    return 0


if __name__ == "__main__":
    sys.exit(main())
