"""Resolve indirect control flow (jmp/jsr through registers, jump/pointer tables).

Used by disasm.py.  For every traced `jmp`/`jsr` whose target is not a constant, a
small backward slice over the traced control-flow graph finds where the target
register came from.  Supported value sources:

  lea X,aN / movea.l #X,aN / pea X / move.l #X,-(sp)      constant
  movea.l (X).l,aN, X in ROM                              long read from ROM
  movea.l (X).l,aN, X in RAM                              every `move.l #imm,(X)` /
                                                          `move.l aK,(X)` store in the code
  movea.l (d,pc,dX),aN / movea.l (d,aM,dX),aN             long pointer table (aM sliced)
  move.w (T,pc,dX),dX ; jmp (T,pc,dX)                     word offset table at T
  push/pop through the stack (move.l aK,-(sp) ... movea.l (sp)+,aN), movem, addq/lea on sp
  routine entry: continues at every bsr/jsr call site (interprocedural)

Assumption: a bsr/jsr between definition and use preserves address registers
and leaves the stack balanced (true for this game's hand-written code as far as
checked; the slice notes it as `via-call`).
"""
import struct

from capstone import m68k as M

import m68kfmt
from m68kfmt import s8, s16

ADDR_BASE = M.M68K_REG_A0          # a0..a7 = ADDR_BASE + n
SP = M.M68K_REG_A7
END = {"rts", "rte", "rtr", "bra", "jmp", "illegal"}
MAX_STEPS = 4000


def mbase(insn):
    return insn.mnemonic.split(".")[0]


def msize(insn):
    p = insn.mnemonic.split(".")
    return {"b": 1, "w": 2, "l": 4}.get(p[1], 0) if len(p) > 1 else 0


def const_addr(insn, op):
    """Constant effective address of a memory operand (abs / pc-relative), else None."""
    if op.type != M.M68K_OP_MEM:
        return None
    am = op.address_mode
    if am == M.M68K_AM_ABSOLUTE_DATA_LONG:
        return op.imm & 0xFFFFFF
    if am == M.M68K_AM_ABSOLUTE_DATA_SHORT:
        return s16(op.imm) & 0xFFFFFF
    if am == M.M68K_AM_PCI_DISP:
        return (insn.address + 2 + s16(op.mem.disp)) & 0xFFFFFF
    return None


def nbits(bits):
    return bin(bits).count("1")


def touches(op, r):
    """Operand uses register r as an address-register base that it modifies (pre/post)."""
    return op.type == M.M68K_OP_MEM and op.address_mode in (
        M.M68K_AM_REGI_ADDR_POST_INC, M.M68K_AM_REGI_ADDR_PRE_DEC) and op.reg == r


def writes_reg(insn, r):
    b = mbase(insn)
    ops = insn.operands
    if b in ("bsr", "jsr", "cmp", "cmpa", "cmpi", "tst", "btst", "cmpm") and not any(touches(o, r) for o in ops):
        return False
    if any(touches(o, r) for o in ops):
        return True
    if not ops:
        return False
    if b == "exg":
        return any(o.type == M.M68K_OP_REG and o.reg == r for o in ops)
    if b in ("link", "unlk"):
        return ops[0].type == M.M68K_OP_REG and ops[0].reg == r
    if b == "movem":
        d = ops[-1]
        if d.type == M.M68K_OP_REG_BITS:
            idx = r - ADDR_BASE + 8 if r >= ADDR_BASE else r - M.M68K_REG_D0
            return bool(d.register_bits >> idx & 1)
        return False
    d = ops[-1]
    if d.type == M.M68K_OP_REG and d.reg == r:
        return True
    if b == "swap" and ops[0].type == M.M68K_OP_REG and ops[0].reg == r:
        return True
    return False


