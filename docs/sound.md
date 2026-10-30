# Sound path (Z80 driver, 68k stub and call API)

Status: derived by static analysis of the ROM (68k disassembly, a new Z80
disassembler, `docs` cross-checks).  Nothing here was observed on a running
Z80 (the harness only dumps 68k work RAM); statements about Z80 behaviour are
read from the driver code and marked "(read from code)" where I did not trace
a full path.

## Layout

| What | ROM address | Notes |
|---|---|---|
| 68k Z80-control + mailbox stub | `$1F6D7A-$1F6F66` (+ `$1F6F66-$1F7082`, more wrappers, never called by the game) | hand-written asm, 68k code region extended to `$1F7082` in `tools/py/disasm.py` |
| Sound Test names (NUL separated) | `$1F7082-$1F7492` | 87 strings |
| Sound Test id table | `$1F7492..` | one byte per Sound Test slot = sound id sent to the driver |
| Z80 driver blob | `$1BB94A-$1BD1D0` (`$1886` bytes) | copied to Z80 RAM `$0000`; remainder of Z80 RAM zeroed up to `$2000` |
| Driver data table 0 | `$1BD1D0` | word offsets + FM patch records (`$27` bytes each, copied to Z80 `$1886+`) |
| Driver data table 1 | `$1BE31F` | word table (purpose not traced) |
| Song/sound index + score data | `$1BE496-$1C70C5` | word table indexed by sound id, then per-sound track lists + score |
| Sample/PCM data (table 3) | `$1C70C5-$200000` area (233275 bytes to end of ROM) | sample data for the DAC feeder; format not traced |

The four data pointers are passed to the Z80 at boot (cmd `$0B`) and stored
at Z80 `$0AA5`, `$0AA8`, `$0AAB`, `$0AAE` (24-bit little endian each).  The
boot call is at `$0008CE`:

```
move.l #$1c70c5,-(sp) ; move.l #$1be496,-(sp) ; move.l #$1be31f,-(sp) ; move.l #$1bd1d0,-(sp)
jsr loc_1F6EA8        ; sound_init(p0=$1bd1d0, p1=$1be31f, p2=$1be496, p3=$1c70c5)
```

The Z80 reads ROM through the 68k bank window (routine `rom_read` at Z80
`$01F9`, 24-bit address in A:HL).

## 68k side (`$1F6D7A-$1F7082`)

All routines use C-style stack args (longs, `8(a6)` first arg; callers pop
them) except where noted.  Labels are the generated `loc_XXXXXX` names in
`src/scooby.s`.

Z80 / bus primitives:

| Address | Function |
|---|---|
| `$1F6D7A` | `z80_bus_request`: `$A11100=$100`, spin until granted (bit0 clear). Also called by the pad reader (`$9D34`) so the Z80 is halted during port reads. |
| `$1F6D8E` | `z80_bus_release`: `$A11100=0`. |
| `$1F6D98` | `z80_load_driver`: IRQs masked, `$A11200=$100`, bus request, copy `$1BB94A..$1BD1D0` to `$A00000`, zero-fill to `$A02000`. |
| `$1F6DDE` | `z80_reset`: assert reset (`$A11200=0`), short delay, release bus, release reset (`$A11200=$100`): Z80 starts at `$0000`. |
| `$1F6EA8` | `sound_init(p0,p1,p2,p3)`: load driver, reset, then enqueue `$FF,$0B` followed by the four 24-bit pointers (3 bytes each, little endian). |

Mailbox protocol (68k writes, Z80 consumes):

* Ring buffer: 64 bytes at Z80 `$1B40` (68k `$A01B40`), write index byte at
  Z80 `$0036` (68k `$A00036`, owned by the 68k), read index at Z80 `$0037`
  (owned by the Z80).  Indices wrap with `& $3F`.  Empty when equal.
* A message is `$FF`, command byte, then parameter bytes.  The Z80 main loop
  (`main_loop` -> dispatch at `$0959`) ignores any byte that is not `$FF`
  when idle, so surplus parameter bytes are harmless.  Parameters equal to
  `$FF` would be misparsed only if the receiver reads fewer bytes than sent.
