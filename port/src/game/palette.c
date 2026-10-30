/* Palette fades (GitLab #13, docs/subsystems/vdp.md). The current CRAM image lives in
 * ram_pal_cur ($FF06F8, 64 words, 0000BBB0GGG0RRR0). Both fades step it 8 times, wait one
 * VBlank per step (`wait_vblanks`, which stays 68k) and DMA the image to CRAM.
 *
 * Because every step ends in a blocking call, each routine is a chain of HLE_RANGEs that
 * stop in front of the `bsr` to the wait (docs/porting.md "Waiting for VBlank"):
 *
 *   palette_read_cram $009DE0  fade OUT: read CRAM into ram_pal_cur, then 8 steps that
 *                              subtract one brightness step (2) from every non-zero colour
 *                              component. D3 counts the steps (7..0).
 *     $009DE0 -> exit $009DEE (wait_vblank)       -> $009DF2 -> exit $009E5C (wait_vblanks)
 *     $009E60 -> exit $009E5C (next step) | $009E7C (rts)
 *   palette_fade      $009E7E  fade IN: A0 = target image (64 words); ram_pal_cur starts
 *                              black and each step moves every component 2 closer.
 *     $009E7E movem (68k)  $009E82 -> exit $009F06 (wait_vblanks)
 *     $009F0A -> exit $009F06 (next step) | $009F2A (movem pop + rts, 68k)
 * The movem push/pop stay in 68k: a range must leave through its exit with the stack
 * pointer it was entered with. */
#include "game.h"
#include "cc68k.h"

#define PAL_CUR 0xFF06F8u
#define PAL_BYTES 0x80
/* original cost of the loops (from difftest) */
#define FADE_OUT_STEP_CYCLES 8000
#define FADE_IN_STEP_CYCLES  14900

/* fade-out step ($009E14..$009E58): every colour &= $EEE, then each non-zero component
 * loses 2. Leaves D0.w = 0, D4.w = $FFFF, A1 = end of the image, CCR = Z (the final
 * `move #$2000,sr` / `move.w #0,d0`); D1.w/D2.w hold the last colour's G / R parts. */
static void fade_out_step(hle_ctx *ctx)
{
	uint32_t a = PAL_CUR;
	uint16_t d1 = 0, d2 = 0;
	for (int i = 0; i < 64; i++, a += 2) {
		uint16_t c = RAM16(a) & 0xEEE;
		uint16_t v = c;
		if (c & 0xF00) v -= 0x200;
		if (c & 0x0F0) v -= 0x020;
		if (c & 0x00F) v -= 0x002;
		RAM16(a) = v;
		d1 = c & 0x0F0;
		d2 = c & 0x00F;
	}
	set_w(&ctx->d[1], d1);
	set_w(&ctx->d[2], d2);
	set_w(&ctx->d[4], 0xFFFF);
	ctx->a[1] = a;
	set_w(&ctx->d[0], 0);
	ctx->sr = 0x2000 | CC_Z;
	ctx->cycles += FADE_OUT_STEP_CYCLES;
}

/* $009DE0: d3 = 7, d0 = $3F, a1 = ram_pal_cur; next the original calls wait_vblank. */
HLE_RANGE(0x009DE0, palette_read_cram, 30, 0x009DEE)
{
	set_w(&ctx->d[3], 7);
	set_w(&ctx->d[0], 0x3F);
	cc_logic16(ctx, 0x3F);
	ctx->a[1] = PAL_CUR;
	hle_jump(ctx, 0x009DEE);
}

/* $009DF2 (after wait_vblank): read the 64 CRAM words, then the first fade step. */
HLE_RANGE(0x009DF2, palette_read_cram_read, 2500, 0x009E5C)
{
	ctx->sr = 0x2700;                          /* move #$2700,sr (also clears the CCR) */
	vdp_ctrl_w(0x0000);                        /* CRAM read command, address 0 */
	vdp_ctrl_w(0x0020);
	uint32_t a = ctx->a[1];
	for (int i = 0; i < 64; i++, a += 2)
		RAM16(a) = vdp_data_r();
	set_w(&ctx->d[0], 0);                      /* dbra d0 ran out */
	ctx->a[1] = a;
	fade_out_step(ctx);
	hle_jump(ctx, 0x009E5C);
}

