# Hand-derived knowledge about the Z80 blob (see docs/sound.md).

# Extra code entry points not reachable by static control flow.  The driver
# self-modifies: dac_poll starts life as `jr dac_poll_install`; that routine
# patches bytes 02b7/02b8 to `exx; ex af,af'` so execution falls through into
# the sample feeder at 02b9 (or patches 02b7 to `ret` while it is idle).
ENTRIES = [0x02b9, 0x02ec, 0x0303]

DATA = []

# (start, end, None): 16-bit little-endian word tables
WORD_TABLES = [(0x1168, 0x1206, None)]

# Names are best-effort from reading the code; "cmd_XX" are the handlers
# of mailbox command $XX.
NAMES = {
    0x02b7: 'dac_poll',          # self-modifying stub, called everywhere
    0x02df: 'dac_poll_install',
    0x0302: 'nop_hook',          # `ret`, patched at runtime
    0x0041: 'tick_timer',
    0x0066: 'psg_envelope_tick',
    0x01d1: 'cmd_read_byte',     # blocking read from the 64-byte mailbox ring
    0x01f9: 'rom_read',          # A:HL = 24-bit ROM addr, C = len, DE = dest
    0x023d: 'rom_read_window',
    0x08a5: 'cmd_get_channel',   # reads byte n, IX = 0x1b80 + n*0x20
    0x08c3: 'driver_start',
    0x08f7: 'main_loop',
    0x0806: 'fm_channels_update',
    0x0828: 'fm_channel_update',
    0x0458: 'music_tracks_update',
    0x0ec8: 'psg_update',
    0x0fc6: 'fm_volume_update',
    0x0f56: 'set_fm_volume',
    0x0e1b: 'set_tempo',
    0x0e2d: 'psg_assign',
    0x0bdf: 'play_sound',        # cmd $10: A = sound id
    0x0c9e: 'stop_sound',        # cmd $12: A = sound id (0xff = all)
    0x0cf9: 'silence_all',
    0x0d42: 'fm_key_off_channel',
    0x0b89: 'resume_all',
    0x1151: 'patch_slot_addr',   # HL = 0x1886 + A*0x27
    0x1159: 'mul_a_de',
    0x1206: 'fm_load_patch',
    0x169e: 'cmd_key_off',
    0x1728: 'cmd_load_patch',
    0x1766: 'cmd_reload_patches',
    0x1737: 'load_patch_from_rom',
    0x17de: 'find_psg_voice',
    0x1858: 'channel_lookup',
    0x09d6: 'cmd_00',
    0x09ea: 'cmd_01',
    0x09f8: 'cmd_02',
    0x09fe: 'cmd_03',
    0x0a07: 'cmd_04',
    0x0a4b: 'cmd_05',
    0x0a54: 'cmd_06',
    0x0a6d: 'cmd_07_mute',
    0x0a96: 'cmd_0b_set_pointers',
    0x0ab1: 'cmd_0c_pause',
    0x0ac9: 'cmd_0d_resume',
    0x0acf: 'cmd_0e',
    0x0a84: 'cmd_10_play',
    0x0a8d: 'cmd_12_stop',
    0x0ae6: 'cmd_14',
    0x0af2: 'cmd_16_stop_all',
    0x0b12: 'cmd_17',
    0x0b4f: 'cmd_1a',
    0x0b64: 'cmd_1b_poke_var',
    0x0b75: 'cmd_1c_set_sticky',
    0x0b7f: 'cmd_1d_clr_sticky',
    0x0a10: 'cmd_1e',
    0x0ba8: 'cmd_1f',
    0x0bb4: 'cmd_20',
}

COMMENTS = {
    0x0000: ('Z80 reset vector.  The 68k copies this blob to Z80 RAM $0000 (see\n'
             'docs/sound.md).  Z80 RAM map: $0000-$1885 this code+data,\n'
             '$1886-$1AF5 16 FM patch slots x $27, $1B20 stack top,\n'
             '$1B22.. driver variables (68k pokes via cmd $1B, reads $A01B22+n),\n'
             '$1B40-$1B7F 64-byte command ring buffer, $1B80-$1D7F 16 channel\n'
             'records x $20, $1D80.. per-channel scratch x $10.'),
    0x0036: ('$0036 = ring buffer WRITE index (68k, byte at $A00036)\n'
             '$0037 = ring buffer READ index (Z80)'),
    0x0038: 'IM 1 interrupt: just saves SP and returns; the main loop polls instead',
    0x0959: 'command dispatch: after an $FF marker the next byte selects the handler',
    0x0eab: 'voice table terminated by a byte with bit 7 set (see psg_update)',
    0x1168: ('Note frequency tables (words): 13 YM2612 F-numbers ($0284..$0508,\n'
             'one octave + 1) followed by descending PSG tone periods ($03f9..$001c)'),
    0x1791: 'per-voice records (7 bytes each, $FF terminated) used by the FM/PSG voice allocator',
}
