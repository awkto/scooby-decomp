#!/usr/bin/env python3
"""List every game string with its ROM address (GitLab #20; text system: docs/subsystems/text.md).

    tools/py/textdump.py [--out build/textdump.txt]

Strings are plain ASCII, NUL terminated. Where they live:

  * the two per-mystery text pools (mystery header +$20: Blake's Hotel $14199C, Ha Ha Carnival $1AF33C),
    reached by long offsets from `say` (op $10), `talk_choice` (op $02, text and reply), the room table
    (entry +$00 = room name) and the object table (+$04 of each 8-byte entry = object name);
  * the shared UI strings in the code bank: verb words ($02FE5E), failure messages ($02FEA0 table, text
    $02FECC..; "\\x01" is replaced by the object name / pronoun), " with " / " to " / pronouns, the blank
    line, and every string a `lea (X).l,a0` hands to one of the text routines (title / password / sound
    test / options screens).

The output is ROM-derived: it goes under build/ (gitignored), never into git. Format, one per line:

    ADDR   KIND           CONTEXT                       "text"

Only the string's first reference is listed with its context; later references to the same address
are counted in the `refs` column of the summary at the top.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import scriptdis as S  # noqa: E402

TEXT_FUNCS = ('draw_text_plane_a', 'sub_009F30', 'sub_009F46', 'sub_009F58', 'sub_009FA2', 'sub_00A054',
              'sub_00A130', 'strlen', 'str_length', 'draw_text_centered', 'draw_text_priority',
              'draw_text_line_clear_first', 'talk_line_print', 'text_box_print', 'text_show', 'region_print')


def esc(b):
    s = b.decode('latin-1')
    return s.replace('\\', '\\\\').replace('"', '\\"').replace('\x01', '\\x01').replace('\n', '\\n')


def cstr_at(rom, a):
    e = rom.index(b'\0', a)
    return rom[a:e]


def symbols():
    """label -> address from data/symbols.txt (`ADDR name  # ...`)."""
    syms = {}
    p = os.path.join(ROOT, 'data', 'symbols.txt')
    for line in open(p):
        line = line.split('#')[0].split()
        if len(line) >= 2 and re.fullmatch(r'[0-9A-Fa-f]{6}', line[0]):
            syms[line[1]] = int(line[0], 16)
    return syms


def code_string_refs(rom):
    """Addresses of strings loaded into A0 right before a text routine is called (src/scooby.s).
    Returns {addr: 'caller-function-or-label'}."""
    syms = symbols()
    refs = {}
    src = open(os.path.join(ROOT, 'src', 'scooby.s')).read().splitlines()
    pending = {}
    cur = ''
    for line in src:
        m = re.match(r'^([A-Za-z_][A-Za-z_0-9]*):', line)
        if m:
            cur = m.group(1)
            continue
        line = line.strip()
        m = re.match(r'lea\.l \(([A-Za-z_0-9]+)\)\.l,a0', line)
        if m:
            name = m.group(1)
            mm = re.fullmatch(r'(?:dat|loc|sub)_([0-9A-F]{6})', name)
            addr = int(mm.group(1), 16) if mm else syms.get(name)
            if addr is not None:
                pending = {'addr': addr, 'left': 6}
            continue
        if pending:
            m = re.match(r'(?:bsr\.[ws]|jsr|bra\.w|jmp)\s+\(?([A-Za-z_0-9]+)', line)
            if m and m.group(1) in TEXT_FUNCS:
                if 0 < pending['addr'] < len(rom) and pending['addr'] not in refs:
                    refs[pending['addr']] = '%s -> %s' % (cur, m.group(1))
                pending = {}
                continue
            pending['left'] -= 1
            if pending['left'] <= 0:
                pending = {}
    return refs


def main(argv):
    out = os.path.join(ROOT, 'build', 'textdump.txt')
    if len(argv) >= 2 and argv[0] == '--out':
        out = argv[1]
    rom = open(S.ROM_PATH, 'rb').read()
    entries = {}     # addr -> [kind, context, refcount]

    def add(addr, kind, ctx):
        if addr >= len(rom):
            return
        if addr in entries:
            entries[addr][2] += 1
        else:
            entries[addr] = [kind, ctx, 1]

    for wi in (0, 1):
        w = S.World(rom, wi)
        tag = 'w%d' % wi
        for i in range(w.nrooms):
            add(w.text + S.u32(rom, w.rooms + 0x14 * i), 'room_name', '%s room %d' % (tag, i + 2))
        for i in range(w.nobj):
            add(w.text + S.u32(rom, w.obj_table + 8 * i + 4), 'object_name', '%s object #%d' % (tag, i + 3))
        for label, hdr, b0, b1 in S.script_list(w):
            d = S.Dis(w)
            d.block(b0, b1, 0)
            short = label.split(' (')[0]
            for a, (op, n) in sorted(d.starts.items()):
                if op == 0x10:
                    attr = S.u16(rom, a + 2)
                    add(w.text + S.u32(rom, a + 4), 'say',
                        '%s %s @%06X colour=%d pal=%d%s%s' % (tag, short, a, attr & 15, (attr >> 4) & 3,
                                                              ' talk' if attr & 0x1000 else '',
                                                              ' turn=%d' % ((attr >> 14) & 3) if attr & 0x2000 else ''))
                elif op == 0x02:
                    add(w.text + S.u32(rom, a + 2), 'talk_choice', '%s %s @%06X' % (tag, short, a))
                    add(w.text + S.u32(rom, a + 6), 'talk_reply', '%s %s @%06X' % (tag, short, a))

    # shared UI strings
    a = 0x02FE5E
    n = 0
    while n < 11:
        s = cstr_at(rom, a)
        add(a, 'verb_word', 'verb id %d' % n)
        a += len(s) + 1
        n += 1
    for i in range(11):
        p = S.u32(rom, 0x02FEA0 + 4 * i)
        add(p, 'verb_fail', 'default failure of verb %d (\\x01 = object name)' % (i + 1))
    for addr, kind in ((0x02FFDB, 'pronoun'), (0x02FFE0, 'pronoun'), (0x02FFE4, 'pronoun'),
                       (0x02FFE8, 'status_with'), (0x02FFEF, 'status_to'), (0x02FFF4, 'blank_line')):
        add(addr, kind, 'shared UI string')
    add(0x02FE50, 'object_name', 'object id 1 (Shaggy)')
    add(0x02FE57, 'object_name', 'object id 2 (Scooby)')
    lim = rom.index(b'Limbo\0', 0x8A90)
    add(lim, 'ui_string', 'room-select/debug menu: name of room 0 (nowhere)')
    for addr, ctx in sorted(code_string_refs(rom).items()):
        add(addr, 'ui_string', ctx)

    lines = []
    for addr in sorted(entries):
        kind, ctx, refs = entries[addr]
        lines.append('%06X  %-12s %-52s refs=%-3d "%s"' % (addr, kind, ctx, refs, esc(cstr_at(rom, addr))))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    kinds = {}
    for k, _c, _r in entries.values():
        kinds[k] = kinds.get(k, 0) + 1
    with open(out, 'w') as f:
        f.write('; Scooby-Doo Mystery strings with ROM addresses. ROM-derived: do not commit.\n')
        f.write('; generated by tools/py/textdump.py (docs/subsystems/text.md)\n')
        f.write('; %d distinct strings: %s\n' % (len(entries), ', '.join('%s %d' % kv for kv in sorted(kinds.items()))))
        f.write('\n'.join(lines) + '\n')
    print('%d strings (%s) -> %s' % (len(entries), ', '.join('%s %d' % kv for kv in sorted(kinds.items())), out))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
