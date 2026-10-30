/* HUD pieces that the VBlank handler draws (GitLab #24 follow-up). */
#include "game.h"
#include "cc68k.h"

#define VERB_STATE       0xFF09DE   /* b: bit 2 = a screen build is in progress */
#define GAME_STATE_BLOCK 0xFF2A00   /* b: bit 1 = ... (vblank_main skips the arrows while set) */
#define UI_TILE_BASE     0xFF0802   /* w: first tile of the UI art, added to every name */
#define ARROW_UP_FRAME   0xFF0804   /* w: frame (0..n) of the inventory "up" arrow */
#define ARROW_DOWN_FRAME 0xFF0806   /* w: frame of the "down" arrow */
#define ARROW_UP_TIMER   0xFF0AB1   /* b: frames the up arrow is still pressed; counts down to -1 */
#define ARROW_DOWN_TIMER 0xFF0AB2
#define ARROW_UP_ART     0x03008C   /* ROM: 3 rows x 2 name words per frame, 0x80 bytes between rows */
#define ARROW_DOWN_ART   0x03020C
#define ARROW_UP_VRAM    0xAAF6     /* plane position (col 59, row 21 / 24 of the 64-wide plane at $A000) */
#define ARROW_DOWN_VRAM  0xAC76

/* one arrow: 2x3 name-table words from the art for `frame`, to VRAM at `vram`, the tile base added */
static void draw_arrow(hle_ctx *ctx, uint32_t art, uint16_t frame, uint16_t vram)
{
	uint32_t a0 = art + (int16_t)(frame * 6 << 7);        /* frame * 0x300 */
	uint16_t d0 = vram;
	uint16_t base = RAM16(UI_TILE_BASE);
	for (int row = 0; row < 3; row++) {
		set_w(&ctx->d[1], (uint16_t)(ROM16(a0) + base));
		set_w(&ctx->d[0], d0);
		vram_write_word(ctx);
		d0 += 2;
		set_w(&ctx->d[1], (uint16_t)(ROM16(a0 + 2) + base));
		set_w(&ctx->d[0], d0);
		vram_write_word(ctx);
		d0 += 0x7E;
		a0 += 0x80;
	}
	set_w(&ctx->d[0], d0);
	ctx->a[0] = a0;
}

/* $00A704 (label meters_draw, really the inventory scroll arrows): VBlank, via vblank_main.
 * Unless a screen build (verb state bit 2) or the game-state bit 1 says no: count the two arrow
 * timers down (to -1), then redraw both arrows (2x3 tiles each) into plane A at their frame
 * ($FF0804 up, $FF0806 down; the frames are set by inventory_arrows_update $005B92).
 * Exact registers: d0 = VRAM address after the second arrow, d1 = last name written, d2.w = -1,
 * d3.w = tile base, a0 = end of the second arrow's art. */
HLE_FUNC(0x00A704, inventory_arrows_draw, 0)
{
	cc_btst(ctx, RAM8(VERB_STATE), 2);
	if (RAM8(VERB_STATE) & 4) { ctx->cycles = 78; return; }
	cc_btst(ctx, RAM8(GAME_STATE_BLOCK), 1);
	if (RAM8(GAME_STATE_BLOCK) & 2) { ctx->cycles = 78; return; }
	cc_logic8(ctx, RAM8(ARROW_UP_TIMER));
	if (!(RAM8(ARROW_UP_TIMER) & 0x80))
		RAM8(ARROW_UP_TIMER) = (uint8_t)cc_sub(ctx, RAM8(ARROW_UP_TIMER), 1, 8, true);
	set_w(&ctx->d[3], RAM16(UI_TILE_BASE));
	set_w(&ctx->d[2], 2);
	draw_arrow(ctx, ARROW_UP_ART, RAM16(ARROW_UP_FRAME), ARROW_UP_VRAM);
	cc_logic8(ctx, RAM8(ARROW_DOWN_TIMER));
	if (!(RAM8(ARROW_DOWN_TIMER) & 0x80))
		RAM8(ARROW_DOWN_TIMER) = (uint8_t)cc_sub(ctx, RAM8(ARROW_DOWN_TIMER), 1, 8, true);
	draw_arrow(ctx, ARROW_DOWN_ART, RAM16(ARROW_DOWN_FRAME), ARROW_DOWN_VRAM);
	ctx->d[2] = 0xFFFF;                                    /* moveq #2 / dbra: low word -1 */
	/* last flag-setting instruction: addi.w #$7E,d0 of the last row: 0xADF6, N only */
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, CC_N);
	ctx->cycles = 3810;            /* measured: 3780..3846 (the six vdp_write_word calls included) */
}