* Every enqueue does: `$1F6E0A` begin (save SR, IRQs off, bus request, `a0` =
  write-index addr, `a1` = ring base, `d1` = write index), `$1F6E52` put
  `$FF` + `d0`, `$1F6E5E` put `d0`, `$1F6E40` end (bus release, restore).
  `$1F6E6C` enqueues one byte arg, `$1F6E82` a 24-bit arg.  No full-buffer
  check is performed by the 68k.
* `$1F7050` `sound_get_var(n)`: bus request, returns byte at Z80 `$1B22+n`.

Command wrappers (arg list = bytes sent after the command byte):

| 68k entry | Cmd | Args | Used by game | Z80 handler (read from code) |
|---|---|---|---|---|
| `$1F6EF8` | `$10` | id | yes, via `$009762` (`sound_play`) | `play_sound`: look up id in the index (table 2), allocate channel records, start score |
| `$1F6F16` | `$12` | id | yes, via `$009776` (`sound_stop`) | `stop_sound` (id `$FF` = all) |
| `$1F6F20` | `$05` | v | no | `set_tempo`-like: `v` scaled by `$DA` into `$08BE` |
| `$1F6F2A` | `$0C` | - | yes (in-game pause, ~`$0033A0`) | clear bit 4 on all channels, silence |
| `$1F6F3E` | `$0D` | - | yes (unpause) | set bit 4 on non-sticky active channels |
| `$1F6F52` | `$16` | - | yes (8x: stop everything on screen changes) | zero all channel records, silence |
| `$1F6F66` | `$1C` | ch | no | set "sticky/exempt from pause" flag (bit 5 of channel flags) |
| `$1F6F70` | `$1D` | ch | no | clear that flag |
| `$1F6F7A` | `$02` | a,b | no | reload FM patch for a channel (b ignored) |
| `$1F6FA2` | `$00` | a,b | no | set patch/pitch for channel |
| `$1F6FAC` | `$01` | a,b | no | key-off |
| `$1F6FB6` | `$14` | a,b | no | store byte in channel record `+28` |
| `$1F6FC0` | `$05` | a,b,(b>>8) | no | cmd `$05` again (3 bytes sent, 1 consumed) |
| `$1F6FF0` | `$06` | a,b | no | channel volume (`+29`) |
| `$1F6FFA` | `$07` | a,b | no | mute (bit 6) |
| `$1F7006` | `$0E` | a,b | no | flag bit 7 |
| `$1F7012` | `$17` | a,b,c | no | match sounds by id, pause/resume |
| `$1F7044` | `$1B` | a,b | no | poke driver variable `$1B22+a = b` |
| `$1F7076` | `$1A` | a,b | no | copy a patch byte |

Only `sound_init`, `$1F6EF8`, `$1F6F16`, `$1F6F2A`, `$1F6F3E`, `$1F6F52`
and the bus request/release are referenced from game code (checked by
grepping `src/scooby.s`); the rest are wrappers the original engine library
had but this game never calls.  Their Z80-side meaning above is from reading
the handlers, not from observation.

Game-facing helpers in the main code:

* `$009762` `sound_play(d0.l = id)` and `$009776` `sound_stop(d0.l = id)`
  push `d0` and call the stubs, preserving `d0/a0`.
* `$00978A` / `$00979A`: bus request/release wrappers (preserve `d0/a0`).
* `$FF06A6` (long) holds the id of the "current music"; it is set to `$2F`
  (the Scooby Doo Theme) at boot and replayed after the Sound Test exits
  (`$009668`).

## Z80 driver (`src/z80/driver.s`)

Generated by `tools/py/z80dis.py` (config: `tools/py/z80dis_cfg.py`) and
verified byte-exact: `make z80check` (needs `z80asm`, `sudo apt-get install
z80asm`).  `make z80disasm` regenerates it.  2546 instructions decoded by
recursive descent (5714 of 6278 bytes are code; the rest are tables and
variables emitted as `defb`/`defw`).  Notable points:

* Reset: `di; im 1; ld sp,$1B20; jp driver_start ($08C3)`.  The `$38`
  interrupt handler only saves SP and `reti`s; all work is polled from
  `main_loop` (`$08F7`).
* The main loop runs, in order: `tick_timer`, `psg_envelope_tick` when the
  timer variable at `$0040` fires, the FM/PSG/track updaters, then drains the
  mailbox and dispatches the command (compare chain at `$0959`).
