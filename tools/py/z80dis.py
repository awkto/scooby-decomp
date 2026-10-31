#!/usr/bin/env python3
"""Z80 sound-driver disassembler for Scooby-Doo Mystery (Genesis).

Extracts the Z80 driver blob from the ROM ($1BB94A, $1886 bytes; the 68k
stub copies it to Z80 RAM at $0000) and writes src/z80/driver.s in z80asm
syntax.  Code is found by recursive descent from the Z80 entry points;
everything else (and every undocumented opcode z80asm cannot encode) is
emitted as `defb`.  `make z80check` re-assembles the file and compares it
byte-for-byte with the ROM.

Usage: .venv/bin/python -I tools/py/z80dis.py [rom] [out.s]
"""
import sys

BLOB_ROM = 0x1BB94A
BLOB_LEN = 0x1BD1D0 - 0x1BB94A

R8 = ['b', 'c', 'd', 'e', 'h', 'l', '(hl)', 'a']
RP = ['bc', 'de', 'hl', 'sp']
RP2 = ['bc', 'de', 'hl', 'af']
CC = ['nz', 'z', 'nc', 'c', 'po', 'pe', 'p', 'm']
ALU = ['add a,', 'adc a,', 'sub ', 'sbc a,', 'and ', 'xor ', 'or ', 'cp ']
ROT = ['rlc', 'rrc', 'rl', 'rr', 'sla', 'sra', None, 'srl']  # 6 = SLL (undoc)


import os
DEBUG = bool(os.environ.get('Z80DBG'))


def h8(v):
    return '0x%02x' % (v & 0xFF)


def h16(v):
    return '0x%04x' % (v & 0xFFFF)


def lab(a):
    return 'l_%04x' % a


class Insn:
    def __init__(self, addr, length, text, flow='next', target=None, ok=True):
        self.addr, self.length, self.text = addr, length, text
        self.flow = flow       # next | stop | jump | cond | call
        self.target = target
        self.ok = ok           # False => undocumented, emit as defb


def disp(d):
    d = d - 256 if d > 127 else d
    return ('+%d' % d) if d >= 0 else ('-%d' % -d)


