/* Controller and Z80 bus handling. */
#include "game.h"

/* ctx-free core of z80_bus_request: returns the last $A11100 byte read (bit 0 clear = granted) */
uint8_t z80_bus_grab(void)
{
	bus_w16(0xA11100, 0x0100);
	uint8_t v;
	long guard = 0;
	do {
		v = bus_r8(0xA11100);
	} while ((v & 1) && ++guard < 1000000);   /* the 68k would spin forever */
	return v;
}

/* $1F6D7A (sound stub): request the Z80 bus and wait until it is granted.
 *   move.w #$100,($A11100).l
 * - btst.b #0,($A11100).l / bne.s -
 *   rts */
HLE_FUNC(0x1F6D7A, z80_bus_request, 40)
{
	cc_logic16(ctx, 0x0100);
	uint8_t v = z80_bus_grab();
	cc_set(ctx, CC_Z, (v & 1) ? 0 : CC_Z);
}

/* $1F6D8E: release the Z80 bus.  move.w #0,($A11100).l / rts */
HLE_FUNC(0x1F6D8E, z80_bus_release, 24)
{
	bus_w16(0xA11100, 0x0000);
	cc_logic16(ctx, 0);
}

/* Read one 3-button pad at I/O data port `data` (ctrl port = data + 6):
 *   move.b #$40,(ctrl)         TH as output
 *   move.b #$00,(data)         TH=0
 *   move.b (data),d0           ..SA..DU
 *   lsl.b #2,d0 / andi.b #$C0,d0
 *   move.b #$40,(data)         TH=1
 *   move.b (data),d1           ..CBRLDU
 *   andi.w #$3F,d1 / or.b d1,d0
 *   move.b d0,(dest)           -> SACBRLDU, active low */
static void read_port(hle_ctx *ctx, uint32_t data, uint32_t dest)
{
	bus_w8(data + 6, 0x40);
	bus_w8(data, 0x00);
	set_b(&ctx->d[0], bus_r8(data));
	set_b(&ctx->d[0], lsl8(ctx, ctx->d[0], 2));
	set_b(&ctx->d[0], ctx->d[0] & 0xC0);
	cc_logic8(ctx, ctx->d[0]);
	bus_w8(data, 0x40);
	set_b(&ctx->d[1], bus_r8(data));
	set_w(&ctx->d[1], ctx->d[1] & 0x3F);
	set_b(&ctx->d[0], ctx->d[0] | ctx->d[1]);
	RAM8(dest) = (uint8_t)ctx->d[0];
	cc_logic8(ctx, ctx->d[0]);
}

/* Accumulate changed bits:  move.b (latch),d0 / move.b (cur),d1 / eor.w d1,d0 / or.b d0,(edge) */
static void accumulate_edges(hle_ctx *ctx, uint32_t latch, uint32_t cur, uint32_t edge)
{
	set_b(&ctx->d[0], RAM8(latch));
	set_b(&ctx->d[1], RAM8(cur));
	set_w(&ctx->d[0], ctx->d[0] ^ ctx->d[1]);
	RAM8(edge) |= (uint8_t)ctx->d[0];
	cc_logic8(ctx, RAM8(edge));
}

/* $009D34: read both controllers once per frame.
 * PADn_PREV <- PADn_CUR; PADn_CUR <- port n (SACBRLDU, active low);
 * PADn_EDGE |= PADn_LATCH ^ PADn_CUR. Z80 bus is held while the ports are read. */
HLE_FUNC(0x009D34, pad_read, 260)
{
	z80_bus_request(ctx);
	RAM16(PAD1_PREV) = RAM16(PAD1_CUR);
	read_port(ctx, 0xA10003, PAD1_CUR);
	read_port(ctx, 0xA10005, PAD2_CUR);
	accumulate_edges(ctx, PAD1_LATCH, PAD1_CUR, PAD1_EDGE);
	accumulate_edges(ctx, PAD2_LATCH, PAD2_CUR, PAD2_EDGE);
	z80_bus_release(ctx);
}
