/* Scaled-bitmap blits ($00E40C, $00F272): the title bats' zoom and the menu pictures.
 *
 * Both routines shrink a chunky picture (one byte per pixel, a 4-bit colour in each byte)
 * and write the result as 4bpp tiles, ready to DMA to VRAM, in the order the tile table says.
 * "Scaling" is plain row/column dropping: the depth (SCALE_DEPTH_X/Y, 0 = full size) selects
 * a 16-byte bit mask in `depth_masks` ($00C3F0, MSB first, a 1 keeps the line/pixel) for the
 * rows (Y depth) and one for the columns (X depth).  Kept pixels are packed to the left and
 * the line is padded with colour 0, dropped rows are replaced by blank rows at the bottom.
 * An optional per-row horizontal wobble (bit 4 of $FF09EB) shifts where each source row is
 * read from.  Format details: docs/formats.md, "Scaled bitmap blits".
 *
 *   $00E40C scaled_blit_120x40  src $FF3800 (the picture, unpacked to the LZSS ring + $800),
 *                               120x40 px -> 600 longs at ZONE_MAP ($FFE000), order table $B5C0
 *   $00F272 scaled_blit_64x32   src $FF5000, 64x32 px -> 256 longs at PLANEA_NAMES ($FFC400),
 *                               order table $BA70
 *
 * Callers: menu_open, title_bats_frame, menu_select_draw, password and sound-test frames (all
 * through hle_call68k; the 68k callers wrap them in movem pairs).
 * Registers on return: d0 d1 d2 a0-a6 are exact; d3-d7 are scratch (the loops' counters, no caller
 * reads them: every 68k caller either restores the registers with movem or is a C range that
 * leaves d3-d7 undefined too) and checked with --ignore-regs in port/tools/dt-blit. The flags
 * are exact (the C callers' movem emulation keeps the last callee's flags). */
#include "game.h"
#include "cc68k.h"

#define DEPTH_MASKS     0x00C3F0    /* 256 x 16 bytes: keep bits for depth 0..255 */
#define TILE_ORDER_BIG  0x00B5C0    /* w[600]: byte offsets of the output longs, in write order */
#define TILE_ORDER_SML  0x00BA70    /* w[256] */
#define STRIP_TABLE     0x008828    /* w[39]: wobble offsets (a ring, ends at $8876) */
#define STRIP_TABLE_END 0x008876
#define STRIP_INDEX     0xFF000C    /* w: byte offset into the wobble table, start of the ring */
#define SCREEN_FLAGS_09EB 0xFF09EB  /* bit 4: wobble enabled */
#define SCALE_SRC_BIG   0xFF3800
#define SCALE_SRC_SML   0xFF5000
#define SCALE_DEPTH_X_  0xFF0624
#define SCALE_DEPTH_Y_  0xFF0626

typedef struct blit_job {
	uint32_t src;         /* chunky source, `width` bytes per row */
	uint32_t dst;         /* tile buffer (long offsets come from `order`) */
	uint32_t order;       /* ROM table of output long offsets */
	int width, rows;      /* source size in pixels (width a multiple of 8) */
	int wobble_sign;      /* wobble table word is added (+1) or subtracted (-1) */
} blit_job;

typedef struct blit_result {
	uint32_t wobble_ptr;  /* wobble ring pointer after the call (a6) */
	uint32_t last_row;    /* address after the last kept row's source pixels, 0 if none (a5) */
	int rows_kept;
	unsigned cycles;      /* original's 68000 cycle cost (model) */
} blit_result;

/* keep bit `i` of a mask (a big-endian byte string, MSB first) */
static inline int mask_bit(uint32_t base, int i) { return (ROM8(base + (i >> 3)) >> (7 - (i & 7))) & 1; }

