"""Function partition, symbol/RAM tables, call graph and cross-references.

Used by tools/py/disasm.py (additive: names flow into src/scooby.s, and the
generated data/functions.txt, data/callgraph.txt, data/xrefs.txt are written
from the same traced instruction set).

Inputs (hand-maintained):
  data/symbols.txt  ROM labels:   addr name   # note     (overrides sub_/loc_/dat_ defaults)
  data/ram.txt      RAM / I/O:    addr size name   # note (emitted as .equ, used for absolute operands)
Outputs (generated, do not edit): data/functions.txt data/callgraph.txt data/xrefs.txt
"""
import bisect
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m68kfmt  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DATA = os.path.join(ROOT, "data")

END = {"rts", "rte", "rtr", "bra", "jmp", "illegal"}
CALL = {"bsr", "jsr"}


def base(insn):
    return insn.mnemonic.split(".")[0]


# ---------------------------------------------------------------- hand tables
def load_symbols():
    """data/symbols.txt -> {addr: name}"""
    out = {}
    p = os.path.join(DATA, "symbols.txt")
    if os.path.exists(p):
        for line in open(p):
            line = line.split("#", 1)[0].split()
            if len(line) >= 2:
                out[int(line[0].lstrip("$"), 16)] = line[1]
    return out


def load_ram():
    """data/ram.txt -> sorted list of (addr, size, name)"""
    out = []
    p = os.path.join(DATA, "ram.txt")
    if os.path.exists(p):
        for line in open(p):
            line = line.split("#", 1)[0].split()
            if len(line) >= 3:
                out.append((int(line[0].lstrip("$"), 16), int(line[1], 0), line[2]))
    out.sort()
    return out


class RamMap:
    def __init__(self, ram):
        self.ram = ram
        self.exact = {a: n for a, s, n in ram}
        self.starts = [a for a, s, n in ram]

    def lookup(self, addr):
        """(name, offset) of the named variable containing addr, or None"""
        i = bisect.bisect_right(self.starts, addr) - 1
        while i >= 0:
            a, s, n = self.ram[i]
            if a <= addr < a + max(s, 1):
                return n, addr - a
            if addr - a > 0x1000:
                break
            i -= 1
        return None

    def label(self, addr):
        r = self.lookup(addr)
        if r is None:
            return None
        n, o = r
        return n if o == 0 else f"{n}+{o}"

    def equs(self):
        return [f"\t.equ {n}, 0x{a:x}" for a, s, n in self.ram]


# -------------------------------------------------------------- function split
def flow_succ(insn, insns):
    """intra-function successors (calls fall through, never enter the callee)."""
    b = base(insn)
    nxt = insn.address + insn.size
    out = []
    tgt = m68kfmt.branch_target(insn)
    if b in CALL:
        out.append(nxt)
    elif b in END:
        if b in ("bra", "jmp") and tgt is not None:
            out.append(tgt)
    else:
        if tgt is not None:   # bcc / dbcc
            out.append(tgt)
        out.append(nxt)
    return [t for t in out if t in insns]


def partition(insns, entries):
    """Assign every instruction to exactly one function.  Returns (owner, entries)."""
    entries = {e for e in entries if e in insns}
    succ = {a: flow_succ(i, insns) for a, i in insns.items()}
    pred = {}
    for a, ss in succ.items():
        for s in ss:
            pred.setdefault(s, []).append(a)
    while True:
        reach = {}   # insn -> set of entries reaching it
        for e in entries:
            seen = {e}
            work = [e]
            while work:
                a = work.pop()
                for s in succ[a]:
                    if s not in seen and s not in entries:
                        seen.add(s)
                        work.append(s)
            for a in seen:
                reach.setdefault(a, set()).add(e)
        new = set()
        contested = {a for a, es in reach.items() if len(es) > 1}
        if contested:
            # promote the heads of contested regions (reached from a non-contested insn)
            for a in contested:
                if any(p not in contested for p in pred.get(a, [])):
                    new.add(a)
            if not new:
                new.add(min(contested))
        else:
            unowned = set(insns) - set(reach)
            if unowned:
                heads = {a for a in unowned if not any(p in unowned for p in pred.get(a, []))}
                new = heads or {min(unowned)}
        if not new:
            break
        entries |= new
    owner = {a: next(iter(es)) for a, es in reach.items()}
    return owner, entries