def decode(m, a):
    """Decode one instruction at a (m = bytes of whole 64k-ish memory)."""
    op = m[a]
    n1 = lambda: m[a + 1]
    n2 = lambda: m[a + 1] | (m[a + 2] << 8)
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    if op == 0xCB:
        o = m[a + 1]
        x, y, z = o >> 6, (o >> 3) & 7, o & 7
        if x == 0:
            if y == 6:
                return Insn(a, 2, 'sll %s' % R8[z], ok=False)
            return Insn(a, 2, '%s %s' % (ROT[y], R8[z]))
        return Insn(a, 2, '%s %d,%s' % (['', 'bit', 'res', 'set'][x], y, R8[z]))
    if op == 0xED:
        o = m[a + 1]
        x, y, z = o >> 6, (o >> 3) & 7, o & 7
        p, q = y >> 1, y & 1
        if x == 1:
            if z == 0:
                if y == 6:
                    return Insn(a, 2, 'in (c)', ok=False)
                return Insn(a, 2, 'in %s,(c)' % R8[y])
            if z == 1:
                if y == 6:
                    return Insn(a, 2, 'out (c),0', ok=False)
                return Insn(a, 2, 'out (c),%s' % R8[y])
            if z == 2:
                return Insn(a, 2, '%s hl,%s' % ('adc' if q else 'sbc', RP[p]))
            if z == 3:
                if q == 0:
                    return Insn(a, 4, 'ld (%s),%s' % (h16(m[a + 2] | m[a + 3] << 8), RP[p]))
                return Insn(a, 4, 'ld %s,(%s)' % (RP[p], h16(m[a + 2] | m[a + 3] << 8)))
            if z == 4:
                return Insn(a, 2, 'neg', ok=(y == 0))
            if z == 5:
                return Insn(a, 2, 'reti' if y == 1 else 'retn', 'stop', ok=(y in (0, 1)))
            if z == 6:
                if y in (0, 1, 2, 3, 4, 5, 6, 7):
                    im = [0, 0, 1, 2, 0, 0, 1, 2][y]
                    return Insn(a, 2, 'im %d' % im, ok=(y in (0, 2, 3)))
            if z == 7:
                t = ['ld i,a', 'ld r,a', 'ld a,i', 'ld a,r', 'rrd', 'rld', 'nop', 'nop'][y]
                return Insn(a, 2, t, ok=(y < 6))
        if x == 2 and z <= 3 and y >= 4:
            t = [['ldi', 'cpi', 'ini', 'outi'], ['ldd', 'cpd', 'ind', 'outd'],
                 ['ldir', 'cpir', 'inir', 'otir'], ['lddr', 'cpdr', 'indr', 'otdr']][y - 4][z]
            return Insn(a, 2, t)
        return Insn(a, 2, 'nop', ok=False)  # ED nop
    if op in (0xDD, 0xFD):
        ir = 'ix' if op == 0xDD else 'iy'
        o = m[a + 1]
        x, y, z = o >> 6, (o >> 3) & 7, o & 7
        p, q = y >> 1, y & 1
        if o == 0xCB:
            d = m[a + 2]
            o2 = m[a + 3]
            x2, y2, z2 = o2 >> 6, (o2 >> 3) & 7, o2 & 7
            ea = '(%s%s)' % (ir, disp(d))
            if z2 != 6:
                return Insn(a, 4, 'x', ok=False)
            if x2 == 0:
                if y2 == 6:
                    return Insn(a, 4, 'x', ok=False)
                return Insn(a, 4, '%s %s' % (ROT[y2], ea))
            return Insn(a, 4, '%s %d,%s' % (['', 'bit', 'res', 'set'][x2], y2, ea))
        ea = lambda: '(%s%s)' % (ir, disp(m[a + 2]))
        if o == 0x21:
            return Insn(a, 4, 'ld %s,%s' % (ir, h16(m[a + 2] | m[a + 3] << 8)))
        if o == 0x22:
            return Insn(a, 4, 'ld (%s),%s' % (h16(m[a + 2] | m[a + 3] << 8), ir))
        if o == 0x2A:
            return Insn(a, 4, 'ld %s,(%s)' % (ir, h16(m[a + 2] | m[a + 3] << 8)))
        if o in (0x09, 0x19, 0x29, 0x39):
            r = ['bc', 'de', ir, 'sp'][(o >> 4) & 3]
            return Insn(a, 2, 'add %s,%s' % (ir, r), ok=(o != 0x29 or True))
        if o == 0x23:
            return Insn(a, 2, 'inc %s' % ir)
        if o == 0x2B:
            return Insn(a, 2, 'dec %s' % ir)
        if o == 0x34:
            return Insn(a, 3, 'inc %s' % ea())
        if o == 0x35:
            return Insn(a, 3, 'dec %s' % ea())
        if o == 0x36:
            return Insn(a, 4, 'ld %s,%s' % (ea(), h8(m[a + 3])))
        if x == 1 and (y == 6) != (z == 6):
            if y == 6:
                return Insn(a, 3, 'ld %s,%s' % (ea(), R8[z]))
            return Insn(a, 3, 'ld %s,%s' % (R8[y], ea()))
        if x == 2 and z == 6:
            return Insn(a, 3, '%s%s' % (ALU[y], ea()))
        if o == 0xE1:
            return Insn(a, 2, 'pop %s' % ir)
        if o == 0xE5:
            return Insn(a, 2, 'push %s' % ir)
        if o == 0xE3:
            return Insn(a, 2, 'ex (sp),%s' % ir)
        if o == 0xE9:
            return Insn(a, 2, 'jp (%s)' % ir, 'stop')
        if o == 0xF9:
            return Insn(a, 2, 'ld sp,%s' % ir)
        return Insn(a, 1, 'nop', ok=False)  # prefix with no effect / undoc
    if x == 0:
        if z == 0:
            if y == 0:
                return Insn(a, 1, 'nop')
            if y == 1:
                return Insn(a, 1, "ex af,af'")
            d = n1()
            t = a + 2 + (d - 256 if d > 127 else d)
            t &= 0xFFFF
            if y == 2:
                return Insn(a, 2, 'djnz %s' % lab(t), 'cond', t)
            if y == 3:
                return Insn(a, 2, 'jr %s' % lab(t), 'jump', t)
            return Insn(a, 2, 'jr %s,%s' % (CC[y - 4], lab(t)), 'cond', t)
        if z == 1:
            if q == 0:
                return Insn(a, 3, 'ld %s,%s' % (RP[p], h16(n2())))
            return Insn(a, 1, 'add hl,%s' % RP[p])
        if z == 2:
            if q == 0:
                return Insn(a, [1, 1, 3, 3][p], ['ld (bc),a', 'ld (de),a',
                            'ld (%s),hl' % h16(n2()), 'ld (%s),a' % h16(n2())][p])
            return Insn(a, [1, 1, 3, 3][p], ['ld a,(bc)', 'ld a,(de)',
                        'ld hl,(%s)' % h16(n2()), 'ld a,(%s)' % h16(n2())][p])
        if z == 3:
            return Insn(a, 1, ('inc ' if q == 0 else 'dec ') + RP[p])
        if z == 4:
            return Insn(a, 1, 'inc %s' % R8[y])
        if z == 5:
            return Insn(a, 1, 'dec %s' % R8[y])
        if z == 6:
            return Insn(a, 2, 'ld %s,%s' % (R8[y], h8(n1())))
        return Insn(a, 1, ['rlca', 'rrca', 'rla', 'rra', 'daa', 'cpl', 'scf', 'ccf'][y])
    if x == 1:
        if op == 0x76:
            return Insn(a, 1, 'halt')
        return Insn(a, 1, 'ld %s,%s' % (R8[y], R8[z]))
    if x == 2:
        return Insn(a, 1, '%s%s' % (ALU[y], R8[z]))
    # x == 3
    if z == 0:
        return Insn(a, 1, 'ret %s' % CC[y], 'cond')
    if z == 1:
        if q == 0:
            return Insn(a, 1, 'pop %s' % RP2[p])
        if p == 0:
            return Insn(a, 1, 'ret', 'stop')
        if p == 1:
            return Insn(a, 1, 'exx')
        if p == 2:
            return Insn(a, 1, 'jp (hl)', 'stop')
        return Insn(a, 1, 'ld sp,hl')
    if z == 2:
        t = n2()
        return Insn(a, 3, 'jp %s,%s' % (CC[y], lab(t)), 'cond', t)
    if z == 3:
        if y == 0:
            t = n2()
            return Insn(a, 3, 'jp %s' % lab(t), 'jump', t)
        if y == 2:
            return Insn(a, 2, 'out (%s),a' % h8(n1()))
        if y == 3:
            return Insn(a, 2, 'in a,(%s)' % h8(n1()))
        if y == 4:
            return Insn(a, 1, 'ex (sp),hl')
        if y == 5:
            return Insn(a, 1, 'ex de,hl')
        if y == 6:
            return Insn(a, 1, 'di')
        if y == 7:
            return Insn(a, 1, 'ei')
    if z == 4:
        t = n2()
        return Insn(a, 3, 'call %s,%s' % (CC[y], lab(t)), 'call', t)
    if z == 5:
        if q == 0:
            return Insn(a, 1, 'push %s' % RP2[p])
        t = n2()
        return Insn(a, 3, 'call %s' % lab(t), 'call', t)
    if z == 6:
        return Insn(a, 2, '%s%s' % (ALU[y], h8(n1())))
    return Insn(a, 1, 'rst %s' % h8(y * 8), 'call', y * 8)


