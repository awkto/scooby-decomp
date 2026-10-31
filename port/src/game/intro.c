/* The mystery intro card (GitLab #24): the Mystery Machine drives across a parallax landscape
 * before a mystery starts (mystery_load $007F66 -> $0013BA). Until #24 this ran on the 68k.
 *
 *   $0013BA mystery_intro   set up the card (VBlank hook vblank_light, planes, the van sprite
 *                           tiles), fade in, then one frame per VBlank until the van has driven
 *                           to x = $10E or a button is pressed, fade out, restore the hooks.
 *   $0016C4 intro_van_sprites   the van as three 6-sprite groups (body, wheels, bounce)
 *   $00175C intro_sprite_group  write 6 sprite entries to the SAT
 *   $0017AE intro_parallax      DMA the two line-scroll tables, then advance the layers and
 *                               rebuild the tables for the next frame
 *
 * RAM (data/ram.txt, block "#24 mystery intro"): the layer positions are 16.16 longs; the
 * scroll tables take the integer word of each.
 */
#include "game.h"
#include "cc68k.h"
#include "glue.h"

#define INTRO_TABLES      0x018FEC   /* 16 bytes per mystery: tiles A, plane B map, plane A map, palette */
#define INTRO_SPRITES     0x010B14   /* the van's sprite tile set (offset table at +4) */
#define INTRO_PALETTE_FIX 0x032482   /* 15 longs: palette entries 1..30 shared by both cards */
#define INTRO_PAL_COLOR0  0x032480   /* word: colour 0 restored at the end */
#define BOUNCE_TABLE      0x00160C   /* 11 words: the van's vertical bounce, one per frame */
#define PAUSE_TABLE       0x001622   /* bytes: frames to hold the bounce after a cycle */

#define RAM_SCROLL_A      0xFF3000   /* line-scroll table of plane A (built for the next frame) */
#define RAM_SCROLL_B      0xFF3800   /* line-scroll table of plane B */
#define RAM_LAYER         0xFF09F0   /* 9 longs, 16.16: the parallax layer positions */
#define RAM_VAN_BOUNCE_X  0xFF0A14   /* w: x offset of the bounce group (0) */
#define RAM_VAN_BOUNCE    0xFF0A16   /* w: bounce height from BOUNCE_TABLE */
#define RAM_VAN_X         0xFF0A18   /* w: van x; the card ends at $10E */
#define RAM_VAN_Y         0xFF0A1A   /* w: van y */
#define RAM_VAN_WHEEL     0xFF0A1C   /* w: wheel animation frame offset (0 / $3C) */
#define RAM_VAN_STEP      0xFF0AD0   /* b: frames until the van moves on (every 2nd frame) */
#define RAM_BOUNCE_PAUSE  0xFF0ACF   /* b: frames to hold the bounce */
#define RAM_BOUNCE_IDX    0xFF08AA   /* w: byte index into BOUNCE_TABLE */
#define RAM_PAUSE_IDX     0xFF08AC   /* w: index into PAUSE_TABLE */
#define RAM_FADE_PALETTE  0xFF0778   /* the palette palette_fade fades to */

static inline uint32_t layer(int n) { return RAM_LAYER + 4 * (uint32_t)n; }

/* ------------------------------------------------------------------ sprites */
/* $00175C: six SAT entries (y, size/link, tile, x) from the ROM tables at $0FC8B4 (y steps),
 * $0FC944 (tile offsets), $0FC99C (sizes) and $0FC89C (x steps). d2/d3 = x/y of the group,
 * d5 = link of the first entry, d6 = first tile. An x of 0 is written as $FFFF. */
