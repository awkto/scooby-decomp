#!/usr/bin/env python3
"""Disassembler for the adventure scripts of Scooby-Doo Mystery (GitLab #19).

The byte code is described in docs/script.md. Scripts live in ROM, in the two mystery
("world") data blocks. Every script is found from the mystery header:

  header+$30            start-up scripts: [room][music][len] body1, then [len] body2
  header+$08 rooms      per room (0x14 bytes): +$0C enter script, +$10 after-fade script
                        (offsets from header+$2C): [len][w] body
  header+$28 objects    8 bytes per object: script offset (from header+$2C), name offset
                        (from header+$20). Script: [len][pronoun flags][w] body
  header+$20            text base (NUL-terminated ASCII), used by ops $02 and $10

Usage (repo root, ROM at rom/baserom.md):
  scriptdis.py [--out build/scripts]       disassemble everything: one file per mystery
                                           and kind, plus build/scripts/op_starts.txt
  scriptdis.py --range ADDR END            disassemble one byte range (hex addresses)
  scriptdis.py --check TRACE...            check a harness trace: every script op the
                                           dispatcher executed must be an op start of the
                                           disassembly ("R <frame> <pc> <addr> <w>" lines
                                           with pc = $0023E2 or $002420)
Output is ROM-derived: keep it under build/ (gitignored).
"""
import os
import struct
import sys

ROM_PATH = os.path.join(os.path.dirname(__file__), '..', '..', 'rom', 'baserom.md')
WORLD_TABLE = 0x031AF2
DISPATCH_PCS = {0x0023E2: 'script_run', 0x002420: 'script_run_frame'}

VERBS = {1: 'PULL', 2: 'TAKE', 3: 'OPEN', 4: 'SHUT', 5: 'USE', 6: 'GIVE', 7: 'PUSH',
         8: 'TALK', 9: 'EAT', 10: 'LOOK', 11: 'WALK'}

# object record fields (ram_objects $FF1200, 26 bytes; docs/script.md)
OBJ_FIELDS = {0: 'state', 2: 'shape', 4: 'icon', 6: 'room', 8: 'dir', 0x0A: 'timer',
              0x0C: 'var', 0x12: 'x', 0x14: 'y', 0x16: 'verb', 0x18: 'flags'}

OPS = {
    0x00: 'nop', 0x01: 'case_verb', 0x02: 'talk_choice', 0x03: 'if', 0x04: 'if_else',
    0x05: 'move_obj', 0x06: 'anim_wait', 0x07: 'show_obj', 0x08: 'set', 0x09: 'set_verb',
    0x0A: 'set_icon', 0x0B: 'set_state', 0x0C: 'sys', 0x0D: 'sound', 0x0E: 'walk_spawn',
    0x0F: 'walk_obj', 0x10: 'say', 0x11: 'goto_room', 0x12: 'set_shape', 0x13: 'on_event',
    0x14: 'set_shape2', 0x15: 'wait', 0x16: 'freeze', 0x17: 'wait_anim', 0x18: 'goto_room_cut',
    0x19: 'unfreeze_wait', 0x1A: 'pal_cycle', 0x1B: 'walk_xy', 0x1C: 'test_moving',
    0x1D: 'test_anim_done', 0x1E: 'face', 0x1F: 'wait_stop', 0x20: 'stop_path',
    0x21: 'path',
}

