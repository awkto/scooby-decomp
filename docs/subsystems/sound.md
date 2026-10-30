# Subsystem: sound API glue (GitLab #22)

Code: `port/src/game/sound.c`, API header `port/src/game/sound.h`. Background and the Z80 side: `docs/sound.md`.
The Z80 driver itself is **not** decompiled; the long-term plan is Z80 + YM2612 + PSG emulation. Only the 68k
side (the `$1F6D7A-$1F7082` stub and the game-facing wrappers) is C.

## What is C

| Address | C name | What it does | Status |
|---|---|---|---|
| `$1F6D7A` / `$1F6D8E` | `z80_bus_request` / `z80_bus_release` | bus request (spin until granted) / release | C (io.c, #11); ctx-free core `z80_bus_grab()` |
| `$1F6D98` | `z80_load_driver` | `$A11200=$100`, bus request, copy ROM `$1BB94A..$1BD1D0` to Z80 RAM, zero the rest to `$A02000`; bus stays requested | C |
| `$1F6DDE` | `z80_reset_driver` (`z80_reset`) | reset pulse, bus release, release reset: Z80 starts at 0 | C |
| `$1F6EA8` | `sound_init_wrapper` -> `sound_driver_init()` | upload + reset + `$FF $0B` + four 24-bit table pointers | C |
| `$1F6EF8` | `sound_cmd_play` -> `sound_play()` | cmd `$10` id | C |
| `$1F6F16` | `sound_cmd_stop` -> `sound_stop()` | cmd `$12` id (`$FF` = all) | C |
| `$1F6F2A` / `$1F6F3E` | `sound_cmd_pause` / `sound_cmd_unpause` | cmd `$0C` / `$0D` | C |
| `$1F6F52` | `sound_cmd_stop_all` -> `sound_stop_all()` | cmd `$16` | C |
| `$009762` / `$009776` | `sound_play_wrapper` / `sound_stop_wrapper` | `d0.l` = id; preserve d0/a0; flags = TST.L of the id | C |
| `$1F6E0A`..`$1F6E82` | (inlined as `mailbox_txn`) | queue begin/end/put_byte/arg8/arg24 | C, internal |
| `$00940C`.. | `sound_test` | the Sound Test screen (calls `sound_play`/`cmd_stop_all` through 68k) | not ported (UI, other subsystem) |
| `$0048B6` | `script_op_sound` | script op: play / stop id | C (#16), calls `$009762` through `hle_call68k` |
| `$1F6F20`, `$1F6F66`-`$1F7076` (14 wrappers), `$1F7050 sound_get_var`, `$00978A`/`$00979A` | | tempo, channel flags, patch pokes... | **not ported**: never called (the first 14) / only called from the already-C DMA helpers. 68k code remains; no difftest states exist for them |

`sound_get_var` reads `(11,sp)` after pushing SR, which is 2 bytes past its long argument: a latent bug in the
original library, never exercised.

## Mailbox protocol (as implemented)

Ring of 64 bytes at Z80 `$1B40` (68k `$A01B40`), write index at Z80 `$0036` (68k side), read index `$0037`
(Z80 side), both masked `& $3F`. Message = `$FF, cmd, args...`.
One 68k *transaction* = Z80 bus request, read the write index, store bytes, bus release. Stores happen in this
order (difftest compares the ordered Z80-area write log, so the C does the same):

    ring[i]=$FF  ring[i+1]=cmd  idx=i+2   ring[..]=arg0  idx   ring[..]=arg1  idx ...

`sound_init` uses six transactions that carry the plain byte stream `FF 0B | p0(3) | p1(3) | p2(3) | p3(3)`
(pointers little endian). The 68k never checks for a full ring.

## C API (`sound.h`)

`sound_play(id)`, `sound_stop(id)`, `sound_pause()`, `sound_resume()`, `sound_stop_all()`, `music_current()`
(long at `$FF06A6`, the music the game last requested), `sound_driver_init(ptrs)`, `sound_send(cmd, args, n)`
(raw message), and `sound_set_hook(fn)`: a host hook that sees every play/stop/pause/resume/stop_all message
before it is queued and may swallow it (volume control, music replacement by a host-side player, muting).
Only the low byte of `id` is sent (the game passes sign-extended longs).

Not renamed to `sound_init` because GPGX's core exports that symbol; same for `z80_reset`.

C callers (`script.c`, `reset.c`, `sprite.c`) still call `$009762`/`$009776` through `hle_call68k`; that
now dispatches to this C code. Switching them to `sound_play(id)` directly is a mechanical follow-up (set the
flags with `cc_logic32` and keep d0).

## Register / flag contract of the replaced entry points

The originals save SR around the queue operation (so the CCR is preserved) and leave `a0 = $A00036`.
`d0` = the last value put (the whole stack-arg long for play/stop; the command number for pause/resume/stop_all;
`p3 >> 16` after `sound_init`). `z80_load_driver` leaves `a0 = $1BD1D0`, `d0 = $FFFF`; `z80_reset` leaves `d0 = 0`.

## Verification

- `port/tools/dt <name>` for each function (results in the table at the end of `docs/porting.md`).
- Audible-behaviour gate: `SCOOBY_SNDLOG=file` makes the host log every 68k write into the mailbox ring
  (`port/src/sndlog.c`); `tools/py/sndcmp.py A.log B.log` parses both into `init/play/stop/pause/...` messages and
  compares the sequences. Run on the Sound Test route (`port/scripts/sound_test.txt`) with `--hle all` and `--hle none`.