# ----------------------------------------------------------------- xrefs
READ_ONLY = {"cmp", "cmpi", "cmpa", "cmpm", "tst", "btst", "mulu", "muls", "divu", "divs", "chk"}
WRITE_ONLY = {"clr", "move", "movea", "moveq", "st", "sf", "seq", "sne", "scc", "scs", "smi", "spl",
              "shi", "sls", "sge", "slt", "sgt", "sle", "svc", "svs"}


def mem_target(insn, op):
    """Static address referenced by a memory operand, or None."""
    if op.type != m68kfmt.M.M68K_OP_MEM:
        return None
    am = op.address_mode
    a = insn.address
    if am == m68kfmt.M.M68K_AM_PCI_DISP:
        return (a + 2 + m68kfmt.s16(op.mem.disp)) & 0xFFFFFFFF
    if am in (m68kfmt.M.M68K_AM_PCI_INDEX_8_BIT_DISP, m68kfmt.M.M68K_AM_PCI_INDEX_BASE_DISP):
        return (a + 2 + m68kfmt.s8(op.mem.disp)) & 0xFFFFFFFF
    if am == m68kfmt.M.M68K_AM_ABSOLUTE_DATA_LONG:
        return op.imm & 0xFFFFFFFF
    if am == m68kfmt.M.M68K_AM_ABSOLUTE_DATA_SHORT:
        v = m68kfmt.s16(op.imm) & 0xFFFFFFFF
        return v | 0xFF000000 if v & 0x8000 else v   # sign-extended to 24 bits below
    return None


def mem_access(insn, idx, n):
    """'r' / 'w' / 'rw' / 'lea' / 'call' for memory operand idx of n operands."""
    b = base(insn)
    if b in ("lea", "pea"):
        return "lea"
    if b in ("jmp", "jsr"):
        return "call"
    if b == "movem":
        return "r" if idx == 0 else "w"
    if n == 1:
        if b in READ_ONLY:
            return "r"
        if b in WRITE_ONLY:
            return "w"
        return "rw"
    if idx < n - 1:
        return "r"
    if b in READ_ONLY:
        return "r"
    if b in WRITE_ONLY:
        return "w"
    return "rw"


def build_xrefs(insns, owner, ind_sites, table_slots):
    """Rows (src, kind, target, detail).  kinds: call jump ind ptr mem.

    call  bsr/jsr static            jump  bra/jmp/bcc out of the function
    ind   resolved indirect jsr/jmp ptr   pointer-table slot / stored code pointer
    mem   operand referencing memory (detail = access.size, e.g. r.w w.l rw.b lea)
    """
    rows = []
    for a, insn in sorted(insns.items()):
        b = base(insn)
        tgt = m68kfmt.branch_target(insn)
        if tgt is not None and tgt in insns:
            if b in CALL:
                rows.append((a, "call", tgt, ""))
            elif owner.get(tgt) != owner.get(a):
                rows.append((a, "jump", tgt, ""))
        n = len(insn.operands)
        for idx, op in enumerate(insn.operands):
            t = mem_target(insn, op)
            if t is None:
                continue
            if b in ("bsr", "jsr", "jmp") and t in insns:
                continue   # already a call/jump row
            sz = insn.mnemonic.split(".")[1] if "." in insn.mnemonic else ""
            rows.append((a, "mem", t, mem_access(insn, idx, n) + (("." + sz) if sz else "")))
    for site, tg in sorted(ind_sites.items()):
        for t in tg:
            rows.append((site, "ind", t, ""))
    for slot, t in sorted(table_slots.items()):
        rows.append((slot, "ptr", t, ""))
    return rows


# ------------------------------------------------------------------ outputs
def chunks_of(insns, owner, entry):
    """Contiguous owned runs of a function as [[start, end]], sorted."""
    mine = sorted(a for a, o in owner.items() if o == entry)
    runs = []
    for a in mine:
        e = a + insns[a].size
        if runs and runs[-1][1] == a:
            runs[-1][1] = e
        else:
            runs.append([a, e])
    return runs