/* $009E60 (after wait_vblanks): DMA the image to CRAM, count the step, then either start
 * the next one (back to the wait) or return. */
HLE_RANGE(0x009E60, palette_read_cram_dma, 40, 0x009E5C, 0x009E7C)
{
	ctx->sr = 0x2700;
	set_w(&ctx->d[0], 0);
	set_w(&ctx->d[1], 0x40);
	ctx->a[0] = PAL_CUR;
	vdp_dma_cram(ctx);                          /* bsr vdp_dma_cram */
	ctx->cycles += VDP_DMA_CYCLES;
	ctx->sr = 0x2000;
	set_w(&ctx->d[3], ctx->d[3] - 1);           /* dbra d3,$009E14 */
	if ((uint16_t)ctx->d[3] == 0xFFFF) {
		hle_jump(ctx, 0x009E7C);
		return;
	}
	fade_out_step(ctx);
	hle_jump(ctx, 0x009E5C);
}

/* fade-in step ($009E96..$009F00): for each colour, with t = the target colour at A0:
 *   c = cur & $EEE ; for each component (R, G, B): if c's component != t's, add 2 to it.
 * Leaves D0.w = 0 (the next instruction), D1.w/D2.w = the last (colour 0) blue parts,
 * D4.w = $FFFE, A1 = ram_pal_cur, CCR = X (borrow of the last `subi.w #2,d4`) | Z. */
static void fade_in_step(hle_ctx *ctx)
{
	uint16_t d1 = 0, d2 = 0;
	for (int off = 0x7E; off >= 0; off -= 2) {
		uint16_t c = RAM16(PAL_CUR + off) & 0xEEE;
		uint16_t t = bus_r16(ctx->a[0] + off);   /* the target is usually in ROM */
		uint16_t v = c;
		if ((c & 0xE00) != (t & 0xE00)) v += 0x200;
		if ((c & 0x0E0) != (t & 0x0E0)) v += 0x020;
		if ((c & 0x00E) != (t & 0x00E)) v += 0x002;
		RAM16(PAL_CUR + off) = v;
		d1 = c & 0x00E;
		d2 = t & 0x00E;
	}
	set_w(&ctx->d[1], d1);
	set_w(&ctx->d[2], d2);
	set_w(&ctx->d[4], 0xFFFE);
	ctx->a[1] = PAL_CUR;
	set_w(&ctx->d[0], 0);
	ctx->sr = (uint16_t)((ctx->sr & 0xFF00) | CC_X | CC_Z);
	ctx->cycles += FADE_IN_STEP_CYCLES;
}

/* $009E82 (after the movem push of $009E7E): clear ram_pal_cur to black, d3 = 7, first
 * step; the original then calls wait_vblanks. */
HLE_RANGE(0x009E82, palette_fade, 300, 0x009F06)
{
	for (uint32_t a = PAL_CUR; a < PAL_CUR + PAL_BYTES; a += 4)
		RAM32_W(a, 0);
	set_w(&ctx->d[0], 0xFFFF);                  /* dbra d0 ran out */
	set_w(&ctx->d[3], 7);
	fade_in_step(ctx);
	hle_jump(ctx, 0x009F06);
}

/* $009F0A (after wait_vblanks): DMA the image to CRAM (A0 is saved around it), count the
 * step; next step, or to the movem pop + rts at $009F2A. */
HLE_RANGE(0x009F0A, palette_fade_dma, 40, 0x009F06, 0x009F2A)
{
	push32(ctx, ctx->a[0]);
	ctx->a[0] = PAL_CUR;
	set_w(&ctx->d[0], 0);
	set_w(&ctx->d[1], 0x40);
	ctx->sr = 0x2700;
	vdp_dma_cram(ctx);
	ctx->cycles += VDP_DMA_CYCLES;
	ctx->sr = 0x2000;
	ctx->a[0] = pop32(ctx);
	set_w(&ctx->d[3], ctx->d[3] - 1);           /* dbra d3,$009E96 */
	if ((uint16_t)ctx->d[3] == 0xFFFF) {
		hle_jump(ctx, 0x009F2A);
		return;
	}
	fade_in_step(ctx);
	hle_jump(ctx, 0x009F06);
}
