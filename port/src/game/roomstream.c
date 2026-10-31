/* Map streaming (GitLab #24): when the camera crosses a cell boundary, the new column or row of
 * the room's tile map is copied from the RAM maps into the VDP planes. Until #24 these ran on
 * the 68k, called by camera_update (camera.c) and room_draw_view (room.c).
 *
 *   $00FC3A stream_right   the column just right of the view
 *   $00FD5E stream_left    the column just left of it
 *   $00FE74 stream_top     the row above
 *   $00FF68 stream_bottom  the row below
 *
 * The maps: plane B at $FF7000, plane A at $FF8C00, one word per cell, ram_map_width cells per
 * row. The planes are 64 x 32 cells (128 bytes a row) at $E000 (B) and $C000 (A) and wrap; a
 * column is uploaded with auto-increment $80 (one plane row per word), in two parts when it
 * crosses the bottom of the plane. Plane A is skipped while $FF09DE bit 6 is set.
 *
 * The C follows the original instruction by instruction where registers and flags are visible
 * to the caller (difftest compares them): d0-d6, a0-a3 and X N Z V C at the rts.
 */
#include "game.h"
#include "cc68k.h"

#define RAM_CAMERA_X   0xFF06CA   /* w: world x of the view's left edge */
#define RAM_CAMERA_Y   0xFF06CE   /* w */
#define RAM_MAP_W      0xFF06C6   /* w: room map width in cells */
#define RAM_MAP_H      0xFF06C8   /* w: room map height in cells */
#define RAM_MAP_B      0xFF7000   /* plane B map */
#define RAM_MAP_A      0xFF8C00   /* plane A map */
#define RAM_LINE_BUF   0xFF042C   /* gather buffer for one column / row */
#define RAM_DRAW_FLAGS 0xFF09DE   /* b: bit 6 = don't touch plane A */
#define VDP_DMA_VRAM   0x0099F6   /* d0 = VRAM address, d1 = words, a0 = source */

static inline void move_w(hle_ctx *ctx, uint32_t *d, uint32_t v) { set_w(d, v); cc_logic16(ctx, v); }
static inline void add_w(hle_ctx *ctx, uint32_t *d, uint32_t s) { set_w(d, cc_add(ctx, *d, s, 16)); }
static inline void sub_w(hle_ctx *ctx, uint32_t *d, uint32_t s) { set_w(d, cc_sub(ctx, *d, s, 16, true)); }
static inline void and_w(hle_ctx *ctx, uint32_t *d, uint32_t m) { set_w(d, *d & m); cc_logic16(ctx, *d); }

/* d3 = d1 - 1 / lea buf,a0 / movea.l a0,a1 / copy d1 words from *src, stepping by STEP bytes
 * (dbra d3); the source register keeps its final value */
static void gather(hle_ctx *ctx, uint32_t *src, int step)
{
	move_w(ctx, &ctx->d[3], ctx->d[1]);
	sub_w(ctx, &ctx->d[3], 1);
	ctx->a[0] = RAM_LINE_BUF;
	ctx->a[1] = ctx->a[0];
	for (;;) {
		uint16_t w = RAM16(*src);
		RAM16(ctx->a[1]) = w;
		cc_logic16(ctx, w);
		ctx->a[1] += 2;
		*src += step ? (uint32_t)(int16_t)step : 2;
		set_w(&ctx->d[3], ctx->d[3] - 1);
		if ((ctx->d[3] & 0xFFFF) == 0xFFFF)
			break;
	}
}

/* one part of a column / row: plane B, then plane A unless suppressed */
static void upload(hle_ctx *ctx, int step)
{
	move_w(ctx, &ctx->d[0], 0xE000);
	add_w(ctx, &ctx->d[0], ctx->d[2]);
	gather(ctx, &ctx->a[2], step);
	hle_call68k(ctx, VDP_DMA_VRAM);
	move_w(ctx, &ctx->d[0], 0xC000);
	add_w(ctx, &ctx->d[0], ctx->d[2]);
	gather(ctx, &ctx->a[3], step);
	cc_btst(ctx, RAM8(RAM_DRAW_FLAGS), 6);
	if (!(RAM8(RAM_DRAW_FLAGS) & 0x40))
		hle_call68k(ctx, VDP_DMA_VRAM);
}