HLE_FUNC(0x00175C, intro_sprite_group, 640)
{
	ctx->a[3] = 0x0FC8B4;
	ctx->a[4] = 0x0FC944;
	ctx->a[5] = 0x0FC99C;
	ctx->a[2] = 0x0FC89C;
	set_w(&ctx->d[1], 5);
	for (;;) {
		set_w(&ctx->d[3], cc_add(ctx, ctx->d[3], ROM16(ctx->a[3]), 16));
		ctx->a[3] += 2;
		vdp_data_w((uint16_t)ctx->d[3]);
		set_w(&ctx->d[0], (ctx->d[5] | ROM16(ctx->a[5])) & 0xFFFF);
		ctx->a[5] += 2;
		vdp_data_w((uint16_t)ctx->d[0]);
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[6], ROM16(ctx->a[4]), 16));
		ctx->a[4] += 2;
		vdp_data_w((uint16_t)ctx->d[0]);
		set_w(&ctx->d[5], cc_add(ctx, ctx->d[5], 1, 16));
		set_w(&ctx->d[2], cc_add(ctx, ctx->d[2], ROM16(ctx->a[2]), 16));
		ctx->a[2] += 2;
		uint16_t x = (uint16_t)ctx->d[2] ? (uint16_t)ctx->d[2] : 0xFFFF;
		vdp_data_w(x);
		cc_logic16(ctx, x);
		set_w(&ctx->d[1], ctx->d[1] - 1);
		if ((ctx->d[1] & 0xFFFF) == 0xFFFF)
			break;
	}
}

/* $0016C4: the van: body (tiles $458), wheels ($494 + frame), and the bouncing roof load
 * ($50C + $3C per bounce step), written to the SAT at $B800 with interrupts off */
HLE_FUNC(0x0016C4, intro_van_sprites, 120)
{
	uint16_t sr = ctx->sr;
	vdp_ctrl_w32(0x78000002);                          /* VRAM write $B800 */
	set_w(&ctx->d[2], RAM16(RAM_VAN_BOUNCE_X) + 0x7C + RAM16(RAM_VAN_X));
	set_w(&ctx->d[3], RAM16(RAM_VAN_BOUNCE) + 0x6F + RAM16(RAM_VAN_Y));
	set_w(&ctx->d[6], 0x458);
	ctx->d[5] = 1;
	hle_call68k(ctx, 0x00175C);
	set_w(&ctx->d[2], RAM16(RAM_VAN_X) + 0x80);
	set_w(&ctx->d[3], RAM16(RAM_VAN_Y) + 0x80);
	set_w(&ctx->d[6], 0x494 + RAM16(RAM_VAN_WHEEL));
	hle_call68k(ctx, 0x00175C);
	set_w(&ctx->d[2], 0xCA + RAM16(RAM_VAN_X));
	set_w(&ctx->d[3], 0x79 + RAM16(RAM_VAN_Y));
	set_w(&ctx->d[6], 0x50C);
	set_w(&ctx->d[0], RAM16(RAM_VAN_BOUNCE));
	if (ctx->d[0] & 0xFFFF) {
		for (uint16_t n = (uint16_t)ctx->d[0]; n; n--)
			set_w(&ctx->d[6], ctx->d[6] + 0x3C);
		set_w(&ctx->d[0], 0xFFFF);                     /* subq #1 / dbra ran out */
	}
	hle_call68k(ctx, 0x00175C);
	bus_w32(VDP_DATA, 0);                              /* end of the sprite list */
	ctx->sr = sr;                                      /* move.w (sp)+,sr */
}

/* ------------------------------------------------------------------ parallax */
static void fill_words(hle_ctx *ctx, uint32_t value_var, int count)
{
	set_w(&ctx->d[1], RAM16(value_var));
	for (int i = 0; i < count; i++) {
		RAM16(ctx->a[0]) = (uint16_t)ctx->d[1];
		ctx->a[0] += 2;
	}
	set_w(&ctx->d[0], 0xFFFF);
	cc_logic16(ctx, ctx->d[1]);
}

static void layer_sub(hle_ctx *ctx, int n, uint32_t step)
{
	RAM32_W(layer(n), cc_sub(ctx, RAM32(layer(n)), step, 32, true));
}