SUBOPS = {
    0x00: 'nop', 0x01: 'end', 0x02: 'room_fade_in', 0x03: 'fade_out', 0x04: 'wait_button',
    0x05: 'fullscreen_picture', 0x06: 'bar_show_if_hidden', 0x07: 'closeup_picture',
    0x08: 'closeup_off', 0x09: 'scooby_away', 0x0A: 'scooby_back', 0x0B: 'scooby_pose_on',
    0x0C: 'bar_hide_once', 0x0D: 'flag09e8_1_on', 0x0E: 'flag09e8_1_off',
    0x0F: 'camera_pan_right', 0x10: 'camera_pan_left', 0x11: 'flag09e8_2_on',
    0x12: 'flag09e8_2_off', 0x13: 'sfx_random_a', 0x14: 'sfx_random_b',
    0x15: 'follow_mode_on', 0x16: 'follow_mode_off', 0x17: 'overlay_a_on',
    0x18: 'overlay_b_on', 0x19: 'overlay_off', 0x1A: 'pal_flash_on', 0x1B: 'pal_flash_off',
    0x1C: 'bar_show', 0x1D: 'bar_hide', 0x1E: 'ending_credits', 0x1F: 'flag09e9_0_on',
    0x20: 'flag09e9_0_off', 0x21: 'wait_dpad_choice', 0x22: 'shaggy_anim_by_x',
    0x23: 'flag09e9_1_on', 0x24: 'flag09e9_1_off', 0x25: 'wait_shaggy_path',
    0x26: 'test_shaggy_path', 0x27: 'minigame_throw', 0x28: 'talk_end',
    0x29: 'flag09e9_3_on', 0x2A: 'flag09e9_3_off', 0x2B: 'shaggy_alt_sprite_on',
    0x2C: 'shaggy_alt_sprite_off', 0x2D: 'flag09e9_6_on', 0x2E: 'flag09e9_6_off',
    0x2F: 'then_talk', 0x30: 'no_idle_anim_on', 0x31: 'no_idle_anim_off',
    0x32: 'minigame_tug', 0x33: 'flag09eb_0_on', 0x34: 'flag09eb_0_off',
    0x35: 'shaggy_pose_45', 0x36: 'scooby_pose_off', 0x37: 'shaggy_pose_47',
    0x38: 'skippable_off', 0x39: 'skippable_on', 0x3A: 'flag09eb_5_on',
    0x3B: 'flag09eb_5_off', 0x3C: 'flag09eb_6_on', 0x3D: 'flag09eb_6_off',
}


def u16(b, o):
    return struct.unpack_from('>H', b, o)[0]


def s16(b, o):
    return struct.unpack_from('>h', b, o)[0]


def u32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def cstr(b, a, limit=400):
    end = b.index(b'\0', a)
    s = b[a:min(end, a + limit)].decode('latin-1')
    return s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')


class World:
    def __init__(self, rom, idx):
        self.rom, self.idx = rom, idx
        self.addr = h = u32(rom, WORLD_TABLE + 4 * idx)
        f = [u32(rom, h + 4 * i) for i in range(13)]
        (self.state_start, self.state_end, self.rooms, self.tiles_base, _maps, _pals,
         self.icons, self.shapes, self.text, self.shape_ptrs, self.obj_table,
         self.script_base, self.start) = f
        self.nrooms = (self.tiles_base - self.rooms) // 0x14
        self.objinit = u32(rom, h + 4)
        # object init records: 14 bytes, +4 if (flags byte) bit 1 (actor: x, y)
        self.nobj = 0
        a = self.objinit
        while a < self.rooms:
            a += 14 + (4 if rom[a + 12] & 2 else 0)
            self.nobj += 1
        self.obj_names = {}
        for i in range(self.nobj):
            self.obj_names[i + 3] = cstr(rom, self.text + u32(rom, self.obj_table + 8 * i + 4))
        self.room_names = {}
        for r in range(self.nrooms):
            self.room_names[r + 2] = cstr(rom, self.text + u32(rom, self.rooms + 0x14 * r))

    # ---- operand formatting
    def who(self, w):
        w &= 0x7FFF
        if w == 1:
            return 'Shaggy'
        if w == 2:
            return 'Scooby'
        if w == 0:
            return 'none'
        n = self.obj_names.get(w)
        return '#%d"%s"' % (w, n) if n is not None else '#%d?' % w

    def room(self, r):
        if r == 1:
            return 'INVENTORY'
        if r == 0:
            return 'nowhere'
        n = self.room_names.get(r)
        return 'room%d"%s"' % (r, n) if n is not None else 'room%d' % r

    def text_at(self, off):
        a = self.text + off
        if a >= len(self.rom):
            return '<bad text $%X>' % off
        return '"%s"' % cstr(self.rom, a)

    def field(self, f, obj):
        """lhs/rhs of op $03/$04/$08 in 'object field' mode"""
        if obj == 1 and f == 6:
            return 'ROOM'
        fn = OBJ_FIELDS.get(f & ~1, '+%02X' % (f & ~1))
        return '%s.%s' % (self.who(obj), fn)


def flag(byte, bit):
    return 'F%02X.%d' % (byte, bit & 7)


