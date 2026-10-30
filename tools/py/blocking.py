#!/usr/bin/env python3
"""Which functions can BLOCK - spin until an interrupt handler (or the raster) changes
something - directly or through their callees (data/callgraph.txt, incl. resolved
indirect calls such as script opcodes).  A C replacement must never reach such a function
through hle_call68k: interrupts don't fire inside C, so the wait never ends.  See
docs/porting.md, "Waiting for VBlank".

    tools/py/blocking.py                 list the spin loops and the number of blocking functions
    tools/py/blocking.py NAME|ADDR ...   for each function: BLOCKS (and why) or ok
    tools/py/blocking.py --all           every blocking function, with the callee that makes it block

A spin loop is a label followed by at most 4 instructions that read a RAM variable, a VDP
port or the HV counter (no post-increment/pre-decrement walks) and branch back to the
label. Counting loops (addq/subq/addi/subi on a data register: bit scans, step
animations) are not spin loops themselves (a wait_vblank inside them is found through the
call graph). Polls of the VDP status DMA-busy bit (`btst #1` of vdp_ctrl) are not counted: DMA
completes immediately in the sandbox and stalls the CPU in GPGX. Heuristic: a loop that
waits through a callee that is not in the call graph is missed.
"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def load():
    lines = open(os.path.join(ROOT, "src/scooby.s")).read().splitlines()
    funcs = {}
    pieces = []
    for l in open(os.path.join(ROOT, "data/functions.txt")):
        if l.startswith("#") or not l.strip():
            continue
        a, _size, name = l.split()[:3]
        funcs[name] = int(a, 16)
        pieces.append((int(a, 16), name))
        for extra in re.findall(r"\+([0-9a-fA-F]{6}):\d+", l):   # non-contiguous pieces
            pieces.append((int(extra, 16), name))
    order = sorted(pieces)

    def func_of(addr):
        best = None
        for a, n in order:
            if a > addr:
                break
            best = n
        return best

    # address of each source line: nearest preceding label with a known address
    addr_at = []
    cur = 0
    for l in lines:
        m = re.match(r"^(?:loc|sub|dat)_([0-9A-F]{6}):$", l)
        if m:
            cur = int(m.group(1), 16)
        else:
            m = re.match(r"^(\w+):$", l)
            if m and m.group(1) in funcs:
                cur = funcs[m.group(1)]
        addr_at.append(cur)

    spins = {}   # function -> [(label, body)]
    for i, l in enumerate(lines):
        m = re.match(r"^(\w+):$", l)
        if not m:
            continue
        lab = m.group(1)
        body = []
        for j in range(i + 1, min(i + 6, len(lines))):
            t = lines[j].strip()
            if re.match(r"^\w+:$", t) or t.startswith("|"):
                break
            body.append(t)
            if t.startswith("b") and t.split()[-1] == lab:
                reads = " ".join(body[:-1])
                if (re.search(r"\((ram_\w+|0xff\w+|vdp_\w+|0xc0000\w)", reads)
                        and not re.search(r"\(a\d\)\+|-\(a\d\)", reads)
                        and not ("vdp_ctrl" in reads and "#0x1," in reads)
                        and not re.search(r"\b(addq|subq|addi|subi)\.[bwl] #\S+,d\d", reads)):
                    spins.setdefault(func_of(addr_at[i]), []).append((lab, body))
                break

    callees = {}
    for l in open(os.path.join(ROOT, "data/callgraph.txt")):
        if l.startswith("#") or "->" not in l:
            continue
        left, right = l.split("->")
        callees[left.split()[1]] = [c.rstrip("*^") for c in right.split() if c != "-"]

    why = {f: "spins at " + ", ".join(lab for lab, _ in v) for f, v in spins.items()}
    changed = True
    while changed:
        changed = False
        for f, cs in callees.items():
            if f in why:
                continue
            for c in cs:
                if c in why:
                    why[f] = "calls " + c
                    changed = True
                    break
    return funcs, spins, why


def main(argv):
    funcs, spins, why = load()
    if not argv:
        for f in sorted(spins, key=lambda n: funcs.get(n, 0)):
            print(f"spin  {funcs.get(f, 0):06X} {f:24s} " + "; ".join(f"{lab}: {' / '.join(b)}" for lab, b in spins[f]))
        print(f"{len(why)} blocking functions (tools/py/blocking.py --all)")
        return 0
    if argv == ["--all"]:
        for f in sorted(why, key=lambda n: funcs.get(n, 0)):
            print(f"{funcs.get(f, 0):06X} {f:24s} {why[f]}")
        return 0
    for a in argv:
        name = a
        if re.fullmatch(r"\$?(0x)?[0-9A-Fa-f]{3,6}", a):
            v = int(a.lstrip("$").replace("0x", ""), 16)
            name = next((n for n, x in funcs.items() if x == v), a)
        print(f"{name:24s} {'BLOCKS: ' + why[name] if name in why else 'ok'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
