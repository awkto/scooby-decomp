/* VDP helpers (VRAM writes through the data port). */
#include "game.h"

/* Set the VDP to VRAM-write at `addr` the way the game does it:
 *   cmd1 = (addr & $3FFF) | $4000 ; cmd2 = addr >> 14 */
static void vram_write_addr(uint16_t addr)
{
	vdp_ctrl_w((uint16_t)((addr & 0x3FFF) | 0x4000));
	vdp_ctrl_w((uint16_t)(addr >> 14));
}

/* $00992C: copy D1.w words from (A0)+ to VRAM address D0.w.
 * D0-D2 preserved, A0 advanced, SR restored but CCR = flags of `lsr.w #6` on addr>>8
 * (the SR is saved after computing the second command word). D1.w = 0 copies 65536 words. */
HLE_FUNC(0x00992C, vram_write_words, 0)
{
	uint16_t addr = (uint16_t)ctx->d[0];
	uint32_t n = (uint16_t)(ctx->d[1] - 1) + 1u;
	ctx->cycles = 140 + 30 * (int)n;
	lsr16(ctx, addr >> 8, 6);
	vram_write_addr(addr);
	for (uint32_t i = 0; i < n; i++) {
		vdp_data_w(bus_r16(ctx->a[0]));
		ctx->a[0] += 2;
	}
}

/* $009964: write the word D1.w to VRAM address D0.w. All registers preserved;
 * CCR as in vram_write_words. */
HLE_FUNC(0x009964, vram_write_word, 60)
{
	uint16_t addr = (uint16_t)ctx->d[0];
	lsr16(ctx, addr >> 8, 6);
	vram_write_addr(addr);
	vdp_data_w((uint16_t)ctx->d[1]);
}

/* $009BE4: fill D1.w words of VRAM at D0.w with D2.w (D1.w = 0 -> 65536 words).
 * D0/D1/D3 preserved; the SR is saved right after `ori.w #$4000,d0`, so the final CCR is
 * N=Z=V=C=0 and X = borrow of the earlier `subi.w #1,d1` (found by difftest). */
HLE_FUNC(0x009BE4, vram_fill_words, 0)
{
	uint16_t addr = (uint16_t)ctx->d[0];
	uint32_t n = (uint16_t)(ctx->d[1] - 1) + 1u;
	ctx->cycles = 140 + 26 * (int)n;
	/* subi.w #1,d1 sets X (borrow when D1.w was 0); andi/ori leave X alone */
	cc_set(ctx, CC_X, (uint16_t)ctx->d[1] == 0 ? CC_X : 0);
	cc_logic16(ctx, (addr & 0x3FFF) | 0x4000);
	vram_write_addr(addr);
	for (uint32_t i = 0; i < n; i++)
		vdp_data_w((uint16_t)ctx->d[2]);
}