def write_outputs(rom, insns, owner, entries, ind_sites, slots, name_of, ram):
    # --- functions.txt
    out = ["# Functions (generated by tools/py/disasm.py via symtab.py -- edit data/symbols.txt to rename).",
           "# addr   size  name   # insns; extra non-contiguous pieces as +start:size",
           "# size = contiguous bytes from the entry; every traced instruction belongs to exactly one function.",
           "# Entry = bsr/jsr target, vector/table/indirect target, stored code pointer, or promoted shared tail/orphan."]
    cnt = {}
    for a, e in owner.items():
        cnt[e] = cnt.get(e, 0) + 1
    for e in sorted(entries):
        runs = chunks_of(insns, owner, e)
        main = next(r for r in runs if r[0] <= e < r[1])
        pieces = [(e, main[1])] + [(r[0], r[1]) for r in runs if r is not main]
        if main[0] < e:
            pieces.append((main[0], e))
        extra = "".join(f" +{s:06x}:{t - s}" for s, t in pieces[1:])
        out.append(f"{e:06x} {pieces[0][1] - pieces[0][0]:5d} {name_of(e)}  # {cnt[e]} insns{extra}")
    open(os.path.join(DATA, "functions.txt"), "w").write("\n".join(out) + "\n")

    # --- xrefs.txt
    rows = build_xrefs(insns, owner, ind_sites, slots)
    xo = ["# Static cross-references (generated by tools/py/disasm.py via symtab.py; query with tools/py/xref.py).",
          "# src  kind  target  detail   -- kinds: call jump ind ptr mem imm",
          "# mem detail = access.size (r/w/rw/lea). Access is inferred from mnemonic/operand position (approximate).",
          "# ptr src = pointer-table slot or the insn storing an immediate code pointer."]
    for a, kind, t, d in rows:
        xo.append(f"{a:06x} {kind} {t:06x} {d}".rstrip())
    for a, insn in sorted(insns.items()):   # RAM addresses passed as immediates
        if insn.mnemonic in ("movea.l", "move.l", "cmpa.l") and insn.operands[0].type == m68kfmt.CS_OP_IMM:
            v = insn.operands[0].imm & 0xFFFFFFFF
            if 0xFF0000 <= v <= 0xFFFFFF:
                xo.append(f"{a:06x} imm {v:06x}")
    open(os.path.join(DATA, "xrefs.txt"), "w").write("\n".join(xo) + "\n")

    # --- callgraph.txt
    cg = {}
    for a, kind, t, d in rows:
        if kind not in ("call", "jump", "ind") or a not in owner:
            continue
        f = owner[a]
        mark = {"call": "", "jump": "^", "ind": "*"}[kind]
        cg.setdefault(f, {})[(owner.get(t, t), mark)] = name_of(owner.get(t, t)) + mark
    co = ["# Call graph (generated by tools/py/disasm.py via symtab.py): caller -> callees.",
          "# name = bsr/jsr, name* = via resolved indirect jsr/jmp, name^ = tail jump/branch out of the function.",
          "# addr   caller -> callees (ordered by address)"]
    for e in sorted(entries):
        callees = [v for k, v in sorted(cg.get(e, {}).items())]
        co.append(f"{e:06x} {name_of(e)} -> " + (" ".join(callees) if callees else "-"))
    open(os.path.join(DATA, "callgraph.txt"), "w").write("\n".join(co) + "\n")


def check_exec_starts():
    """Acceptance check: every data/exec_starts.txt address lies in exactly one function."""
    ivs = []
    for line in open(os.path.join(DATA, "functions.txt")):
        if line.startswith("#") or not line.strip():
            continue
        head, _, note = line.partition("#")
        a, size, name = head.split()
        a = int(a, 16)
        ivs.append((a, a + int(size), name))
        for tok in note.split():
            if tok.startswith("+"):
                s, z = tok[1:].split(":")
                ivs.append((int(s, 16), int(s, 16) + int(z), name))
    ivs.sort()
    bad = 0
    for (a0, b0, n0), (a1, b1, n1) in zip(ivs, ivs[1:]):
        if a1 < b0:
            print(f"overlap {n0} [{a0:06x},{b0:06x}) / {n1} [{a1:06x},{b1:06x})")
            bad += 1
    starts = [a for a, b, n in ivs]
    miss = 0
    total = 0
    for line in open(os.path.join(DATA, "exec_starts.txt")):
        if not line.strip() or line.startswith("#"):
            continue
        x = int(line, 16)
        total += 1
        i = bisect.bisect_right(starts, x) - 1
        if i < 0 or not (ivs[i][0] <= x < ivs[i][1]):
            print(f"exec start {x:06x} is in no function")
            miss += 1
    print(f"{len(ivs)} function chunks, {total} exec starts, {miss} uncovered, {bad} overlaps")
    return miss == 0 and bad == 0


if __name__ == "__main__":
    import sys
    sys.exit(0 if check_exec_starts() else 1)
