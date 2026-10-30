"""Format capstone m68k instructions as GNU as (Motorola, --register-prefix-optional) source.

Every addressing mode is emitted with an explicit size so GAS cannot pick a different
encoding than the original.  Callers must still verify round-trips: anything this
formatter gets wrong is caught by the build check and falls back to dc.w.
"""
from capstone import m68k as M
from capstone import CS_OP_IMM

REG_NAMES = {}
for i in range(8):
    REG_NAMES[getattr(M, f"M68K_REG_D{i}")] = f"d{i}"
    REG_NAMES[getattr(M, f"M68K_REG_A{i}")] = f"a{i}"
REG_NAMES[M.M68K_REG_A7] = "sp"
REG_NAMES[M.M68K_REG_PC] = "pc"
REG_NAMES[M.M68K_REG_SR] = "sr"
REG_NAMES[M.M68K_REG_CCR] = "ccr"
REG_NAMES[M.M68K_REG_USP] = "usp"

SIZE = {1: "b", 2: "w", 4: "l"}


def hx(v):
    return f"-0x{-v:x}" if v < 0 else f"0x{v:x}"


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s8(v):
    v &= 0xFF
    return v - 0x100 if v & 0x80 else v


class Unsupported(Exception):
    pass


def reglist(bits):
    names = [f"d{i}" for i in range(8)] + [f"a{i}" for i in range(8)]
    out, i = [], 0
    while i < 16:
        if bits >> i & 1:
            j = i
            while j + 1 < 16 and bits >> (j + 1) & 1 and (j + 1) // 8 == i // 8:
                j += 1
            out.append(names[i] if i == j else f"{names[i]}-{names[j]}")
            i = j + 1
        else:
            i += 1
    return "/".join(out)


def fmt_operand(insn, op, label_for):
    """label_for(addr) -> label string or None (for pc-relative / absolute targets)."""
    t = op.type
    am = op.address_mode
    if t == M.M68K_OP_REG:
        return REG_NAMES[op.reg]
    if t == M.M68K_OP_IMM:
        return "#" + hx(op.imm)
    if t == M.M68K_OP_REG_BITS:
        return reglist(op.register_bits)
    if t == M.M68K_OP_BR_DISP:
        tgt = insn.address + 2 + op.br_disp.disp
        return label_for(tgt) or hx(tgt)
    if t != M.M68K_OP_MEM:
        raise Unsupported(f"op type {t}")
    m = op.mem
    if am == M.M68K_AM_REGI_ADDR:
        return f"({REG_NAMES[op.reg]})"
    if am == M.M68K_AM_REGI_ADDR_POST_INC:
        return f"({REG_NAMES[op.reg]})+"
    if am == M.M68K_AM_REGI_ADDR_PRE_DEC:
        return f"-({REG_NAMES[op.reg]})"
    if am == M.M68K_AM_REGI_ADDR_DISP:
        return f"({hx(s16(m.disp))},{REG_NAMES[m.base_reg]})"
    if am in (M.M68K_AM_AREGI_INDEX_8_BIT_DISP, M.M68K_AM_AREGI_INDEX_BASE_DISP):  # capstone uses BASE_DISP for d8=0
        idx = REG_NAMES[m.index_reg] + "." + ("l" if m.index_size else "w")
        return f"({hx(s8(m.disp))},{REG_NAMES[m.base_reg]},{idx})"
    if am == M.M68K_AM_PCI_DISP:
        tgt = (insn.address + 2 + s16(m.disp)) & 0xFFFFFFFF
        lab = label_for(tgt)
        return f"({lab},pc)" if lab else f"({hx(tgt)},pc)"
    if am in (M.M68K_AM_PCI_INDEX_8_BIT_DISP, M.M68K_AM_PCI_INDEX_BASE_DISP):
        tgt = (insn.address + 2 + s8(m.disp)) & 0xFFFFFFFF
        idx = REG_NAMES[m.index_reg] + "." + ("l" if m.index_size else "w")
        lab = label_for(tgt)
        return f"({lab or hx(tgt)},pc,{idx})"
    if am == M.M68K_AM_ABSOLUTE_DATA_SHORT:
        return f"({hx(s16(op.imm))}).w"
    if am == M.M68K_AM_ABSOLUTE_DATA_LONG:
        v = op.imm & 0xFFFFFFFF
        lab = label_for(v)
        return f"({lab or hx(v)}).l"
    raise Unsupported(f"addr mode {am}")


def fmt(insn, label_for=lambda a: None):
    mn = insn.mnemonic
    # capstone spells some mnemonics differently from GAS
    mn = {"bra.b": "bra.s", "bsr.b": "bsr.s"}.get(mn, mn)
    if mn.startswith("b") and mn.endswith(".b") and len(mn) <= 5:
        mn = mn[:-2] + ".s"
    ops = [fmt_operand(insn, op, label_for) for op in insn.operands]
    return f"{mn} {','.join(ops)}".rstrip()


def branch_target(insn):
    """Return static control-flow target address, or None."""
    for op in insn.operands:
        if op.type == M.M68K_OP_BR_DISP:
            return insn.address + 2 + op.br_disp.disp
    if insn.mnemonic.startswith(("jmp", "jsr")):
        op = insn.operands[0]
        if op.type == M.M68K_OP_MEM:
            if op.address_mode == M.M68K_AM_ABSOLUTE_DATA_LONG:
                return op.imm & 0xFFFFFFFF
            if op.address_mode == M.M68K_AM_ABSOLUTE_DATA_SHORT:
                return s16(op.imm) & 0xFFFFFFFF
            if op.address_mode == M.M68K_AM_PCI_DISP:
                return insn.address + 2 + s16(op.mem.disp)
    return None