class Dis:
    def __init__(self, w):
        self.w, self.rom = w, w.rom
        self.starts = {}        # op address -> (op, length)
        self.lines = []
        self.errors = []

    def out(self, depth, a, text):
        self.lines.append('%06X  %s%s' % (a, '    ' * depth, text))

    def cond(self, a):
        r, w = self.rom, self.w
        mode = u16(r, a + 0xE)
        w6, w8, wa, wc = u16(r, a + 6), u16(r, a + 8), u16(r, a + 0xA), u16(r, a + 0xC)
        if mode & 1:
            lhs = flag(w6, w8)
        elif mode & 2:
            lhs = w.field(w6, w8)
        else:
            lhs = str(s16(r, a + 6))
        if mode & 4:
            rhs = flag(wa, wc)
        elif mode & 8:
            rhs = w.field(wa, wc)
        else:
            rhs = str(s16(r, a + 0xA))
        op = '==' if mode & 0x10 else '>' if mode & 0x20 else '<' if mode & 0x40 else '!='
        return '%s %s %s' % (lhs, op, rhs)

    def block(self, a, end, depth):
        """Disassemble ops from a to end (exclusive)."""
        r, w = self.rom, self.w
        while a < end:
            if a + 2 > end:
                self.errors.append('%06X: truncated' % a)
                self.out(depth, a, '?? truncated')
                return
            op = u16(r, a)
            name = OPS.get(op)
            if name is None:
                self.errors.append('%06X: bad opcode %04X' % (a, op))
                self.out(depth, a, '?? dc.w $%04X' % op)
                return
            w2, w4 = u16(r, a + 2), u16(r, a + 4)
            nxt = None
            if op == 0x00:
                n = 2
                self.out(depth, a, 'nop')
            elif op == 0x01:
                n = w6 = u16(r, a + 6)
                v = VERBS.get(w2, 'verb%d' % w2)
                txt = 'case %s' % v
                if w4:
                    txt += ' with %s' % w.who(w4)
                self.out(depth, a, txt + ':')
                self.starts[a] = (op, 8)
                self.block(a + 8, a + n, depth + 1)
                a += n
                continue
            elif op == 0x02:
                n = u16(r, a + 0xA)
                t1, t2 = u32(r, a + 2), u32(r, a + 6)
                self.out(depth, a, 'talk_choice %s reply %s pose %d:' % (
                    w.text_at(t1), w.text_at(t2), s16(r, a + 0xC)))
                self.starts[a] = (op, 0xE)
                self.block(a + 0xE, a + n, depth + 1)
                a += n
                continue
            elif op in (0x03, 0x04):
                self.starts[a] = (op, 0x10)
                self.out(depth, a, 'if %s:' % self.cond(a))
                self.block(a + 0x10, a + w2, depth + 1)
                if op == 0x04:
                    self.out(depth, a + w2, 'else:')
                    self.block(a + w2, a + w2 + w4, depth + 1)
                    a += w2 + w4
                else:
                    a += w2
                self.out(depth, a, 'endif')
                continue
            elif op == 0x13:
                self.starts[a] = (op, 4)
                self.out(depth, a, 'on_event:')
                self.block(a + 4, a + w2, depth + 1)
                a += w2
                continue
            elif op == 0x05:
                n = 6
                nw = ' nowait' if w2 & 0x8000 else ''
                self.out(depth, a, 'move_obj %s -> %s%s' % (w.who(w2), w.room(w4), nw))
            elif op == 0x06:
                n = 6
                self.out(depth, a, 'anim_wait %s anim %d' % (w.who(w2), s16(r, a + 4)))
            elif op in (0x07, 0x16, 0x17, 0x19, 0x1C, 0x1D, 0x1F, 0x20):
                n = 4
                self.out(depth, a, '%s %s' % (OPS[op], w.who(w2)))
            elif op == 0x08:
                n = 12
                self.out(depth, a, self.op_set(a))
            elif op == 0x09:
                n = 6
                self.out(depth, a, 'set_verb %s slot %d = %s' % (
                    w.who(w2), r[a + 5] - (256 if r[a + 5] > 127 else 0),
                    VERBS.get(r[a + 4], 'verb%d' % r[a + 4])))
            elif op == 0x0A:
                n = 6
                self.out(depth, a, 'set_icon %s = %d' % (w.who(w2), s16(r, a + 4)))
            elif op == 0x0B:
                n = 6
                self.out(depth, a, 'set_state %s = $%04X%s' % (
                    w.who(w2), w4, ' wait' if w2 & 0x8000 else ''))
            elif op == 0x0C:
                n = 4
                self.out(depth, a, 'sys %s' % SUBOPS.get(w2, 'sub%02X?' % w2))
            elif op == 0x0D:
                n = 4
                if w2 & 0x8000:
                    self.out(depth, a, 'sound_stop $%02X' % (w2 & 0x7FFF))
                else:
                    self.out(depth, a, 'sound $%02X' % w2)
            elif op in (0x0E, 0x1B):
                n = 10 if op == 0x0E else 12
                speed, anim = s16(r, a + 4), s16(r, a + 6)
                if op == 0x0E:
                    tgt = 'spawn %d' % u16(r, a + 8)
                else:
                    tgt = '(%d,%d)' % (s16(r, a + 8), s16(r, a + 10))
                how = 'teleport' if speed < 0 else 'speed %d' % speed
                self.out(depth, a, '%s %s -> %s %s anim %d%s' % (
                    'walk', w.who(w2), tgt, how, anim, ' nowait' if w2 & 0x8000 else ''))
            elif op == 0x0F:
                n = 12
                self.out(depth, a, 'walk_obj %s -> %s dx %d speed %d anim %d%s' % (
                    w.who(w2), w.who(w4), s16(r, a + 8), s16(r, a + 10), s16(r, a + 6),
                    ' nowait' if w2 & 0x8000 else ''))
            elif op == 0x10:
                n = 8
                attr = w2
                bits = ['side %d' % (attr & 0xF), 'pal %d' % ((attr >> 4) & 3)]
                if attr & 0x1000:
                    bits.append('talkanim')
                if attr & 0x2000:
                    bits.append('turn %d' % ((attr >> 14) & 3))
                self.out(depth, a, 'say %s  [%s]' % (w.text_at(u32(r, a + 4)), ', '.join(bits)))
            elif op in (0x11, 0x18):
                n = 8
                self.out(depth, a, '%s %s spawn %d dir %d' % (
                    OPS[op], w.room(w2), w4, s16(r, a + 6)))
            elif op in (0x12, 0x14):
                n = 6
                self.out(depth, a, 'set_shape %s = %d' % (w.who(w2), s16(r, a + 4)))
            elif op == 0x15:
                n = 4
                self.out(depth, a, 'wait %d' % w2)
            elif op == 0x1A:
                n = 10
                if s16(r, a + 8) < 0:
                    self.out(depth, a, 'pal_cycle %d off, reload colours %d..%d' % (
                        w2, w4, u16(r, a + 6)))
                else:
                    self.out(depth, a, 'pal_cycle %d colours %d..%d speed %d' % (
                        w2, w4, u16(r, a + 6), s16(r, a + 8)))
            elif op == 0x1E:
                n = 6
                self.out(depth, a, 'face %s dir %d%s' % (
                    w.who(w2), s16(r, a + 4), ' wait' if w2 & 0x8000 else ''))
            elif op == 0x21:
                n = 8 + w2
                data = r[a + 8:a + 8 + w2]
                self.out(depth, a, 'path %s delay %d data[%d] %s' % (
                    w.who(w4), s16(r, a + 6), w2, data.hex()))
            else:
                raise AssertionError(op)
            self.starts[a] = (op, n)
            a += n
        if a != end:
            self.errors.append('%06X: block overruns its end %06X' % (a, end))
            self.out(depth, a, '?? overrun (block end %06X)' % end)

    def op_set(self, a):
        r, w = self.rom, self.w
        mode = u16(r, a + 0xA)
        w2, w4, w6, w8 = u16(r, a + 2), u16(r, a + 4), u16(r, a + 6), u16(r, a + 8)
        if mode & 4:
            src = flag(w6, w8)
        elif mode & 8:
            src = w.field(w6, w8)
        elif mode & 0x80:
            src = 'random(%d)' % w6
        else:
            src = str(s16(r, a + 6))
        if mode & 1:
            dst = flag(w2, w4)
            if mode & 0x10:
                return 'set %s = %s' % (dst, src)
            # the original's EOR uses a3 instead of d3 as index (bug), see docs/script.md
            return 'toggle %s if %s   ; (buggy: eor at a3+..)' % (dst, src)
        dst = w.field(w2, w4)
        o = '=' if mode & 0x10 else '-=' if mode & 0x20 else '|=' if mode & 0x40 else '+='
        return 'set %s %s %s' % (dst, o, src)