/* $0017AE: upload this frame's line-scroll tables (DMA $FF3000 -> $BC00, $FF3800 -> $BC02,
 * every 4 bytes: plane A/B alternate), then move the layers left at their speeds and rebuild
 * the tables. Blake's Hotel has 5 bands per plane, the Ha Ha Carnival card 9. */
HLE_FUNC(0x0017AE, intro_parallax, 9500)
{
	uint16_t sr = ctx->sr;
	vdp_reg_w(15, 4);
	set_w(&ctx->d[1], 0xE0);
	set_w(&ctx->d[0], 0xBC00);
	ctx->a[0] = RAM_SCROLL_A;
	hle_call68k(ctx, 0x0099F6);                        /* vdp_dma_vram */
	set_w(&ctx->d[0], 0xBC02);
	ctx->a[0] = RAM_SCROLL_B;
	hle_call68k(ctx, 0x0099F6);
	vdp_reg_w(15, 2);
	ctx->sr = sr;
	cc_logic16(ctx, RAM16(RAM_VAN_X));
	if (!(RAM16(RAM_VAN_X) & 0x8000))
		RAM16(layer(0)) = 0xFF00;                      /* the van is on screen: road stops */
	cc_logic16(ctx, RAM16(MYSTERY_IDX));
	if (RAM16(MYSTERY_IDX)) {
		/* Ha Ha Carnival */
		layer_sub(ctx, 8, 0x8000);
		layer_sub(ctx, 1, 0xC000);
		layer_sub(ctx, 2, 0x10000);
		layer_sub(ctx, 3, 0x10000);
		layer_sub(ctx, 4, 0x18000);
		layer_sub(ctx, 5, 0x20000);
		layer_sub(ctx, 6, 0x30000);
		layer_sub(ctx, 7, 0x40000);
		ctx->a[0] = RAM_SCROLL_B;
		fill_words(ctx, layer(0), 0x68);
		fill_words(ctx, layer(8), 0x20);
		fill_words(ctx, layer(2), 0x19);
		fill_words(ctx, layer(5), 0x0F);
		fill_words(ctx, layer(6), 0x30);
		ctx->a[0] = RAM_SCROLL_A;
		fill_words(ctx, layer(1), 0x28);
		fill_words(ctx, layer(3), 0x10);
		fill_words(ctx, layer(4), 0x10);
		fill_words(ctx, layer(7), 0x98);
		return;
	}
	/* Blake's Hotel */
	layer_sub(ctx, 1, 0x8000);
	layer_sub(ctx, 2, 0x10000);
	layer_sub(ctx, 3, 0x20000);
	layer_sub(ctx, 4, 0x40000);
	ctx->a[0] = RAM_SCROLL_B;
	fill_words(ctx, layer(0), 0x50);
	ctx->a[0] = RAM_SCROLL_B + 0xA0;
	fill_words(ctx, layer(3), 0x90);
	ctx->a[0] = RAM_SCROLL_A;
	fill_words(ctx, layer(1), 0x60);
	fill_words(ctx, layer(2), 0x30);
	fill_words(ctx, layer(4), 0x50);
}

/* ------------------------------------------------------------------ the card */
/* $0013BA mystery_intro, up to the fade-in (palette_fade waits, so the range stops there).
 * Saved on the stack: SR, d0-a6 and the VBlank hook (70 bytes); the fade's jsr pushes 4 more. */
#define INTRO_FRAME (2 + 60 + 4)