* **Self-modifying DAC feeder.**  `dac_poll` (`$02B7`) is called from nearly
  everywhere.  It starts as `jr dac_poll_install`; that code patches `$02B7`
  with `exx; ex af,af'` so execution falls into the sample-write code at
  `$02B9` (YM2612 DAC register `$2A`), or with `ret` while idle.  The
  disassembler therefore needs the extra entry points `$02B9/$02EC/$0303`
  from the config file; the generated listing shows the bytes in their
  initial (unpatched) state, exactly as in the ROM.
* A few jump targets land inside the operand of an instruction (e.g.
  `jp m,$0DAF` into the `ld a,$80` at `$0DAE`); those instructions are
  emitted as per-byte `defb` with a comment holding the mnemonic so the
  output stays byte-exact.
* Z80 RAM map: `$0000-$1885` blob, `$1886-$1AF5` 16 FM patch slots x `$27`,
  `$1B20` stack top, `$1B22..` driver variables, `$1B40-$1B7F` ring buffer,
  `$1B80-$1D7F` 16 channel records x `$20` (flags at `+6`: bit0 active,
  bit1 muted-by-17, bit4 running, bit5 sticky, bit6/7 flags; `+3..+5` 24-bit ROM
  score pointer, `+14` sound id, `+15` track number), `$1D80..` per-channel
  scratch x `$10`.
* Command handlers: `cmd_00`..`cmd_20` labels in the listing.  Commands
  implemented: `00 01 02 03 04 05 06 07 0B 0C 0D 0E 10 12 14 16 17 1A 1B 1C 1D 1E 1F 20`.
* Sound `id` -> score: `play_sound` reads the word at `table2 + id*2`
  (so ids are `$00-$7F`), then `$21` bytes at `table2 + that`: byte 0 = number
  of tracks, then 16-bit offsets of each track; each track claims a free
  channel record whose `+3..+5` is set to the track's ROM address.

## Sound IDs

Source: the Sound Test screen (code at `$00940C-$0096F0`).  Cursor position
`(row*16 + col)` selects a name from `$1F7082` and the id byte from `$1F7492`;
B plays the id with `sound_play`, A stops (`sound_stop`-equivalent cmd `$16`), C
exits and replays `$FF06A6`.  There are 87 slots (rows 0-5, last row 0-6).
The table below was **read from ROM**, not captured from a running Sound Test
(I did not drive the emulator; swap on the shared host was over the limit).
The "music / SFX" column is my classification from the names.

Ids that are not in the Sound Test (e.g. `$02`, `$08`, `$0A`, `$0C`, `$10`)
may still be valid (the game uses `$00 $03 $05 $0B $1E $27 $28 $2F $32 $59
$60 $63 $71 $72 $75 $79 $7B $7C` from direct `sound_play` calls; all of
those appear in the table except `$32`; the extraction of that list from
the call sites was a quick pattern scan, so treat it as indicative).