def script_list(w):
    """(label, header_addr, body_start, body_end) for every script of mystery w."""
    r = w.rom
    out = []
    # start-up scripts
    a = w.start
    room, music, ln = u16(r, a), u16(r, a + 2), u16(r, a + 4)
    out.append(('start1 (start room %d, music $%02X)' % (room, music), a, a + 6, a + 6 + ln))
    b = a + 6 + ln
    ln2 = u16(r, b)
    out.append(('start2', b, b + 2, b + 2 + ln2))
    for i in range(w.nrooms):
        e = w.rooms + 0x14 * i
        for k, f in (('enter', 0xC), ('after_fade', 0x10)):
            s = w.script_base + u32(r, e + f)
            ln = u16(r, s)
            out.append(('room %d "%s" %s' % (i + 2, w.room_names[i + 2], k), s, s + 4, s + 4 + ln))
    for i in range(w.nobj):
        s = w.script_base + u32(r, w.obj_table + 8 * i)
        ln = u16(r, s)
        out.append(('object #%d "%s" (pronoun flags %d, w %04X)' % (
            i + 3, w.obj_names[i + 3], u16(r, s + 2), u16(r, s + 4)), s, s + 6, s + 6 + ln))
    return out


def disassemble_all(rom, outdir):
    os.makedirs(outdir, exist_ok=True)
    starts = {}
    errors = []
    for wi in (0, 1):
        w = World(rom, wi)
        groups = {'start': [], 'rooms': [], 'objects': []}
        seen = {}
        for label, hdr, b0, b1 in script_list(w):
            kind = 'start' if label.startswith('start') else 'rooms' if label.startswith('room') else 'objects'
            d = Dis(w)
            d.lines.append('')
            d.lines.append('; ---- %s   header $%06X, body $%06X-$%06X' % (label, hdr, b0, b1))
            if hdr in seen:
                d.lines.append(';      (same script as %s)' % seen[hdr])
            else:
                seen[hdr] = label
                d.block(b0, b1, 0)
            groups[kind] += d.lines
            starts.update(d.starts)
            errors += ['w%d %s: %s' % (wi, label, e) for e in d.errors]
        for kind, lines in groups.items():
            with open(os.path.join(outdir, 'w%d_%s.txt' % (wi, kind)), 'w') as f:
                f.write('; Scooby-Doo Mystery, mystery %d (%s): %s scripts. ROM-derived, do not commit.\n'
                        % (wi, 'Blake\'s Hotel' if wi == 0 else 'Ha Ha Carnival', kind))
                f.write('; generated by tools/py/scriptdis.py; format in docs/script.md\n')
                f.write('\n'.join(lines) + '\n')
        with open(os.path.join(outdir, 'w%d_names.txt' % wi), 'w') as f:
            for k, v in sorted(w.obj_names.items()):
                f.write('object #%d %s\n' % (k, v))
            for k, v in sorted(w.room_names.items()):
                f.write('room %d %s\n' % (k, v))
    with open(os.path.join(outdir, 'op_starts.txt'), 'w') as f:
        for a in sorted(starts):
            op, n = starts[a]
            f.write('%06X %02X %d\n' % (a, op, n))
    return starts, errors