HLE_RANGE(0x0013BA, mystery_intro, 26000, HLE_EXIT_SP(0x009E7E, -(INTRO_FRAME + 4)))
{
	push16(ctx, ctx->sr);
	movem_push_l(ctx, MOVEM_ALL);
	ctx->sr = 0x2700;
	push32(ctx, RAM32(VBLANK_HOOK));
	RAM32_W(VBLANK_HOOK, 0x00A4A4);                    /* vblank_light: pads and timers only */
	vdp_ctrl_w(0x9100);                                /* no window */
	vdp_ctrl_w(0x9200);
	vdp_ctrl_w(0x9003);                                /* planes 128 x 32 cells */
	vdp_ctrl_w(0x8D2F);                                /* h-scroll table at $BC00 */
	vdp_ctrl_w(0x855C);                                /* sprite table at $B800 */
	RAM32_W(HBLANK_HOOK, 0x01006A);                    /* hblank_nop */
	vdp_ctrl_w(0x8B03);                                /* line scroll */

	/* the card's tiles and both plane maps, LZSS-packed, per mystery */
	set_w(&ctx->d[0], lslw(ctx, RAM16(MYSTERY_IDX), 4));
	ctx->a[4] = INTRO_TABLES + (uint32_t)(int16_t)ctx->d[0];
	static const uint16_t vram_at[3] = { 0x0000, 0xE000, 0xC000 };
	for (int i = 0; i < 3; i++) {
		ctx->a[0] = ROM32(ctx->a[4] + 4 * (uint32_t)i);
		if (i == 0) {
			ctx->a[1] = 0xFF3000;
			ctx->a[2] = 0xFF4000;
		}
		hle_call68k(ctx, 0x0097AA);                    /* lzss_decompress a0 -> a2, size at (a1) */
		if (i == 0)
			ctx->d[0] = 0;                             /* moveq #0,d0 */
		else
			set_w(&ctx->d[0], vram_at[i]);
		ctx->d[1] = RAM32(ctx->a[1]);
		set_w(&ctx->d[1], lsrw(ctx, ctx->d[1], 1));   /* bytes -> words */
		ctx->a[0] = ctx->a[2];
		hle_call68k(ctx, 0x00992C);                    /* vram_write_words */
	}

	/* the van: six RLE-packed tile groups of $3C0 words, $780 bytes apart from tile $458 */
	ctx->a[3] = INTRO_SPRITES;
	set_w(&ctx->d[0], ROM16(ctx->a[3] + 4));
	ctx->a[5] = ctx->a[3] + (uint32_t)(int16_t)ctx->d[0];
	set_w(&ctx->d[3], 5);
	set_w(&ctx->d[0], 0x8B00);
	ctx->a[2] = 0xFF3000;
	set_w(&ctx->d[1], 0x3C0);
	for (;;) {
		set_w(&ctx->d[2], ROM16(ctx->a[5]));
		ctx->a[5] += 2;
		ctx->a[1] = ctx->a[3] + (uint32_t)(int16_t)ctx->d[2];
		ctx->a[0] = ROM32(ctx->a[1] + 6) + ctx->a[3];
		hle_call68k(ctx, 0x009C64);                    /* rle_decompress a0 -> a2 */
		ctx->a[0] = ctx->a[2];
		hle_call68k(ctx, 0x00992C);
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 0x780, 16));
		set_w(&ctx->d[3], ctx->d[3] - 1);
		if ((ctx->d[3] & 0xFFFF) == 0xFFFF)
			break;
	}
	hle_call68k(ctx, 0x0016C4);

	/* clear both scroll tables and v-scroll, park the layers, van starts off screen left */
	ctx->d[0] = 0;
	cc_logic32(ctx, 0);
	ctx->a[0] = 0xFF3000;
	for (int i = 0; i < 0x400; i++) {
		RAM32_W(ctx->a[0], 0);
		ctx->a[0] += 4;
	}
	set_w(&ctx->d[1], 0xFFFF);
	vdp_ctrl_w32(0x40000010);                          /* VSRAM write 0 */
	bus_w32(VDP_DATA, 0);
	RAM32_W(layer(0), 0);
	RAM32_W(layer(2), 0);
	RAM32_W(layer(4), 0);
	RAM32_W(layer(6), 0);
	RAM16(layer(8)) = 0;
	RAM16(RAM_VAN_Y) = 0x9F;
	RAM16(RAM_VAN_X) = 0xFF62;
	RAM8(RAM_VAN_STEP) = 1;
	RAM16(RAM_VAN_WHEEL) = 0;
	RAM32_W(RAM_VAN_BOUNCE_X, 0);
	hle_call68k(ctx, 0x0017AE);

	/* palette: the mystery's 32 colours, entries 1..30 from the shared set; Blake's Hotel
	 * has a black background */
	ctx->a[4] = INTRO_TABLES;
	set_w(&ctx->d[0], lslw(ctx, RAM16(MYSTERY_IDX), 4));
	ctx->a[0] = ROM32(ctx->a[4] + 0xC + (uint32_t)(int16_t)ctx->d[0]);
	ctx->a[1] = RAM_FADE_PALETTE;
	for (int i = 0; i < 32; i++) {
		RAM32_W(ctx->a[1], ROM32(ctx->a[0]));
		ctx->a[0] += 4;
		ctx->a[1] += 4;
	}
	ctx->a[1] = RAM_FADE_PALETTE + 2;
	ctx->a[0] = INTRO_PALETTE_FIX;
	for (int i = 0; i < 15; i++) {
		RAM32_W(ctx->a[1], ROM32(ctx->a[0]));
		ctx->a[0] += 4;
		ctx->a[1] += 4;
	}
	set_w(&ctx->d[0], 0xFFFF);
	cc_logic32(ctx, RAM32(ctx->a[1] - 4));
	cc_logic16(ctx, RAM16(MYSTERY_IDX));
	if (!RAM16(MYSTERY_IDX))
		RAM16(RAM_FADE_PALETTE) = 0;

	/* the original enables interrupts here (move.w #$2000,sr) and draws the first frame; C keeps
	 * them off over the nested calls and lowers the mask at the exit (the same instruction
	 * boundary for anything outside this code) */
	hle_call68k(ctx, 0x0017AE);
	hle_call68k(ctx, 0x0016C4);
	ctx->d[0] = 0x7C;
	cc_logic32(ctx, ctx->d[0]);
	hle_call68k(ctx, 0x009762);                        /* sound_play $7C: the van's engine */
	ctx->d[0] = 0x1E;
	cc_logic32(ctx, ctx->d[0]);
	hle_call68k(ctx, 0x009762);                        /* sound_play $1E */
	ctx->a[0] = RAM_FADE_PALETTE;
	ctx->sr = (uint16_t)((ctx->sr & 0x00FF) | 0x2000);
	glue_call(ctx, 0x009E7E, 0x0015A6);                /* palette_fade (waits) */
}