static void scaled_blit(const blit_job *j, uint32_t ymask, uint32_t xmask, blit_result *res)
{
	uint32_t wob = STRIP_TABLE + RAM16(STRIP_INDEX);
	int wobble_on = (RAM8(SCREEN_FLAGS_09EB) >> 4) & 1;
	int longs_per_row = j->width / 8;
	int slot = 0;                       /* next index into the order table */
	uint32_t acc = 0;                   /* the 8 pixels being packed, 4 bits each */
	unsigned cyc = 150;
	res->last_row = 0;
	res->rows_kept = 0;

	for (int row = 0; row < j->rows; row++) {
		/* the wobble word is consumed for every source row, kept or not */
		int16_t w = (int16_t)ROM16(wob);
		wob += 2;
		cyc += 8 + 14 + 8 + 4 + 4;
		if ((int32_t)wob >= STRIP_TABLE_END) { wob = STRIP_TABLE; cyc += 12 - 2; }
		uint32_t src = j->src + (uint32_t)row * j->width;
		cyc += 8 + 4 + 10;                          /* row end: lea, subq, bpl */
		if (j->wobble_sign < 0) cyc += 4;           /* neg.w */
		if ((row & (j->rows == 40 ? 7 : 15)) == 0)  /* fetch the next 8 (16) Y mask bits */
			cyc += j->rows == 40 ? 8 + 22 + 8 + 16 : 8 + 8 + 16;
		if (!mask_bit(ymask, row)) { cyc += 10; continue; }
		cyc += 12;

		uint32_t p = src;
		if (wobble_on)
			p += (int32_t)(int16_t)(j->wobble_sign < 0 ? -w : w);
		cyc += 4 + 8 + 4 + 20 + (wobble_on ? 8 + 12 : 10);
		int kept = 0;
		for (int x = 0; x < j->width; x++) {
			uint8_t pix = RAM8(p + x);
			if (x % 16 == 0) cyc += 12;             /* move.w (n,a3),d3 */
			if (!mask_bit(xmask, x)) { cyc += 8 + 4 + 10; continue; }
			acc = (acc << 4) | pix;                 /* or.b: a pixel is expected to be 0..15 */
			kept++;
			cyc += 8 + 4 + 12 + 16 + 4 + 4 + 4;
			if (kept % 8 == 0) {                    /* 8 pixels: one long of tile row */
				RAM32_W(j->dst + (int16_t)ROM16(j->order + 2 * slot), acc);
				slot++;
				cyc += 12 + 4 + 8 + 18;
			} else
				cyc += 10;
		}
		res->last_row = p + j->width;
		/* pad the row with colour 0 up to the full width: the pixels still missing from the
		 * long being packed shift in as zeros, then whole blank longs follow */
		int pad = j->width - kept;
		cyc += 4 + 10;
		if (pad) {
			int part = pad & 7;
			cyc += 4 + 12 + 4 + 8 + 12 + 4 + 8;
			if (part) {
				acc <<= 4 * part;
				RAM32_W(j->dst + (int16_t)ROM16(j->order + 2 * slot), acc);
				slot++;
				cyc += 4 + 10 + 8 + 2 * 4 * part + 8 + 18 - 12;
			}
			for (int n = 0; n < pad / 8; n++) {
				RAM32_W(j->dst + (int16_t)ROM16(j->order + 2 * slot), 0);
				slot++;
				cyc += 8 + 18 + 14;
			}
		}
		res->rows_kept++;
		cyc += 4;
	}
	/* rows dropped by the Y mask: blank rows at the bottom */
	for (int r = res->rows_kept; r < j->rows; r++) {
		for (int n = 0; n < longs_per_row; n++) {
			RAM32_W(j->dst + (int16_t)ROM16(j->order + 2 * slot), 0);
			slot++;
		}
		cyc += longs_per_row * 26 + 10;
	}
	res->wobble_ptr = wob;
	res->cycles = cyc;
}

/* The registers and flags the original leaves, from the result of the blit. */
static void blit_finish(hle_ctx *ctx, const blit_job *j, uint32_t ymask, uint32_t xmask,
                        const blit_result *r, int ymask_bytes)
{
	int total = j->rows * j->width / 8;
	bool blank_rows = r->rows_kept < j->rows;
	ctx->a[0] = j->dst;
	ctx->a[1] = j->src + (uint32_t)j->rows * j->width;
	ctx->a[2] = ymask + ymask_bytes;
	ctx->a[3] = xmask;
	ctx->a[4] = j->order + 2 * total;
	if (r->last_row) ctx->a[5] = r->last_row;
	ctx->a[6] = r->wobble_ptr;
	/* d0: the loop counters run down to -1; a blank-row fill leaves the last order entry in its low word */
	ctx->d[0] = blank_rows ? 0xFFFF0000u | ROM16(j->order + 2 * (total - 1)) : 0xFFFFFFFFu;
	ctx->d[1] = blank_rows ? 0 : (ctx->d[1] & 0xFFFF0000u);   /* mask byte/word shifted out */
	ctx->d[2] = 0xFFFF;
	/* last flag-setting instruction: addi.w to the row count (no blank rows: N, nothing else) or
	 * the blank fill's move.l #0 (Z; X = carry of that addi, set unless the count was 0) */
	if (!blank_rows)
		cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, CC_N);
	else
		cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, CC_Z | (r->rows_kept ? CC_X : 0));
}

/* $00E40C: the 120x40 picture -> ZONE_MAP (title bats, menu pictures) */
HLE_FUNC(0x00E40C, scaled_blit_120x40, 0)
{
	blit_job j = { SCALE_SRC_BIG, ZONE_MAP, TILE_ORDER_BIG, 120, 40, -1 };
	uint32_t ymask = DEPTH_MASKS + (int16_t)(RAM16(SCALE_DEPTH_Y_) << 4);
	uint32_t xmask = DEPTH_MASKS + (int16_t)(RAM16(SCALE_DEPTH_X_) << 4);
	blit_result r;
	scaled_blit(&j, ymask, xmask, &r);
	blit_finish(ctx, &j, ymask, xmask, &r, 5);
	ctx->cycles = (int)(r.cycles * 101 / 100);   /* model + 1 %: measured on the original */
}

/* $00F272: the 64x32 picture -> PLANEA_NAMES (plane A tiles of the menu pictures) */
HLE_FUNC(0x00F272, scaled_blit_64x32, 0)
{
	blit_job j = { SCALE_SRC_SML, PLANEA_NAMES, TILE_ORDER_SML, 64, 32, +1 };
	uint32_t ymask = DEPTH_MASKS + (int16_t)(RAM16(SCALE_DEPTH_Y_) << 4);
	uint32_t xmask = DEPTH_MASKS + (int16_t)(RAM16(SCALE_DEPTH_X_) << 4);
	blit_result r;
	scaled_blit(&j, ymask, xmask, &r);
	blit_finish(ctx, &j, ymask, xmask, &r, 4);
	ctx->cycles = (int)(r.cycles * 101 / 100);   /* model + 1 %: measured on the original */
}