/* map offset of (row d5, column d4) in bytes, d6 = row stride (both copies of the original's
 * register arithmetic: d2 = 2*col + row*stride) */
static void map_offset(hle_ctx *ctx)
{
	move_w(ctx, &ctx->d[2], ctx->d[4]);
	move_w(ctx, &ctx->d[3], ctx->d[5]);
	add_w(ctx, &ctx->d[2], ctx->d[2]);
	move_w(ctx, &ctx->d[6], RAM16(RAM_MAP_W));
	add_w(ctx, &ctx->d[6], ctx->d[6]);
	ctx->d[3] = (ctx->d[3] & 0xFFFF) * (ctx->d[6] & 0xFFFF);   /* mulu.w d6,d3 */
	cc_logic32(ctx, ctx->d[3]);
	add_w(ctx, &ctx->d[2], ctx->d[3]);
}

/* the column at map (d5 - 1, d4) .. down 21 cells, into plane column d4 & 63 from plane row
 * (d5 + 1) & 31; shared tail of stream_right / stream_left */
static void stream_column(hle_ctx *ctx)
{
	map_offset(ctx);
	sub_w(ctx, &ctx->d[2], ctx->d[6]);
	ctx->a[2] = RAM_MAP_B + (uint32_t)(int16_t)ctx->d[2];
	ctx->a[3] = RAM_MAP_A + (uint32_t)(int16_t)ctx->d[2];
	add_w(ctx, &ctx->d[5], 1);
	and_w(ctx, &ctx->d[4], 0x3F);
	and_w(ctx, &ctx->d[5], 0x1F);
	move_w(ctx, &ctx->d[1], 0x20);
	sub_w(ctx, &ctx->d[1], ctx->d[5]);               /* rows until the plane wraps */
	if ((int16_t)cmpw(ctx, ctx->d[1], 0x15) > 0)
		move_w(ctx, &ctx->d[1], 0x15);              /* the view is 21 rows */
	set_w(&ctx->d[5], lslw(ctx, ctx->d[5], 7));
	move_w(ctx, &ctx->d[2], ctx->d[4]);
	add_w(ctx, &ctx->d[2], ctx->d[4]);
	add_w(ctx, &ctx->d[2], ctx->d[5]);              /* plane offset: row * 128 + col * 2 */
	vdp_ctrl_w(0x8F80);                              /* auto-increment one plane row */
	cc_logic16(ctx, 0x8F80);
	int step = (int16_t)ctx->d[6];
	upload(ctx, step);
	sub_w(ctx, &ctx->d[1], 0x15);
	if ((int16_t)ctx->d[1] < 0) {
		/* the rest from the top of the plane */
		set_w(&ctx->d[1], neg16(ctx, ctx->d[1]));
		move_w(ctx, &ctx->d[2], ctx->d[4]);
		add_w(ctx, &ctx->d[2], ctx->d[4]);
		upload(ctx, step);
	}
	vdp_ctrl_w(0x8F02);
	cc_logic16(ctx, 0x8F02);
}

/* the row at map (d5, d4 - 1) .., 34 cells, into plane row d5 & 31 from column (d4 - 1) & 63;
 * shared tail of stream_top / stream_bottom (d5 already advanced by the caller) */
static void stream_row(hle_ctx *ctx)
{
	and_w(ctx, &ctx->d[4], 0x3F);
	and_w(ctx, &ctx->d[5], 0x1F);
	move_w(ctx, &ctx->d[1], 0x40);
	sub_w(ctx, &ctx->d[1], ctx->d[4]);               /* columns until the plane wraps */
	if ((int16_t)cmpw(ctx, ctx->d[1], 0x22) > 0)
		move_w(ctx, &ctx->d[1], 0x22);              /* the view is 34 columns incl. margins */
	set_w(&ctx->d[5], lslw(ctx, ctx->d[5], 7));
	move_w(ctx, &ctx->d[2], ctx->d[4]);
	add_w(ctx, &ctx->d[2], ctx->d[4]);
	add_w(ctx, &ctx->d[2], ctx->d[5]);
	upload(ctx, 0);
	sub_w(ctx, &ctx->d[1], 0x22);
	if ((int16_t)ctx->d[1] < 0) {
		/* the rest from the left edge of the plane */
		set_w(&ctx->d[1], neg16(ctx, ctx->d[1]));
		move_w(ctx, &ctx->d[2], ctx->d[5]);
		upload(ctx, 0);
	}
}