def check_traces(rom, paths):
    starts, errors = disassemble_all(rom, os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'scripts'))
    seen, bad = {}, {}
    for p in paths:
        with open(p) as f:
            for line in f:
                if not line.startswith('R '):
                    continue
                parts = line.split()
                pc, addr = int(parts[2], 16), int(parts[3], 16)
                if pc not in DISPATCH_PCS:
                    continue
                if addr in starts:
                    seen[addr] = seen.get(addr, 0) + 1
                else:
                    bad[addr] = bad.get(addr, 0) + 1
    ops = {}
    for a in seen:
        ops[starts[a][0]] = ops.get(starts[a][0], 0) + 1
    print('dispatched script ops: %d distinct addresses, %d executions; %d NOT op starts'
          % (len(seen), sum(seen.values()), len(bad)))
    print('opcodes seen: ' + ' '.join('%02X(%s)x%d' % (o, OPS[o], ops[o]) for o in sorted(ops)))
    for a in sorted(bad):
        print('  MISMATCH: dispatch at $%06X (%d times) is not an op start' % (a, bad[a]))
    return 1 if bad else 0


def main(argv):
    with open(ROM_PATH, 'rb') as f:
        rom = f.read()
    if len(argv) >= 1 and argv[0] == '--check':
        return check_traces(rom, argv[1:])
    if len(argv) >= 1 and argv[0] == '--range':
        a, e = int(argv[1], 16), int(argv[2], 16)
        wi = 0 if len(argv) < 4 else int(argv[3])
        d = Dis(World(rom, wi))
        d.block(a, e, 0)
        print('\n'.join(d.lines))
        return 0
    outdir = os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'scripts')
    if len(argv) >= 2 and argv[0] == '--out':
        outdir = argv[1]
    starts, errors = disassemble_all(rom, outdir)
    print('%d script ops written to %s' % (len(starts), os.path.normpath(outdir)))
    for e in errors:
        print('ERROR', e)
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
