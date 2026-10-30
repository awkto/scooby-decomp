/* Palette cycling (VBlank), GitLab #24 follow-up.  Script op "pal_cycle" sets up to 5 slots; this
 * is the per-frame step that rotates the colours of every due slot and uploads them. */
#include "game.h"
#include "cc68k.h"

#define PALCYC           0xFF087C   /* 5 slots, 5 word arrays of 10 bytes each (see below) */
#define PALCYC_ON        0xFF0ACD   /* b: bit n = slot n is cycling */
#define PAL_CUR          0xFF06F8   /* the CRAM image (64 words) */
/* PALCYC layout, slot i at +2*i: +$00 first colour index, +$0A last colour index, +$14 frames per
 * step (the reload value), +$1E frames left until the next step */

/* $00A7CE palette_cycle: called from vblank_main.  For every slot that is on (bit 4..0 of
 * PALCYC_ON, slot 4 first): count its timer down; when it runs out reload it and rotate the
 * colours first..last of the CRAM image one place towards the front (the first one wraps to the
 * end), then DMA just that range to CRAM (vdp_dma_cram: d0 = CRAM address, d1 = words, a0 = source).
 * A slot with first == last would loop 65536 times in the original (dbra on -1); this is
 * reproduced, not guarded.
 * Registers/flags: all scratch, the only caller is vblank_main (exemption in difftest.c). */
HLE_FUNC(0x00A7CE, palette_cycle, 0)
{
	int cyc = 12;
	for (int slot = 4; slot >= 0; slot--) {
		uint32_t off = 2 * slot;
		cyc += 44;
		if (!(RAM8(PALCYC_ON) & (1 << slot)))
			continue;
		cyc += 44;
		uint16_t left = (uint16_t)(RAM16(PALCYC + 0x1E + off) - 1);
		RAM16(PALCYC + 0x1E + off) = left;
		if (!(left & 0x8000))                       /* not due yet */
			continue;
		RAM16(PALCYC + 0x1E + off) = RAM16(PALCYC + 0x14 + off);
		uint16_t first = RAM16(PALCYC + off);
		uint16_t span = (uint16_t)(RAM16(PALCYC + 0x0A + off) - first);   /* colours - 1 */
		uint32_t base = PAL_CUR + 2 * first;
		uint16_t save = RAM16(base);
		uint32_t p = base;
		uint32_t n = (uint32_t)(uint16_t)(span - 1) + 1;   /* dbra: span 0 wraps to 65536 */
		for (uint32_t i = 0; i < n; i++, p += 2)
			RAM16(p) = RAM16(p + 2);
		RAM16(p) = save;
		cyc += 180 + 26 * (int)n;
		set_w(&ctx->d[0], 2 * first);
		set_w(&ctx->d[1], (uint16_t)(span + 1));
		ctx->a[0] = base;
		vdp_dma_cram(ctx);
		cyc += VDP_DMA_CYCLES;
	}
	ctx->cycles = cyc;
}
