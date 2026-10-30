/* 68k sound API glue (issue #22; docs/subsystems/sound.md).  The Z80 driver itself is not
 * decompiled: it runs on the emulated Z80 + YM2612 + PSG.  This file is the 68k side:
 * driver upload and reset, the mailbox ring at Z80 RAM $1B40 and the command senders. */
#include "game.h"
#include "sound.h"

#define Z80_RING_WR    0xA00036   /* mailbox write index: owned by the 68k */
#define Z80_RING       0xA01B40   /* 64-byte mailbox ring (read index at $A00037, Z80's) */
#define RING_MASK      0x3F
#define Z80_BUSREQ     0xA11100
#define Z80_RESETREG   0xA11200
#define DRIVER_ROM     0x1BB94A   /* Z80 driver blob in ROM ... */
#define DRIVER_END     0x1BD1D0   /* ... ends where data table 0 starts */
#define Z80_RAM        0xA00000
#define Z80_RAM_END    0xA02000
#define RAM_CUR_MUSIC  0xFF06A6

static sound_hook_fn g_hook;
void sound_set_hook(sound_hook_fn fn) { g_hook = fn; }

uint32_t music_current(void) { return RAM32(RAM_CUR_MUSIC); }

static void bus_drop(void) { bus_w16(Z80_BUSREQ, 0x0000); }

/* ---- mailbox ----
 * One transaction = $1F6E0A (begin) ... $1F6E40 (end): bus request, read the write index,
 * store the bytes (each store of an argument is followed by an index update), bus release.
 * The store order is what difftest compares, so it mirrors the original exactly:
 * `$FF, cmd, index` then `arg, index` per argument byte.  The 68k never checks for a full
 * ring.  `ff_prefix` = false is the plain put_byte path used by sound_init. */
static void mailbox_txn(bool ff_prefix, uint8_t first, const uint8_t *rest, int nrest)
{
	z80_bus_grab();
	uint8_t idx = bus_r8(Z80_RING_WR) & RING_MASK;   /* always < $40 in practice */
	if (ff_prefix) {
		bus_w8(Z80_RING + idx, 0xFF);
		idx = (idx + 1) & RING_MASK;
	}
	bus_w8(Z80_RING + idx, first);
	idx = (idx + 1) & RING_MASK;
	bus_w8(Z80_RING_WR, idx);
	for (int i = 0; i < nrest; i++) {
		bus_w8(Z80_RING + idx, rest[i]);
		idx = (idx + 1) & RING_MASK;
		bus_w8(Z80_RING_WR, idx);
	}
	bus_drop();
}

void sound_send(uint8_t cmd, const uint8_t *args, int nargs)
{
	mailbox_txn(true, cmd, args, nargs);
}

/* command senders: all go through the host hook first */
static void send_cmd(uint8_t cmd, const uint8_t *args, int nargs)
{
	if (g_hook && g_hook(cmd, args, nargs))
		return;
	sound_send(cmd, args, nargs);
}

void sound_play(uint32_t id)    { uint8_t a = (uint8_t)id; send_cmd(SNDCMD_PLAY, &a, 1); }
void sound_stop(uint32_t id)    { uint8_t a = (uint8_t)id; send_cmd(SNDCMD_STOP, &a, 1); }
void sound_pause(void)          { send_cmd(SNDCMD_PAUSE, NULL, 0); }
void sound_resume(void)         { send_cmd(SNDCMD_RESUME, NULL, 0); }
void sound_stop_all(void)       { send_cmd(SNDCMD_STOP_ALL, NULL, 0); }

/* ---- Z80 driver upload and reset ---- */

/* $1F6D98 z80_load_driver body: copy the driver blob ($1886 bytes of ROM) to Z80 RAM $0000
 * and zero the rest of Z80 RAM up to $2000.  Leaves the Z80 bus requested (the reset that
 * follows releases it). */
static void z80_upload_driver(void)
{
	bus_w16(Z80_RESETREG, 0x0100);        /* deassert reset so the bus can be granted */
	z80_bus_grab();
	uint32_t dst = Z80_RAM;
	for (uint32_t src = DRIVER_ROM; src < DRIVER_END; src++)
		bus_w8(dst++, ROM8(src));
	while (dst != Z80_RAM_END)
		bus_w8(dst++, 0);
}

/* $1F6DDE z80_reset body: pulse the Z80 reset line, release the bus, start the driver at
 * Z80 $0000. */
static void z80_restart(void)
{
	bus_w16(Z80_RESETREG, 0x0000);
	bus_w16(Z80_BUSREQ, 0x0000);
	bus_w16(Z80_RESETREG, 0x0100);
}

/* Boot-time init (cmd $0B): upload, reset, then `$FF $0B` + four 24-bit little-endian data
 * pointers.  The original sends them as six separate transactions: $FF, $0B, then one per
 * pointer; the Z80 only sees the byte stream. */