# Extra code entry points (jump-table targets etc.) discovered by hand,
# and data ranges (start, end, kind) that must not be decoded as code.
EXTRA_ENTRIES = []
DATA_RANGES = []


def trace(m, entries, size):
    insns = {}
    work = list(entries)
    while work:
        a = work.pop()
        while 0 <= a < size and a not in insns:
            if any(lo <= a < hi for lo, hi, _ in DATA_RANGES):
                break
            try:
                i = decode(m, a)
            except IndexError:
                break
            if not i.ok or a + i.length > size:
                break
            insns[a] = i
            if i.target is not None and i.flow in ('jump', 'cond', 'call'):
                work.append(i.target)
            if i.flow in ('stop', 'jump'):
                break
            a += i.length
    return insns


def main():
    rom = sys.argv[1] if len(sys.argv) > 1 else 'rom/baserom.md'
    out = sys.argv[2] if len(sys.argv) > 2 else 'src/z80/driver.s'
    blob = open(rom, 'rb').read()[BLOB_ROM:BLOB_ROM + BLOB_LEN]
    m = blob + b'\0' * 8
    size = len(blob)
    from z80dis_cfg import ENTRIES, DATA, WORD_TABLES, COMMENTS, NAMES
    DATA_RANGES.extend(DATA)
    insns = trace(m, [0x0000, 0x0038, 0x0066] + ENTRIES, size)
    labels = set()
    for i in insns.values():
        if i.target is not None and i.flow in ('jump', 'cond', 'call'):
            labels.add(i.target)
    for lo, hi, kind in DATA:
        labels.add(lo)
    labels.update(a for lo, hi, _ in WORD_TABLES for a in (lo,))
    words = {}
    lines = ['; Scooby-Doo Mystery (Genesis) - Z80 sound driver, 0x%04x bytes.' % size,
             '; ROM $%06X..$%06X, copied to Z80 RAM $0000 by the 68k stub.' % (BLOB_ROM, BLOB_ROM + BLOB_LEN),
             '; Generated by tools/py/z80dis.py; verified byte-exact by `make z80check`.',
             '', '\torg 0x0000', '']
    a = 0
    while a < size:
        if a in labels:
            lines.append('%s:' % lab(a))
        if a in COMMENTS:
            for cl in COMMENTS[a].split('\n'):
                lines.append('; ' + cl)
        if a in insns and any(k in labels for k in range(a + 1, a + insns[a].length)):
            # a jump lands inside this instruction: emit it byte-wise
            i = insns[a]
            lines.append('\t; %s' % i.text)
            for k in range(a, a + i.length):
                if k != a and k in labels:
                    lines.append('%s:' % lab(k))
                lines.append('\tdefb 0x%02x' % blob[k])
            a += i.length
            continue
        if a in insns:
            i = insns[a]
            lines.append('\t%s%s' % (i.text, ('\t; %04x' % a) if DEBUG else ''))
            a += i.length
            continue
        # data run: until next label or code
        end = a
        while end < size and (end == a or (end not in labels and end not in insns and end not in COMMENTS)):
            end += 1
        tbl = next((t for t in WORD_TABLES if t[0] <= a < t[1]), None)
        if tbl and a % 2 == 0 and tbl[0] == a:
            lines.append('\t; %d words' % ((tbl[1] - tbl[0]) // 2))
            lines.append('\tincbin "tab_%X_%X.bin"' % (BLOB_ROM + tbl[0], tbl[1] - tbl[0]))
            a = tbl[1]
            continue
        end = min(end, tbl[0] if tbl and tbl[0] > a else end)
        # break data lines at 16 bytes, and at word-table starts
        stops = sorted(t[0] for t in WORD_TABLES if t[0] > a)
        if stops:
            end = min(end, stops[0])
        k = a
        if end - a >= 24 and len(set(blob[a:end])) > 4:
            # table / text data: sliced from the base ROM at build time (Makefile rule tab_<romoff>_<len>.bin) (no ROM bytes in the repo)
            lines.append('\tincbin "tab_%X_%X.bin"' % (BLOB_ROM + a, end - a))
            k = end
        while k < end:
            chunk = blob[k:min(k + 16, end)]
            lines.append('\tdefb ' + ','.join('0x%02x' % b for b in chunk))
            k += len(chunk)
        a = end
    import re
    ren = lambda mo: NAMES.get(int(mo.group(1), 16), mo.group(0))
    lines = [re.sub(r'\bl_([0-9a-f]{4})\b', ren, ln) for ln in lines]
    with open(out, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    nb = sum(i.length for i in insns.values())
    print('z80dis: %d insns, %d code bytes of %d, %d labels' % (len(insns), nb, size, len(labels)))


if __name__ == '__main__':
    sys.path.insert(0, __file__.rsplit('/', 1)[0])
    main()