| Slot | Sound id | Name (as shown) | Kind |
|---|---|---|---|
| $00 | $00 | Scooby Slurp | SFX / voice |
| $01 | $01 | Gulp | SFX / voice |
| $02 | $03 | Scooby Laugh | SFX / voice |
| $03 | $04 | Teeth Chatter | SFX / voice |
| $04 | $05 | RUH-ROW! | SFX / voice |
| $05 | $06 | RELP! | SFX / voice |
| $06 | $07 | Scooby Growling | SFX / voice |
| $07 | $09 | Running | SFX / voice |
| $08 | $0B | Bats | SFX / voice |
| $09 | $0D | Termites | SFX / voice |
| $0A | $0F | Stomach Rumble | SFX / voice |
| $0B | $13 | GANGWAY! | SFX / voice |
| $0C | $14 | Shocked | SFX / voice |
| $0D | $15 | Scared | SFX / voice |
| $0E | $19 | HAALP! | SFX / voice |
| $0F | $1B | Hit 1 | SFX / voice |
| $10 | $1C | Hit 2 | SFX / voice |
| $11 | $1D | SCOOBY-DOOBY-DOO! | SFX / voice |
| $12 | $1E | Cruising Mystery Machine | music (guess from name) |
| $13 | $1F | Skid | SFX / voice |
| $14 | $22 | Zap! | SFX / voice |
| $15 | $24 | Zoinks! | SFX / voice |
| $16 | $25 | Get 1 | SFX / voice |
| $17 | $27 | Get 2 | SFX / voice |
| $18 | $28 | Drop 1 | SFX / voice |
| $19 | $2A | Drop 2 | SFX / voice |
| $1A | $2B | Scooby YEOWW! | SFX / voice |
| $1B | $2C | Push | SFX / voice |
| $1C | $2E | Pull | SFX / voice |
| $1D | $2F | Scooby Doo Theme | music (guess from name) |
| $1E | $31 | Ha Ha Carnival Victory | music (guess from name) |
| $1F | $33 | Bungee Down | SFX / voice |
| $20 | $34 | Bungee Up | SFX / voice |
| $21 | $37 | Wooden Door Closed | SFX / voice |
| $22 | $38 | Metal Door Closed | SFX / voice |
| $23 | $3A | Small Close | SFX / voice |
| $24 | $3C | Fridge Close | SFX / voice |
| $25 | $3D | Microwave Close | SFX / voice |
| $26 | $3E | Wooden Door Open | SFX / voice |
| $27 | $41 | Small Door Open | SFX / voice |
| $28 | $43 | Fridge Open | SFX / voice |
| $29 | $44 | Microwave Open | SFX / voice |
| $2A | $45 | Microwave Hum | SFX / voice |
| $2B | $46 | Stone Wall Moves | SFX / voice |
| $2C | $48 | Small Object | SFX / voice |
| $2D | $4A | Throw Large | SFX / voice |
| $2E | $4B | Throw Small | SFX / voice |
| $2F | $4D | Running Water | SFX / voice |
| $30 | $4E | Unscrew 1 | SFX / voice |
| $31 | $4F | Unscrew 2 | SFX / voice |
| $32 | $51 | Totem Falls | SFX / voice |
| $33 | $52 | Totem Hits | SFX / voice |
| $34 | $53 | Cow Bell | SFX / voice |
| $35 | $55 | Snore | SFX / voice |
| $36 | $57 | Engine Sputter | SFX / voice |
| $37 | $58 | Engine Run | SFX / voice |
| $38 | $59 | Clinking Coins | SFX / voice |
| $39 | $5A | Weed Killer Spray | SFX / voice |
| $3A | $5B | Spring | SFX / voice |
| $3B | $5C | The Chief! | SFX / voice |
| $3C | $5D | Blakes: The Mines | music (guess from name) |
| $3D | $5E | Ding | SFX / voice |
| $3E | $5F | Hammer Bell | SFX / voice |
| $3F | $60 | Bumper Thud | SFX / voice |
| $40 | $61 | Crane Run | SFX / voice |
| $41 | $62 | Crane Lift | SFX / voice |
| $42 | $63 | Crane Grab | SFX / voice |
| $43 | $64 | Blakes Theme | music (guess from name) |
| $44 | $66 | Bumper Tune | music (guess from name) |
| $45 | $67 | Balloon Pop | SFX / voice |
| $46 | $68 | Zelda Machine | music (guess from name) |
| $47 | $69 | Blakes Chase Tune | music (guess from name) |
| $48 | $6B | Zelda Breaks | SFX / voice |
| $49 | $6C | Roller Coaster | music (guess from name) |
| $4A | $6F | Off Track | SFX / voice |
| $4B | $71 | Funhouse Theme | music (guess from name) |
| $4C | $72 | Bumper Cars 2 | music (guess from name) |
| $4D | $73 | Spinning | SFX / voice |
| $4E | $74 | Tiger Roars | SFX / voice |
| $4F | $75 | Tiger Swipe | SFX / voice |
| $50 | $77 | Ha Ha Carnival Theme | music (guess from name) |
| $51 | $78 | Mumble | SFX / voice |
| $52 | $79 | Evil Clown 1 | SFX / voice |
| $53 | $7A | Evil Clown 2 | SFX / voice |
| $54 | $7B | Ha Ha Carnival Chase | music (guess from name) |
| $55 | $7C | Scooby 2nd Theme | music (guess from name) |
| $56 | $7F | Blakes Victory | music (guess from name) |