class Resolver:
    def __init__(self, rom, insns, codeok, ram_lo=0xFF0000):
        self.rom = rom
        self.insns = insns
        self.codeok = codeok
        self.ram_lo = ram_lo
        self.preds = {}
        self.callers = {}
        for a, i in insns.items():
            b = mbase(i)
            tgt = m68kfmt.branch_target(i)
            if b in ("bsr", "jsr"):
                if tgt is not None:
                    self.callers.setdefault(tgt, []).append(a)
            elif tgt is not None:
                self.preds.setdefault(tgt, []).append(a)
            if b not in END:
                self.preds.setdefault(a + i.size, []).append(a)
        self.imm_ptrs = {}      # insn addr -> code pointer immediate value
        self._stores = None

    # ---------- memory variable stores ----------
    def stores(self):
        if self._stores is None:
            self._stores = {}
            for a, i in self.insns.items():
                if mbase(i) in ("move", "movea") and msize(i) == 4 and len(i.operands) == 2:
                    t = const_addr(i, i.operands[1])
                    if t is not None:
                        self._stores.setdefault(t, []).append(a)
        return self._stores

    def resolve_mem(self, addr, off, notes):
        """Values a long variable at addr can hold (+off)."""
        out = []
        if addr < len(self.rom):
            out.append((struct.unpack(">I", self.rom[addr:addr + 4])[0] + off, None, f"rom {addr:06x}"))
            return out
        if addr < self.ram_lo:
            notes.append(f"pointer variable {addr:06x} outside ROM/RAM")
            return out
        st = self.stores().get(addr, [])
        if not st:
            notes.append(f"no direct stores to {addr:06x} found")
        for s in st:
            i = self.insns[s]
            src = i.operands[0]
            if src.type == M.M68K_OP_IMM:
                out.append(((src.imm & 0xFFFFFFFF) + off, s, f"ram {addr:06x}"))
            elif src.type == M.M68K_OP_MEM and src.address_mode == M.M68K_AM_REGI_ADDR_POST_INC and src.reg == SP:
                continue   # restore of a value saved with move.l (addr),-(sp)
            elif src.type == M.M68K_OP_REG:
                for v in self.slice(s, ("reg", src.reg, off), notes):
                    out.append((v[0], v[1], f"ram {addr:06x}/" + v[2]))
            else:
                notes.append(f"store to {addr:06x} at {s:06x} from {i.op_str} not tracked")
        return out

    # ---------- table readers ----------
    def long_table(self, base, maxn=256):
        ents = []
        seen = set()
        for k in range(maxn):
            p = base + 4 * k
            if p + 4 > len(self.rom) or p in self.insns or p in seen or self.inside_insn(p):
                break
            v = struct.unpack(">I", self.rom[p:p + 4])[0]
            if not self.codeok(v):
                break
            ents.append(v)
            seen.add(v)
        return ents

    def word_table(self, base, maxn=256):
        ents = []
        seen = set()
        for k in range(maxn):
            p = base + 2 * k
            if p in self.insns or p in seen or self.inside_insn(p):
                break
            v = (base + s16(struct.unpack(">H", self.rom[p:p + 2])[0])) & 0xFFFFFF
            if not self.codeok(v):
                break
            ents.append(v)
            seen.add(v)
        return ents

    def inside_insn(self, p):
        for d in (2, 4, 6, 8):
            i = self.insns.get(p - d)
            if i is not None and i.size > d:
                return True
        return False

    # ---------- backward slice ----------
    def slice(self, at, q, notes, budget=None):
        """Possible values of query q immediately before insn `at` executes.

        q = ("reg", r, off) | ("stk", k, off) ; result list of (value, imm_insn, how).
        how is a string; tables return ("table"/"wtable", base) markers in notes via self.tables.
        """
        out = []
        work = [(at, q)]
        seen = set()
        steps = 0
        while work:
            a, q = work.pop()
            if (a, q) in seen:
                continue
            seen.add((a, q))
            steps += 1
            if steps > MAX_STEPS:
                notes.append("slice budget exhausted")
                break
            preds = self.preds.get(a, [])
            calls = self.callers.get(a, [])
            if not preds and not calls:
                notes.append(f"reaches {a:06x} with no known predecessor/caller")
                continue
            for p in preds:
                r = self.transfer(p, q, notes)
                if r is None:
                    continue
                kind, val = r
                if kind == "cont":
                    work.append((p, val))
                else:
                    out.extend(val)
            for c in calls:
                # value at routine entry == value before the call (regs); stack shifts by the return address
                if q[0] == "reg":
                    work.append((c, q))
                elif q[1] >= 4:
                    work.append((c, ("stk", q[1] - 4, q[2])))
                else:
                    ci = self.insns[c]
                    out.append((c + ci.size + q[2], None, f"return-address of {c:06x}"))
        return out

    def src_value(self, p, src, off, notes):
        """Value of a long source operand of insn p (before p executes)."""
        i = self.insns[p]
        if src.type == M.M68K_OP_IMM:
            return ("vals", [((src.imm & 0xFFFFFFFF) + off, p, "imm")])
        if src.type == M.M68K_OP_REG:
            return ("cont", ("reg", src.reg, off))
        ca = const_addr(i, src)
        if ca is not None:
            return ("vals", self.resolve_mem(ca, off, notes))
        if src.type == M.M68K_OP_MEM and src.address_mode == M.M68K_AM_REGI_ADDR_POST_INC and src.reg == SP:
            return ("cont", ("stk", 0, off))
        if src.type == M.M68K_OP_MEM and src.address_mode in (
                M.M68K_AM_PCI_INDEX_8_BIT_DISP, M.M68K_AM_PCI_INDEX_BASE_DISP,
                M.M68K_AM_AREGI_INDEX_8_BIT_DISP, M.M68K_AM_AREGI_INDEX_BASE_DISP, M.M68K_AM_REGI_ADDR,
                M.M68K_AM_REGI_ADDR_DISP):
            if off:
                notes.append(f"offset {off} on table load at {p:06x}")
            am = src.address_mode
            if am in (M.M68K_AM_PCI_INDEX_8_BIT_DISP, M.M68K_AM_PCI_INDEX_BASE_DISP):
                bases = [((p + 2 + s8(src.mem.disp)) & 0xFFFFFF, None, "pc")]
            else:
                breg = src.reg if am == M.M68K_AM_REGI_ADDR else src.mem.base_reg
                disp = 0 if am == M.M68K_AM_REGI_ADDR else (
                    s16(src.mem.disp) if am == M.M68K_AM_REGI_ADDR_DISP else s8(src.mem.disp))
                bases = self.slice(p, ("reg", breg, disp), notes)
            vals = []
            for b, bi, how in bases:
                b &= 0xFFFFFF
                if am in (M.M68K_AM_REGI_ADDR, M.M68K_AM_REGI_ADDR_DISP):
                    if b < len(self.rom):   # fixed slot, not indexed: a single pointer
                        vals += self.resolve_mem(b, 0, notes)
                    else:
                        notes.append(f"pointer loaded from RAM structure {b:06x} at {p:06x}")
                    continue
                if b >= len(self.rom):
                    notes.append(f"pointer table in RAM {b:06x} at {p:06x}")
                    continue
                ents = self.long_table(b)
                self.tables.setdefault(b, ("l", b))
                self.table_len[b] = max(self.table_len.get(b, 0), len(ents))
                if not ents:
                    notes.append(f"table {b:06x}: no valid code pointers")
                vals += [(e, None, f"ltable {b:06x}") for e in ents]
            return ("vals", vals)
        notes.append(f"source `{i.mnemonic} {i.op_str}` at {p:06x} not tracked")
        return ("vals", [])

    def transfer(self, p, q, notes):
        """Apply insn p backward to query q. Returns None, ('cont', q'), or ('vals', [...])."""
        i = self.insns[p]
        b = mbase(i)
        ops = i.operands
        kind, x, off = q
        if kind == "reg":
            r = x
            if not writes_reg(i, r):
                return ("cont", q)
            d = ops[-1]
            if b == "lea" and d.type == M.M68K_OP_REG and d.reg == r:
                s = ops[0]
                ca = const_addr(i, s)
                if ca is not None:
                    return ("vals", [(ca + off, None, "lea")])
                if s.address_mode == M.M68K_AM_REGI_ADDR_DISP:
                    return ("cont", ("reg", s.mem.base_reg, off + s16(s.mem.disp)))
                if s.address_mode == M.M68K_AM_REGI_ADDR:
                    return ("cont", ("reg", s.reg, off))
                notes.append(f"lea at {p:06x} with {i.op_str} not tracked")
                return None
            if b in ("move", "movea") and msize(i) == 4 and d.type == M.M68K_OP_REG and d.reg == r:
                return self.src_value(p, ops[0], off, notes)
            if b in ("addq", "subq", "adda", "suba") and ops[0].type == M.M68K_OP_IMM and d.type == M.M68K_OP_REG:
                v = ops[0].imm if b in ("addq", "adda") else -ops[0].imm
                return ("cont", ("reg", r, off + v))
            notes.append(f"`{i.mnemonic} {i.op_str}` at {p:06x} clobbers {m68kfmt.REG_NAMES.get(r, r)}")
            return None
        # stack slot query: x = byte offset from sp after insn p
        k = x
        if b in ("bsr", "jsr"):
            return ("cont", q)
        pushed = popped = 0
        for o in ops:
            if o.type == M.M68K_OP_MEM and o.reg == SP and o.address_mode == M.M68K_AM_REGI_ADDR_PRE_DEC:
                pushed = msize(i) * (nbits(ops[0].register_bits) if b == "movem" else 1)
            if o.type == M.M68K_OP_MEM and o.reg == SP and o.address_mode == M.M68K_AM_REGI_ADDR_POST_INC:
                popped = msize(i) * (nbits(ops[-1].register_bits) if b == "movem" else 1)
        if b == "pea":
            pushed = 4
        if pushed:
            if k >= pushed:
                return ("cont", ("stk", k - pushed, off))
            if b == "pea" and k == 0:
                ca = const_addr(i, ops[0])
                if ca is not None:
                    return ("vals", [(ca + off, None, "pea")])
            if b in ("move", "movea") and msize(i) == 4 and k == 0:
                return self.src_value(p, ops[0], off, notes)
            if b == "movem" and k % 4 == 0 and msize(i) == 4:
                bits = ops[0].register_bits
                regs = [n for n in range(16) if bits >> n & 1]   # lowest reg at lowest address
                n = regs[k // 4]
                reg = (ADDR_BASE + n - 8) if n >= 8 else (M.M68K_REG_D0 + n)
                return ("cont", ("reg", reg, off))
            notes.append(f"stack slot written by `{i.mnemonic} {i.op_str}` at {p:06x}")
            return None
        if popped:
            return ("cont", ("stk", k + popped, off))
        d = ops[-1] if ops else None
        if d is not None and d.type == M.M68K_OP_REG and d.reg == SP:
            if b in ("addq", "adda") and ops[0].type == M.M68K_OP_IMM:
                return ("cont", ("stk", k + ops[0].imm, off))
            if b in ("subq", "suba") and ops[0].type == M.M68K_OP_IMM and k >= ops[0].imm:
                return ("cont", ("stk", k - ops[0].imm, off))
            if b == "lea" and ops[0].type == M.M68K_OP_MEM and ops[0].address_mode == M.M68K_AM_REGI_ADDR_DISP \
                    and ops[0].mem.base_reg == SP:
                return ("cont", ("stk", k + s16(ops[0].mem.disp), off))
            notes.append(f"sp changed by `{i.mnemonic} {i.op_str}` at {p:06x}")
            return None
        if b in ("rts", "rte", "rtr", "link", "unlk"):
            notes.append(f"stack frame change at {p:06x}")
            return None
        return ("cont", q)

    # ---------- per site ----------
    def resolve_site(self, a):
        """Return (kind, values[(v, imm_insn, how)], notes)."""
        i = self.insns[a]
        op = i.operands[0]
        notes = []
        am = op.address_mode
        if am == M.M68K_AM_REGI_ADDR:
            vals = self.slice(a, ("reg", op.reg, 0), notes)
            return vals, notes
        if am == M.M68K_AM_REGI_ADDR_DISP:
            vals = self.slice(a, ("reg", op.mem.base_reg, s16(op.mem.disp)), notes)
            return vals, notes
        if am in (M.M68K_AM_PCI_INDEX_8_BIT_DISP, M.M68K_AM_PCI_INDEX_BASE_DISP):
            base = (a + 2 + s8(op.mem.disp)) & 0xFFFFFF
            ireg = op.mem.index_reg
            # idiom: move.w (T,pc,dX.w),dX ; jmp (T,pc,dX.w)
            for p in self.preds.get(a, []):
                pi = self.insns[p]
                if (mbase(pi) == "move" and msize(pi) == 2 and pi.operands[-1].type == M.M68K_OP_REG
                        and pi.operands[-1].reg == ireg
                        and pi.operands[0].address_mode in (M.M68K_AM_PCI_INDEX_8_BIT_DISP, M.M68K_AM_PCI_INDEX_BASE_DISP)
                        and (p + 2 + s8(pi.operands[0].mem.disp)) & 0xFFFFFF == base):
                    ents = self.word_table(base)
                    self.tables.setdefault(base, ("w", base))
                    self.table_len[base] = max(self.table_len.get(base, 0), len(ents))
                    return [(e, None, f"wtable {base:06x}") for e in ents], notes
            notes.append(f"pc-indexed jump into {base:06x} without a recognised offset table")
            return [], notes
        if am in (M.M68K_AM_AREGI_INDEX_8_BIT_DISP, M.M68K_AM_AREGI_INDEX_BASE_DISP):
            notes.append("register-indexed jump target")
            return [], notes
        notes.append(f"addressing mode {am} not handled")
        return [], notes

    def data_tables(self, in_gap, maxn=256):
        """Long pointer tables (to code or data) read via indexed long loads, located in a gap.

        Returns {base: [entry values]}.  Entries: even ROM addresses >= 0x200, stopping at
        a traced instruction or at an address one of the entries points to.
        """
        found = {}
        for a, i in sorted(self.insns.items()):
            if msize(i) != 4 or mbase(i) not in ("move", "movea") or len(i.operands) != 2:
                continue
            src = i.operands[0]
            am = src.address_mode
            if src.type != M.M68K_OP_MEM:
                continue
            if am in (M.M68K_AM_PCI_INDEX_8_BIT_DISP, M.M68K_AM_PCI_INDEX_BASE_DISP):
                bases = [(a + 2 + s8(src.mem.disp)) & 0xFFFFFF]
            elif am in (M.M68K_AM_AREGI_INDEX_8_BIT_DISP, M.M68K_AM_AREGI_INDEX_BASE_DISP):
                bases = [v[0] & 0xFFFFFF for v in self.slice(a, ("reg", src.mem.base_reg, s8(src.mem.disp)), [])]
            else:
                continue
            for b in bases:
                if b in found or b in self.tables or not in_gap(b) or b & 1:
                    continue
                ents = []
                for k in range(maxn):
                    p = b + 4 * k
                    if p + 4 > len(self.rom) or p in self.insns or p in ents or self.inside_insn(p):
                        break
                    v = struct.unpack(">I", self.rom[p:p + 4])[0]
                    if v & 1 or not 0x200 <= v < len(self.rom):
                        break
                    ents.append(v)
                if ents:
                    found[b] = ents
        return found

    def run(self):
        """Resolve all indirect sites; returns dict site -> (targets, notes)."""
        self.tables = {}
        self.table_len = {}
        sites = {}
        for a, i in sorted(self.insns.items()):
            if mbase(i) not in ("jmp", "jsr") or m68kfmt.branch_target(i) is not None:
                continue
            vals, notes = self.resolve_site(a)
            tg = []
            for v, ii, how in vals:
                v &= 0xFFFFFFFF
                if ii is not None:
                    self.imm_ptrs[ii] = v
                if v not in [t for t, _ in tg]:
                    tg.append((v, how))
            sites[a] = (tg, notes)
        return sites
