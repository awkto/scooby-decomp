#!/usr/bin/env python3
"""Recursive-descent disassembler for the Scooby-Doo Mystery ROM.

Produces src/scooby.s, which must assemble back to a byte-identical ROM.
Code discovery: entry points from the vector table plus extra seeds in
tools/py/seeds.txt (one hex address per line, '#' comments).  Every decoded
instruction is round-trip checked with GAS; instructions that do not
reproduce exactly are emitted as dc.w with the mnemonic as a comment.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from capstone import Cs, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_000  # noqa: E402
import m68kfmt  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ROM = os.path.join(ROOT, "rom", "baserom.md")
SEEDS = os.path.join(os.path.dirname(__file__), "seeds.txt")
OUT = os.path.join(ROOT, "src", "scooby.s")
AS = ["m68k-linux-gnu-as", "-m68000", "--register-prefix-optional"]

END_MNEMONICS = {"rts", "rte", "rtr", "bra", "jmp", "illegal"}


def load_seeds():
    seeds = {}
    if os.path.exists(SEEDS):
        for line in open(SEEDS):
            line = line.split("#", 1)
            name = None
            body = line[0].split()
            if not body:
                continue
            addr = int(body[0], 16)
            if len(body) > 1:
                name = body[1]
            seeds[addr] = name
    return seeds


def trace(rom, md, entries, quiet=()):
    insns = {}       # addr -> capstone insn
    labels = set()   # code addresses that are branch targets / entries
    work = list(entries)
    while work:
        pc = work.pop()
        if pc not in quiet:
            labels.add(pc)
        while pc < len(rom) and pc not in insns:
            if pc & 1:
                break
            try:
                insn = next(md.disasm(rom[pc:pc + 10], pc, 1))
            except StopIteration:
                break
            if insn.mnemonic == "dc.w":
                print(f"warning: invalid opcode traced at 0x{pc:06x}", file=sys.stderr)
                break
            insns[pc] = insn
            tgt = m68kfmt.branch_target(insn)
            if tgt is not None and tgt < len(rom) and not tgt & 1:
                work.append(tgt)
                labels.add(tgt)
            base = insn.mnemonic.split(".")[0]
            if base in END_MNEMONICS:
                break
            pc += insn.size
    return insns, labels


CODE_REGIONS = [(0x200, 0x11000), (0x1F6D7A, 0x1F7082)]


def try_block(rom, md, start, end, dataread):
    """Decode linearly from start; accept only if it reaches a terminator cleanly."""
    pc = start
    while pc < end:
        if dataread[pc] or dataread[pc + 1]:
            return False
        try:
            insn = next(md.disasm(rom[pc:pc + 10], pc, 1))
        except StopIteration:
            return False
        if insn.mnemonic == "dc.w":   # capstone's marker for an invalid opcode
            return False
        if rom[pc:pc + 2] == b"\x00\x00":   # ori.b #0,d0: almost always padding/data
            return False
        if any(dataread[pc + k] for k in range(insn.size)):
            return False
        tgt = m68kfmt.branch_target(insn)
        if tgt is not None and (tgt & 1 or not any(a <= tgt < b for a, b in CODE_REGIONS)):
            return False
        if insn.mnemonic.split(".")[0] in END_MNEMONICS:
            return True
        pc += insn.size
    return False


def gapfill(rom, md, insns, labels, entries, observed):
    """Iteratively seed code that directly follows a block terminator."""
    dr_path = os.path.join(ROOT, "data", "rom_data_reads.txt")
    dataread = bytearray(len(rom))
    if os.path.exists(dr_path):
        for line in open(dr_path):
            if line.strip() and not line.startswith("#"):
                a, b = (int(x, 16) for x in line.split())
                dataread[a:b] = b"\x01" * (b - a)
    seeds = list(entries) + sorted(observed)
    quiet = set(observed) - set(entries)
    while True:
        new = []
        for a, insn in insns.items():
            if insn.mnemonic.split(".")[0] not in END_MNEMONICS:
                continue
            nxt = a + insn.size
            # skip alignment padding
            while nxt in range(len(rom) - 1) and rom[nxt:nxt + 2] == b"\x00\x00" and nxt not in insns:
                nxt += 2
            if nxt in insns or not any(lo <= nxt < hi for lo, hi in CODE_REGIONS):
                continue
            hi = next(h for lo, h in CODE_REGIONS if lo <= nxt < h)
            if try_block(rom, md, nxt, hi, dataread):
                new.append(nxt)
        if not new:
            return insns, labels
        seeds += new
        insns, labels = trace(rom, md, seeds, quiet=quiet)


def roundtrip_bad(rom, insns):
    """Assemble every instruction at its own address; return set of bad addrs."""
    bad = set()
    texts = {}
    for a, insn in insns.items():
        try:
            texts[a] = m68kfmt.fmt(insn, lambda t: f"(B+0x{t:x})")
        except m68kfmt.Unsupported:
            bad.add(a)
    pending = dict(texts)
    with tempfile.TemporaryDirectory() as td:
        while pending:
            src = [".text", "B:"]
            order = sorted(pending)
            for a in order:
                src.append(f".org 0x{a:x}")
                src.append(" " + pending[a])
            s = os.path.join(td, "c.s")
            o = os.path.join(td, "c.o")
            b = os.path.join(td, "c.bin")
            open(s, "w").write("\n".join(src) + "\n")
            r = subprocess.run(AS + ["-o", o, s], capture_output=True, text=True)
            errs = set()
            for m in re.finditer(r"c\.s:(\d+): Error", r.stderr):
                ln = int(m.group(1)) - 3   # 1-based, minus .text/B: lines
                idx = ln // 2
                # an .org error points at the .org line after an overlong insn
                if ln % 2 == 0 and idx > 0:
                    idx -= 1
                if 0 <= idx < len(order):
                    errs.add(order[idx])
            if errs:
                for a in errs:
                    bad.add(a)
                    del pending[a]
                continue
            subprocess.run(["m68k-linux-gnu-ld", "-e", "0", "-Ttext=0", "--oformat=binary", "-o", b, o], check=True)
            out = open(b, "rb").read()
            for a in order:
                n = insns[a].size
                if out[a:a + n] != rom[a:a + n]:
                    bad.add(a)
            break
    return bad


def main():
    rom = open(ROM, "rb").read()
    md = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_000)
    md.detail = True

    names = {}
    vec = struct.unpack(">64I", rom[:256])
    entries = []
    vec_names = {1: "Reset", 2: "BusError", 3: "AddressError", 4: "IllegalInsn",
                 5: "ZeroDivide", 28: "HBlank", 30: "VBlank", 26: "ExtInt"}
    for i in range(1, 64):
        if vec[i] < len(rom):
            entries.append(vec[i])
            if i in vec_names:
                names.setdefault(vec[i], vec_names[i])
    exec_file = os.path.join(ROOT, "data", "exec_starts.txt")
    observed = set()
    if os.path.exists(exec_file):
        for line in open(exec_file):
            if line.strip() and not line.startswith("#"):
                observed.add(int(line, 16))
    for a, n in load_seeds().items():
        entries.append(a)
        if n:
            names[a] = n

    insns, labels = trace(rom, md, entries + sorted(observed - set(entries)), quiet=observed - set(entries))
    insns, labels = gapfill(rom, md, insns, labels, entries, observed)
    bad = roundtrip_bad(rom, insns)

    # Collect every ROM address referenced pc-relatively or via absolute memory operands,
    # so they get labels (needed to reassemble, and to make the source relocatable).
    starts = sorted(insns)
    import bisect
    refs = set()
    for a, insn in insns.items():
        for op in insn.operands:
            if op.type != m68kfmt.M.M68K_OP_MEM:
                continue
            am = op.address_mode
            if am == m68kfmt.M.M68K_AM_PCI_DISP:
                refs.add(a + 2 + m68kfmt.s16(op.mem.disp))
            elif am in (m68kfmt.M.M68K_AM_PCI_INDEX_8_BIT_DISP, m68kfmt.M.M68K_AM_PCI_INDEX_BASE_DISP):
                refs.add(a + 2 + m68kfmt.s8(op.mem.disp))
            elif am == m68kfmt.M.M68K_AM_ABSOLUTE_DATA_LONG and (op.imm & 0xFFFFFFFF) < len(rom):
                refs.add(op.imm & 0xFFFFFFFF)
    anchor = {}   # referenced addr -> (labelled addr, offset)
    for r in refs:
        i = bisect.bisect_right(starts, r) - 1
        if i >= 0 and starts[i] < r < starts[i] + insns[starts[i]].size:
            anchor[r] = (starts[i], r - starts[i])
            labels.add(starts[i])
        else:
            anchor[r] = (r, 0)
    data_labels = {r for r, (b, o) in anchor.items() if o == 0 and r not in insns}

    def name_of(addr):
        if addr in names:
            return names[addr]
        return f"loc_{addr:06X}" if addr in insns else f"dat_{addr:06X}"

    def label_for(addr):
        if addr in labels and addr in insns:
            return name_of(addr)
        if addr in anchor:
            b, o = anchor[addr]
            return name_of(b) + (f"+{o}" if o else "")
        return None

    lines = [
        "| Scooby-Doo Mystery (USA) - generated by tools/py/disasm.py",
        "| Assemble with: m68k-linux-gnu-as -m68000 --register-prefix-optional",
        "\t.text",
    ]
    pc = 0
    code_bytes = 0
    while pc < len(rom):
        if pc in insns:
            insn = insns[pc]
            if pc in labels:
                lines.append(f"{name_of(pc)}:")
            if pc in bad:
                words = struct.unpack(f">{insn.size // 2}H", rom[pc:pc + insn.size])
                lines.append("\tdc.w " + ",".join(f"0x{w:04x}" for w in words)
                             + f"\t| {insn.mnemonic} {insn.op_str}")
            else:
                lines.append("\t" + m68kfmt.fmt(insn, label_for))
            code_bytes += insn.size
            pc += insn.size
        else:
            end = pc
            while end < len(rom) and end not in insns:
                end += 1
            lines.append(f"| data 0x{pc:06X}-0x{end:06X}")
            cuts = sorted({pc, end} | {d for d in data_labels if pc < d < end})
            for c0, c1 in zip(cuts, cuts[1:]):
                if c0 in data_labels:
                    lines.append(f"{name_of(c0)}:")
                lines.append(f'\t.incbin "baserom.md",0x{c0:X},0x{c1 - c0:X}')
            pc = end
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write("\n".join(lines) + "\n")
    print(f"instructions: {len(insns)}  code bytes: {code_bytes} ({100*code_bytes/len(rom):.1f}%)"
          f"  labels: {len(labels)}  non-roundtrip: {len(bad)}")
    from collections import Counter
    c = Counter(insns[a].mnemonic for a in bad)
    if c:
        print("bad by mnemonic:", c.most_common(15))


if __name__ == "__main__":
    main()