/* camera x in cells (move.w / lsr.w #3) */
static void cell_x(hle_ctx *ctx, uint32_t *d) { move_w(ctx, d, RAM16(RAM_CAMERA_X)); set_w(d, lsrw(ctx, *d, 3)); }
static void cell_y(hle_ctx *ctx, uint32_t *d) { move_w(ctx, d, RAM16(RAM_CAMERA_Y)); set_w(d, lsrw(ctx, *d, 3)); }

/* $00FC3A: the view moved right: stream the column at x + 32 cells (not at the right edge) */
HLE_FUNC(0x00FC3A, stream_right, 1200)
{
	cell_x(ctx, &ctx->d[4]);
	move_w(ctx, &ctx->d[5], RAM16(RAM_MAP_W));
	sub_w(ctx, &ctx->d[5], 0x20);
	if (cmpw(ctx, ctx->d[4], ctx->d[5]) == 0) {
		vdp_ctrl_w(0x8F02);
		cc_logic16(ctx, 0x8F02);
		return;
	}
	cell_y(ctx, &ctx->d[5]);
	add_w(ctx, &ctx->d[4], 0x20);
	stream_column(ctx);
}

/* $00FD5E: the view moved left: stream the column at x - 1 (not at the left edge) */
HLE_FUNC(0x00FD5E, stream_left, 1200)
{
	cell_x(ctx, &ctx->d[4]);
	if (!(ctx->d[4] & 0xFFFF)) {
		vdp_ctrl_w(0x8F02);
		cc_logic16(ctx, 0x8F02);
		return;
	}
	sub_w(ctx, &ctx->d[4], 1);
	cell_y(ctx, &ctx->d[5]);
	stream_column(ctx);
}

/* $00FE74: the view moved up: stream the row above (not at the top edge) */
HLE_FUNC(0x00FE74, stream_top, 1500)
{
	cell_y(ctx, &ctx->d[5]);
	if (!(ctx->d[5] & 0xFFFF))
		return;
	cell_x(ctx, &ctx->d[4]);
	map_offset(ctx);
	sub_w(ctx, &ctx->d[2], ctx->d[6]);
	ctx->a[2] = RAM_MAP_B - 2 + (uint32_t)(int16_t)ctx->d[2];
	ctx->a[3] = RAM_MAP_A - 2 + (uint32_t)(int16_t)ctx->d[2];
	add_w(ctx, &ctx->d[5], 1);
	sub_w(ctx, &ctx->d[4], 1);
	stream_row(ctx);
}

/* $00FF68: the view moved down: stream the row below (not at the bottom edge) */
HLE_FUNC(0x00FF68, stream_bottom, 1500)
{
	/* the bottom edge: the view is 19 rows (move.w / move.w / subi / lsr / cmp) */
	move_w(ctx, &ctx->d[5], RAM16(RAM_CAMERA_Y));
	move_w(ctx, &ctx->d[4], RAM16(RAM_MAP_H));
	sub_w(ctx, &ctx->d[4], 0x13);
	set_w(&ctx->d[5], lsrw(ctx, ctx->d[5], 3));
	if (cmpw(ctx, ctx->d[5], ctx->d[4]) == 0)
		return;
	add_w(ctx, &ctx->d[5], 0x13);
	cell_x(ctx, &ctx->d[4]);
	map_offset(ctx);
	ctx->a[2] = RAM_MAP_B - 2 + (uint32_t)(int16_t)ctx->d[2];
	ctx->a[3] = RAM_MAP_A - 2 + (uint32_t)(int16_t)ctx->d[2];
	add_w(ctx, &ctx->d[5], 2);
	sub_w(ctx, &ctx->d[4], 1);
	stream_row(ctx);
}
