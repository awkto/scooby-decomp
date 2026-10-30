#!/usr/bin/env python3
"""Recursive-descent disassembler for the Scooby-Doo Mystery ROM.

Produces src/scooby.s, which must assemble back to a byte-identical ROM.
Code discovery: entry points from the vector table plus extra seeds in
tools/py/seeds.txt (one hex address per line, '#' comments), runtime starts from
data/exec_starts.txt, a gap-fill heuristic, and indirect jmp/jsr resolution
(tools/py/indirect.py: jump/pointer tables, RAM handler vectors, stack-passed
tables), iterated to a fixpoint.  Writes data/indirect.txt (every indirect site
and its targets) and data/code_gaps.txt (classified non-code ranges in the code
area; manual classes from data/gap_notes.txt).  Every decoded
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
import indirect  # noqa: E402
import symtab  # noqa: E402

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


def load_dataread(rom):
    dr_path = os.path.join(ROOT, "data", "rom_data_reads.txt")
    dataread = bytearray(len(rom))
    if os.path.exists(dr_path):
        for line in open(dr_path):
            if line.strip() and not line.startswith("#"):
                a, b = (int(x, 16) for x in line.split())
                dataread[a:b] = b"\x01" * (b - a)
    return dataread


def gapfill(rom, md, insns, labels, seeds, quiet, dataread):
    """Iteratively seed code that directly follows a block terminator."""
    seeds = list(seeds)
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
            return insns, labels, seeds
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


def code_gaps(insns):
    """Ranges inside CODE_REGIONS not covered by traced instructions."""
    gaps = []
    for lo, hi in CODE_REGIONS:
        pc = lo
        while pc < hi:
            if pc in insns:
                pc += insns[pc].size
                continue
            end = pc
            while end < hi and end not in insns:
                end += 1
            gaps.append((pc, end))
            pc = end
    return gaps


def write_indirect(sites, insns, tables):
    """data/indirect.txt: every indirect jmp/jsr site and its target set."""
    out = ["# Indirect jmp/jsr sites in traced code (generated by tools/py/disasm.py).",
           "# site  instruction  status  targets (hex) [how]  ; notes",
           "# status: RESOLVED (all value sources found), PARTIAL (targets found + caveats), UNRESOLVED",
           "# Tables: start kind entries   (l = code pointers, w = word offsets from base, d = data-pointer table in a code-area gap)"]
    for t, (kind, base, n) in sorted(tables.items()):
        out.append(f"#   table {t:06x} {kind} x{n}" + (f" base {base:06x}" if kind == "w" else ""))
    for a, (tg, notes) in sorted(sites.items()):
        i = insns[a]
        status = "UNRESOLVED" if not tg else ("PARTIAL" if notes else "RESOLVED")
        hows = sorted({h for _, h in tg})
        bad = [t for t, _ in tg if t not in insns]
        line = f"{a:06x}  {i.mnemonic} {i.op_str:<10}  {status}"
        if tg:
            line += "  " + " ".join(f"{t:06x}" for t, _ in tg) + "  [" + "; ".join(hows) + "]"
        if bad:
            notes = notes + ["not code: " + " ".join(f"{t:06x}" for t in bad)]
        if notes:
            line += "  ; " + "; ".join(dict.fromkeys(notes))
        out.append(line)
    open(os.path.join(ROOT, "data", "indirect.txt"), "w").write("\n".join(out) + "\n")


def load_gap_notes():
    notes = {}
    path = os.path.join(ROOT, "data", "gap_notes.txt")
    if os.path.exists(path):
        for line in open(path):
            if line.strip() and not line.startswith("#"):
                addr, cls, *rest = line.split(None, 2)
                notes[int(addr, 16)] = (cls, rest[0].strip() if rest else "")
    return notes


def write_gap_report(rom, md, gaps, table_ents, anchor, insns, dataread, refs_from):
    """data/code_gaps.txt: every non-code range in the code area, classified.

    Automatic class (overridden by data/gap_notes.txt, keyed by gap start):
      table    - every byte is a resolved code-pointer table
      padding  - all zero / all 0xFF
      string   - mostly printable ASCII
      dead?    - decodes cleanly to a terminator but is never reached
      data     - referenced by code or read at runtime, but not otherwise classified
      unknown  - none of the above
    """
    manual = load_gap_notes()
    out = ["# Non-code ranges inside the code area (generated by tools/py/disasm.py).",
           "# start   end     bytes  class    detail",
           "# class from data/gap_notes.txt where present (marked *), else automatic."]
    totals = {}
    for g0, g1 in gaps:
        blob = rom[g0:g1]
        refs = sorted(r for r in anchor if g0 <= r < g1)
        users = sorted({u for r in refs for u in refs_from.get(r, [])})
        nread = sum(dataread[g0:g1])
        tb = sum(2 if table_ents[p][0] == "w" else 4 for p in range(g0, g1) if p in table_ents)
        if g0 in manual:
            cls, detail = manual[g0]
            cls += "*"
        else:
            printable = sum(1 for c in blob if 32 <= c < 127 or c == 0)
            if tb >= len(blob):
                cls = "table"
            elif all(c == 0 for c in blob) or all(c == 0xFF for c in blob):
                cls = "padding"
            elif len(blob) >= 4 and printable >= 0.85 * len(blob) and sum(1 for c in blob if 65 <= c < 91 or 97 <= c < 123) >= len(blob) // 3:
                cls = "string"
            elif refs or nread:
                cls = "data"
            elif not g0 & 1 and g1 - g0 >= 4 and try_block(rom, md, g0, g1 + 0x100, dataread):
                cls = "dead?"
            else:
                cls = "unknown"
            detail = ""
        bits = []
        if detail:
            bits.append(detail)
        if refs:
            bits.append("refs " + ",".join(f"{r:06x}" for r in refs[:6]) + ("..." if len(refs) > 6 else ""))
        if users:
            bits.append("used by " + ",".join(f"{u:06x}" for u in users[:4]) + ("..." if len(users) > 4 else ""))
        if nread:
            bits.append(f"{nread} bytes read at runtime")
        if tb:
            kinds = sorted({table_ents[p][0] for p in range(g0, g1) if p in table_ents})
            bits.append(f"{tb} bytes symbolic pointer table ({'/'.join({'l': 'code', 'w': 'code-offset', 'd': 'data'}[k] for k in kinds)})")
        out.append(f"{g0:06x}  {g1:06x}  {g1 - g0:5d}  {cls:<8} {'; '.join(bits)}")
        totals[cls.rstrip('*')] = totals.get(cls.rstrip('*'), 0) + g1 - g0
    out.append("# totals: " + "  ".join(f"{k}={v}" for k, v in sorted(totals.items())))
    open(os.path.join(ROOT, "data", "code_gaps.txt"), "w").write("\n".join(out) + "\n")


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

    dataread = load_dataread(rom)
    quiet = observed - set(entries)
    seeds = entries + sorted(quiet)
    # Fixpoint: trace + gap fill, then resolve indirect jmp/jsr and seed the targets.
    while True:
        insns, labels = trace(rom, md, seeds, quiet=quiet)
        insns, labels, seeds = gapfill(rom, md, insns, labels, seeds, quiet, dataread)

        def codeok(v, insns=insns):
            if v & 1 or not any(lo <= v < hi for lo, hi in CODE_REGIONS) or dataread[v]:
                return False
            if v in insns:
                return True
            if any(v - d in insns and insns[v - d].size > d for d in (2, 4, 6, 8)):
                return False
            hi = next(h for lo, h in CODE_REGIONS if lo <= v < h)
            return try_block(rom, md, v, hi, dataread)

        res = indirect.Resolver(rom, insns, codeok)
        sites = res.run()
        new = sorted({t for tg, _ in sites.values() for t, _ in tg if t not in insns and codeok(t)})
        if not new:
            break
        seeds += new
    imm_ptrs = {a: v for a, v in res.imm_ptrs.items() if v in insns}
    # pointer tables: entry address -> target, kept only where every entry is traced code
    table_ents = {}
    tables = {}
    for t, (kind, base) in res.tables.items():
        n = res.table_len.get(t, 0)
        step = 4 if kind == "l" else 2
        ents = []
        for k in range(n):
            p = t + step * k
            v = (struct.unpack(">I", rom[p:p + 4])[0] if kind == "l"
                 else (base + m68kfmt.s16(struct.unpack(">H", rom[p:p + 2])[0])) & 0xFFFFFF)
            if v not in insns or p in insns:
                break
            ents.append((p, v))
        if ents:
            tables[t] = (kind, base, len(ents))
            for p, v in ents:
                table_ents[p] = (kind, base, v)
                labels.add(v)
    for v in imm_ptrs.values():
        labels.add(v)
    # long pointer tables (mostly to data) sitting in code-area gaps, read via indexed loads
    gap_set = set()
    for g0, g1 in code_gaps(insns):
        gap_set.update(range(g0, g1))
    dtables = res.data_tables(lambda b: b in gap_set)
    for t, ents in sorted(dtables.items()):
        ok = [v for v in ents if v in insns or not any(v - d in insns and insns[v - d].size > d for d in (2, 4, 6, 8))]
        if len(ok) != len(ents):
            continue
        tables[t] = ("d", t, len(ents))
        for k, v in enumerate(ents):
            table_ents.setdefault(t + 4 * k, ("d", t, v))
            if v in insns:
                labels.add(v)
    write_indirect(sites, insns, tables)
    bad = roundtrip_bad(rom, insns)

    # Collect every ROM address referenced pc-relatively or via absolute memory operands,
    # so they get labels (needed to reassemble, and to make the source relocatable).
    starts = sorted(insns)
    import bisect
    refs = set()
    refs_from = {}
    for a, insn in insns.items():
        for op in insn.operands:
            if op.type != m68kfmt.M.M68K_OP_MEM:
                continue
            am = op.address_mode
            r = None
            if am == m68kfmt.M.M68K_AM_PCI_DISP:
                r = a + 2 + m68kfmt.s16(op.mem.disp)
            elif am in (m68kfmt.M.M68K_AM_PCI_INDEX_8_BIT_DISP, m68kfmt.M.M68K_AM_PCI_INDEX_BASE_DISP):
                r = a + 2 + m68kfmt.s8(op.mem.disp)
            elif am == m68kfmt.M.M68K_AM_ABSOLUTE_DATA_LONG and (op.imm & 0xFFFFFFFF) < len(rom):
                r = op.imm & 0xFFFFFFFF
            if r is not None:
                refs.add(r)
                refs_from.setdefault(r, []).append(a)
    anchor = {}   # referenced addr -> (labelled addr, offset)
    for r in refs:
        i = bisect.bisect_right(starts, r) - 1
        if i >= 0 and starts[i] < r < starts[i] + insns[starts[i]].size:
            anchor[r] = (starts[i], r - starts[i])
            labels.add(starts[i])
        else:
            anchor[r] = (r, 0)
    data_labels = {r for r, (b, o) in anchor.items() if o == 0 and r not in insns}
    data_labels |= set(tables)
    data_labels |= {v for kind, base, v in table_ents.values() if v not in insns}
    for v in data_labels:
        anchor.setdefault(v, (v, 0))

    # ---- functions, symbols, RAM names (tools/py/symtab.py) ----
    sym = symtab.load_symbols()
    ram = symtab.RamMap(symtab.load_ram())
    fn_entries = set(entries)
    for a, insn in insns.items():
        if symtab.base(insn) in symtab.CALL:
            t = m68kfmt.branch_target(insn)
            if t in insns:
                fn_entries.add(t)
    for tg, _ in sites.values():
        fn_entries.update(t for t, _ in tg if t in insns)
    fn_entries.update(imm_ptrs.values())
    fn_entries.update(v for kind, b_, v in table_ents.values() if kind in "lw" and v in insns)
    owner, fn_entries = symtab.partition(insns, fn_entries)
    for e in fn_entries:
        names.setdefault(e, f"sub_{e:06X}")
        labels.add(e)
    for a, n in sym.items():
        if a in insns:
            labels.add(a)
        elif not any(a - d in insns and insns[a - d].size > d for d in (2, 4, 6, 8)):
            data_labels.add(a)
            anchor.setdefault(a, (a, 0))
        else:
            print(f"warning: symbols.txt {a:06x} {n} is inside an instruction", file=sys.stderr)
            continue
        names[a] = n

    def name_of(addr):
        if addr in names:
            return names[addr]
        return f"loc_{addr:06X}" if addr in insns else f"dat_{addr:06X}"

    def label_for(addr):
        if addr >= len(rom) and ram.ram:
            return ram.label(addr)
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
    ] + ram.equs()
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
                text = m68kfmt.fmt(insn, label_for)
                if pc in imm_ptrs:   # immediate that is a code pointer (stored to a RAM vector etc.)
                    text = text.replace(f"#0x{imm_ptrs[pc]:x}", f"#{name_of(imm_ptrs[pc])}", 1)
                elif insn.mnemonic in ("movea.l", "move.l", "cmpa.l") and insn.operands[0].type == m68kfmt.CS_OP_IMM:
                    rn = ram.exact.get(insn.operands[0].imm & 0xFFFFFFFF)   # RAM address passed as an immediate
                    if rn:
                        text = text.replace(f"#0x{insn.operands[0].imm & 0xFFFFFFFF:x}", f"#{rn}", 1)
                lines.append("\t" + text)
            code_bytes += insn.size
            pc += insn.size
        else:
            end = pc
            while end < len(rom) and end not in insns:
                end += 1
            lines.append(f"| data 0x{pc:06X}-0x{end:06X}")
            p = pc
            while p < end:
                if p in data_labels:
                    lines.append(f"{name_of(p)}:")
                te = table_ents.get(p)
                step = 2 if te and te[0] == "w" else 4
                if te and p + step <= end and not any(d in data_labels for d in range(p + 1, p + step)):
                    kind, base, v = te
                    if kind in "ld":
                        lines.append(f"\tdc.l {name_of(v)}")
                    else:
                        lines.append(f"\tdc.w {name_of(v)}-{name_of(base)}")
                    p += step
                    continue
                q = p + 1
                while q < end and q not in data_labels and q not in table_ents:
                    q += 1
                lines.append(f'\t.incbin "baserom.md",0x{p:X},0x{q - p:X}')
                p = q
            pc = end
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write("\n".join(lines) + "\n")
    gaps = code_gaps(insns)
    gap_bytes = sum(b - a for a, b in gaps)
    tbl_bytes = sum(1 for g0, g1 in gaps for p in range(g0, g1)
                    if any(p - k in table_ents and k < (2 if table_ents[p - k][0] == "w" else 4) for k in range(4)))
    n_res = sum(1 for tg, notes in sites.values() if tg and not notes)
    n_part = sum(1 for tg, notes in sites.values() if tg and notes)
    n_unres = sum(1 for tg, notes in sites.values() if not tg)
    print(f"instructions: {len(insns)}  code bytes: {code_bytes} ({100*code_bytes/len(rom):.1f}%)"
          f"  labels: {len(labels)}  non-roundtrip: {len(bad)}"
          f"  code-area gaps: {gap_bytes} bytes in {len(gaps)} ranges ({tbl_bytes} in symbolic pointer tables)"
          f"  indirect sites: {len(sites)} ({n_res} resolved, {n_part} partial, {n_unres} unresolved)")
    viol = sorted(a for a, i in insns.items() if any(dataread[a:a + i.size]))
    if viol:
        # Not a mis-trace: every one of these has its exec bit set (the CPU really ran it) AND some 68k access read
        # the same bytes as data.  They are all in the script-interpreter code ($259e script_op_1C,
        # $272c script_op_1E, $286c..$287e the compare ladder of the condition evaluator sub_002796), i.e. the code is correct as disassembled
        # and the extra data reads come from the interpreter fetching "script words" from a pointer that lies in
        # code (out-of-range object/script offsets under all-ones or random story states).  See docs/walkthrough.md.
        print(f"note: {len(viol)} traced instructions are also read as data (not a mis-trace; see docs/walkthrough.md):",
              " ".join(f"{a:06x}" for a in viol[:20]))
    write_gap_report(rom, md, gaps, table_ents, anchor, insns, dataread, refs_from)
    ind_sites = {a: [t for t, _ in tg] for a, (tg, _) in sites.items() if tg}
    slots = {p: v for p, (kind, b_, v) in table_ents.items() if v in insns}
    slots.update({a: v for a, v in imm_ptrs.items()})
    symtab.write_outputs(rom, insns, owner, fn_entries, ind_sites, slots, name_of, ram)
    from collections import Counter
    c = Counter(insns[a].mnemonic for a in bad)
    if c:
        print("bad by mnemonic:", c.most_common(15))


if __name__ == "__main__":
    main()