/* $0015A6: faded in. Reset the bounce, then the frame loop from $15BE: wait_vblank */
HLE_RANGE(0x0015A6, mystery_intro_start, 40, HLE_EXIT_SP(0x00974C, -4))
{
	RAM16(RAM_BOUNCE_IDX) = 0;
	RAM16(RAM_PAUSE_IDX) = 0;
	RAM8(RAM_BOUNCE_PAUSE) = 0;
	cc_logic8(ctx, 0);
	glue_call(ctx, 0x00974C, 0x0015C4);
}

GLUE_CALL(0x0015BE, 6, 0x00974C)

/* $0015C4: one frame of the card. The van bounces through BOUNCE_TABLE (a pause from
 * PAUSE_TABLE after each cycle), the scenery scrolls, the wheels turn and the van moves one
 * pixel every second frame. The card ends when the van reaches x = $10E or a button is newly
 * pressed: fade out (palette_read_cram waits). */
HLE_RANGE(0x0015C4, mystery_intro_frame, 120, 0x0015BE, HLE_EXIT_SP(0x009DE0, -4))
{
	cc_logic8(ctx, RAM8(RAM_BOUNCE_PAUSE));
	if (RAM8(RAM_BOUNCE_PAUSE)) {
		RAM8(RAM_BOUNCE_PAUSE) = (uint8_t)cc_sub(ctx, RAM8(RAM_BOUNCE_PAUSE), 1, 8, true);
	} else {
		set_w(&ctx->d[0], RAM16(RAM_BOUNCE_IDX));
		RAM16(RAM_VAN_BOUNCE) = ROM16(BOUNCE_TABLE + (uint32_t)(int16_t)ctx->d[0]);
		set_w(&ctx->d[0], ctx->d[0] + 2);
		if (cmpw(ctx, ctx->d[0], 0x16) == 0)
			ctx->d[0] = 0;                             /* moveq #0,d0 */
		RAM16(RAM_BOUNCE_IDX) = (uint16_t)ctx->d[0];
		cc_logic16(ctx, ctx->d[0]);
		if (!(ctx->d[0] & 0xFFFF)) {
			/* a bounce cycle is over: hold for PAUSE_TABLE[i] frames */
			set_w(&ctx->d[0], RAM16(RAM_PAUSE_IDX));
			RAM8(RAM_BOUNCE_PAUSE) = ROM8(PAUSE_TABLE + (uint32_t)(int16_t)ctx->d[0]);
			set_w(&ctx->d[0], ctx->d[0] + 1);
			if (cmpw(ctx, ctx->d[0], 1) == 0)
				ctx->d[0] = 0;
			RAM16(RAM_PAUSE_IDX) = (uint16_t)ctx->d[0];
		}
	}
	hle_call68k(ctx, 0x0017AE);
	hle_call68k(ctx, 0x0016C4);
	uint8_t step = (uint8_t)cc_sub(ctx, RAM8(RAM_VAN_STEP), 1, 8, true);
	RAM8(RAM_VAN_STEP) = step;
	if (step & 0x80) {
		RAM16(RAM_VAN_WHEEL) ^= 0x3C;
		RAM8(RAM_VAN_STEP) = 1;
		RAM16(RAM_VAN_X) = (uint16_t)cc_add(ctx, RAM16(RAM_VAN_X), 1, 16);
	}
	/* a button pressed this frame (any of S A C B, active low, not held last frame) */
	set_b(&ctx->d[0], RAM8(PAD1_CUR));
	set_w(&ctx->d[0], ctx->d[0] & 0xF0);
	bool pressed = false;
	if (cmpw(ctx, ctx->d[0], 0xF0) != 0) {
		set_b(&ctx->d[1], RAM8(0xFF09E2));
		set_w(&ctx->d[1], ctx->d[1] & 0xF0);
		if (cmpw(ctx, ctx->d[0], ctx->d[1]) != 0)
			pressed = true;
	}
	if (!pressed && cmpw(ctx, RAM16(RAM_VAN_X), 0x10E) != 0) {
		hle_jump(ctx, 0x0015BE);
		return;
	}
	glue_call(ctx, 0x009DE0, 0x001682);                /* palette_read_cram: fade out */
}

/* $001682: restore colour 0, the gameplay VDP registers and HBlank hook, the saved VBlank
 * hook, the registers and SR; return to mystery_load */
HLE_FUNC(0x001682, mystery_intro_end, 300)
{
	RAM16(RAM_FADE_PALETTE) = ROM16(INTRO_PAL_COLOR0);
	vdp_ctrl_w(0x8B00);                                /* full-screen scroll */
	RAM32_W(HBLANK_HOOK, 0x01006C);                    /* hblank_verb_bar */
	vdp_ctrl_w(0x856C);                                /* sprite table at $D800 */
	vdp_ctrl_w(0x8D37);                                /* h-scroll table at $DC00 */
	vdp_ctrl_w(0x9001);                                /* planes 64 x 32 */
	RAM32_W(VBLANK_HOOK, pop32(ctx));
	movem_pop_l(ctx, MOVEM_ALL);
	ctx->sr = pop16(ctx);
}