void sound_driver_init(const uint32_t ptr[4])
{
	z80_upload_driver();
	z80_restart();
	uint8_t ff = 0xFF, cmd = SNDCMD_INIT;
	mailbox_txn(false, ff, NULL, 0);
	mailbox_txn(false, cmd, NULL, 0);
	for (int i = 0; i < 4; i++) {
		uint8_t b[2] = { (uint8_t)(ptr[i] >> 8), (uint8_t)(ptr[i] >> 16) };
		mailbox_txn(false, (uint8_t)ptr[i], b, 2);
	}
}

/* ================================================================ HLE wrappers
 * The 68k entry points keep their register contract (C stack args, caller pops).  The
 * originals preserve the CCR (sound_queue_begin/end save and restore SR), leave a0 =
 * $A00036 (the write-index address) and d0 = the last value put through put_byte. */

/* arg n (0-based) of a C-style call at entry: (sp) = return address */
static uint32_t stack_arg(hle_ctx *ctx, int n) { return bus_r32(ctx->a[7] + 4 + 4 * n); }

/* $1F6EF8 sound_cmd_play(id): cmd $10 */
HLE_FUNC(0x1F6EF8, sound_cmd_play, 519)
{
	uint32_t id = stack_arg(ctx, 0);
	sound_play(id);
	ctx->d[0] = id;                 /* move.l 8(a6),d0 / put_byte */
	ctx->a[0] = Z80_RING_WR;
}

/* $1F6F16 sound_cmd_stop(id): cmd $12 */
HLE_FUNC(0x1F6F16, sound_cmd_stop, 528)
{
	uint32_t id = stack_arg(ctx, 0);
	sound_stop(id);
	ctx->d[0] = id;
	ctx->a[0] = Z80_RING_WR;
}

/* $1F6F2A sound_cmd_pause: cmd $0C (in-game pause, Start button) */
HLE_FUNC(0x1F6F2A, sound_cmd_pause, 430)
{
	sound_pause();
	ctx->d[0] = SNDCMD_PAUSE;       /* moveq #$C,d0, left by put_ff_cmd */
	ctx->a[0] = Z80_RING_WR;
}

/* $1F6F3E sound_cmd_unpause: cmd $0D */
HLE_FUNC(0x1F6F3E, sound_cmd_unpause, 430)
{
	sound_resume();
	ctx->d[0] = SNDCMD_RESUME;
	ctx->a[0] = Z80_RING_WR;
}

/* $1F6F52 sound_cmd_stop_all: cmd $16 (screen changes, game start) */
HLE_FUNC(0x1F6F52, sound_cmd_stop_all, 430)
{
	sound_stop_all();
	ctx->d[0] = SNDCMD_STOP_ALL;
	ctx->a[0] = Z80_RING_WR;
}

/* $009762 sound_play(d0.l = id): game-facing wrapper; preserves d0/a0, the final
 * `tst.l (sp)+` leaves the flags of the id long. */
HLE_FUNC(0x009762, sound_play_wrapper, 632)
{
	sound_play(ctx->d[0]);
	cc_logic32(ctx, ctx->d[0]);
}

/* $009776 sound_stop(d0.l = id) */
HLE_FUNC(0x009776, sound_stop_wrapper, 640)
{
	sound_stop(ctx->d[0]);
	cc_logic32(ctx, ctx->d[0]);
}

/* $1F6D98 z80_load_driver: clobbers a0 (= end of the blob, $1BD1D0) and d0 (= $FFFF, the
 * dbra counter); Z80 bus left requested.  The CCR is that of `move.l a1,-(sp)` at entry
 * (flags of a1), which the SR save/restore then preserves. */
HLE_FUNC(0x1F6D98, z80_load_driver, 218400)
{
	z80_upload_driver();
	cc_logic32(ctx, ctx->a[1]);
	ctx->a[0] = DRIVER_END;
	ctx->d[0] = 0xFFFF;             /* the dbra counter (blob size - 1) ended at -1 in the low word */
}

/* $1F6DDE z80_reset: d0 ends 0 (delay counter), CCR preserved */
HLE_FUNC(0x1F6DDE, z80_reset_driver, 406)
{
	z80_restart();
	ctx->d[0] = 0;
}

/* $1F6EA8 sound_driver_init(p0,p1,p2,p3) (stack args): the registers it leaves are the last
 * arg24's: d0 = p3 >> 16 (asr), a0 = $A00036, flags of the pushed p3 long. */
HLE_FUNC(0x1F6EA8, sound_init_wrapper, 221500)
{
	uint32_t p[4];
	for (int i = 0; i < 4; i++)
		p[i] = stack_arg(ctx, i);
	sound_driver_init(p);
	ctx->d[0] = (uint32_t)((int32_t)p[3] >> 16);
	ctx->a[0] = Z80_RING_WR;
	cc_logic32(ctx, p[3]);
}
