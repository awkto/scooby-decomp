/* Sprite / actor animation subsystem (GitLab #15).
 *
 * Six actor slots (0 Shaggy, 1 Scooby, 2..5 actors of the room) each play an animation
 * script from an "animation set" in ROM. A frame is an RLE stream ($009C64) that unpacks
 * either straight to 4bpp tiles (most room actors) or to chunky pixels (one byte per pixel;
 * Shaggy, Scooby and a few others) which are then depth-scaled and packed into tiles.
 * The tiles are staged in a RAM buffer per slot and DMA'd to the slot's VRAM area during
 * VBlank, one slot per frame. Each frame is shown as 4..8 hardware sprites whose layout
 * depends only on the frame size. Formats: docs/formats.md "Actor sprites"; the function
 * list, call flow and test results: docs/subsystems/sprite.md.
 *
 *   main loop / scripts --> actor_animate $00B0B8   (script step, RLE decode, depth scaling)
 *   vblank_main         --> actor_dma $00AF24 --> sprite_build $00A984 (SAT), SAT DMA, timers
 *
 * The frame pipeline of one slot (bit sets in sprite.h):
 *   script op 2 "frame"  -> tiles-format frame: decode into the slot buffer, TILES_READY
 *                        -> chunky frame: remember the stream, NEED_SCALE; the second loop of
 *                           actor_animate decodes it to $FF6000, scales it into the slot
 *                           buffer, TILES_READY
 *   actor_dma            -> DMA the buffer to VRAM, clear TILES_READY / DMA_DONE, set ANIM_LOOPED
 *   sprite_build         -> latch the new frame header, hot spot and depth for display
 *                           (DMA_DONE), draw the pieces.
 */
#include "sprite.h"
#include "cc68k.h"

#define RLE_DECOMPRESS 0x009C64
#define VDP_DMA_VRAM   0x0099F6
#define SOUND_PLAY     0x009762
#define SOUND_STOP     0x009776

/* the game's names for RAM used here that belong to other subsystems */
#define POINTER_X       0xFF06DC   /* pointer mode crosshair, screen px */
#define POINTER_Y       0xFF06DE
#define POINTER_DX      0xFF06E0
#define POINTER_DY      0xFF06E2
#define UI_TILE_BASE_B  0xFF06E4   /* crosshair tile */
#define UI_TILE_BASE_C  0xFF06E6   /* verb-bar cursor tile (verb page) */
#define UI_TILE_BASE_D  0xFF06E8   /* verb-bar cursor tile (inventory page) */
#define VERB_STATE      0xFF09DE
#define INV_HL_X        0xFF06EE   /* w: inventory highlight cell x (<0: none) */
#define INV_HL_Y        0xFF06F0
#define INV_HL_ATTR     0xFF06F2
#define FX_SPRITE_PTR   0xFF08A6   /* l: (x, y) table walked by the effect sprite ($09E8 bit 4) */
#define FX_SPRITE_TILE  0xFF08A4
#define FX_BLINK_TIMER  0xFF0ACE
#define TITLE_STRIP_IDX 0xFF000C

static inline bool bit8(uint32_t a, int b) { return (RAM8(a) >> (b & 7)) & 1; }
static inline void set8(uint32_t a, int b) { RAM8(a) |= (uint8_t)(1u << (b & 7)); }
static inline void clr8(uint32_t a, int b) { RAM8(a) &= (uint8_t)~(1u << (b & 7)); }
static inline void setw(uint32_t *r, uint32_t v) { *r = (*r & 0xFFFF0000u) | (v & 0xFFFF); }
static inline int16_t S16(uint32_t a) { return (int16_t)RAM16(a); }

/* animation sets and frame headers live in ROM, buffers in RAM */
static inline uint16_t rd16(uint32_t a) { return (a & 0xFF0000) == 0xFF0000 ? RAM16(a) : ROM16(a); }
static inline uint8_t rd8(uint32_t a) { return (a & 0xFF0000) == 0xFF0000 ? RAM8(a) : ROM8(a); }
static inline uint32_t rd32(uint32_t a) { return ((uint32_t)rd16(a) << 16) | rd16(a + 2); }

/* X flag of a 16-bit add / sub (the only flag sprite_build leaves that is not constant) */
static inline unsigned x_add16(uint32_t d, uint32_t s) { return ((d & 0xFFFF) + (s & 0xFFFF)) > 0xFFFF; }
static inline unsigned x_sub16(uint32_t d, uint32_t s) { return (s & 0xFFFF) > (d & 0xFFFF); }

/* ================================================================== frame packing */
/* The scalers read the chunky frame at CHUNKY_BUF (one byte per pixel, row-major) and write
 * 4bpp tiles to the slot buffer at A0. Each output row is cut into 8-pixel groups; a group is
 * one long stored at A0 + offs[row * groups + group] (ROM tables $00BC70 / $00C030 that map
 * the raster onto the column-major tiles of the sprite pieces). */

/* $00D3F0 / $00D42C: no scaling: pack every 8 bytes into a long (as two words). */
static void frame_pack(hle_ctx *ctx, uint32_t offs)
{
	uint32_t src = CHUNKY_BUF, dst = ctx->a[0];
	for (int i = 0; i < 480; i++) {
		uint16_t hi = 0, lo = 0;
		for (int k = 0; k < 4; k++)       /* lsl.w #4 / or.b: a byte > 15 spills into the next nibble */
			hi = (uint16_t)((hi << 4) | RAM8(src++));
		for (int k = 0; k < 4; k++)
			lo = (uint16_t)((lo << 4) | RAM8(src++));
		int16_t o = (int16_t)ROM16(offs + 2 * i);
		RAM16(dst + o) = hi;
		RAM16(dst + o + 2) = lo;
	}
	/* registers as the 68k leaves them are dead (see the note above frame_scale) */
	ctx->a[1] = offs + 960;
	ctx->a[2] = src;
	set_w(&ctx->d[0], 0xFFFF);
}

/* $00D468 / $00DA6A: depth scaling. SCALE_MASKS + depth*16 holds 8 words of "keep" bits
 * (MSB first, bit set = keep this column / row); depth 0 keeps everything, larger depths
 * drop evenly spread columns and rows. Kept rows are packed to the top and kept pixels to
 * the left of each row; the rest is filled with colour 0, so the sprite shrinks towards
 * its top left corner (sprite_build compensates with the scaled hot spot).
 *
 * Only caller: actor_animate ($00B0B8), between movem.l d0/a3-a5,-(sp) and a restore of
 * d0/a0-a1/a5; it reloads every other register before using it and its own caller sees
 * the registers of its entry (movem). So the final D0-D7/A1-A5 and the CCR of these four
 * routines are dead; difftest ignores them (dt_ignores). */
#define SCALE_MASKS 0x00C3F0
static void frame_scale(hle_ctx *ctx, int w, int h, uint32_t offs)
{
	uint32_t dst = ctx->a[0];
	/* lsl.w #4 / lea (0,a3,d0.w): 16-bit index */
	uint32_t xmask = SCALE_MASKS + (int16_t)(uint16_t)(RAM16(SCALE_DEPTH_X) << 4);
	uint32_t ymask = SCALE_MASKS + (int16_t)(uint16_t)(RAM16(SCALE_DEPTH_Y) << 4);
	int groups = w / 8, out_rows = 0, ofs_i = 0;
	uint32_t acc = ctx->d[7];
	for (int y = 0; y < h; y++) {
		if (!((ROM16(ymask + 2 * (y >> 4)) << (y & 15)) & 0x8000))
			continue;                                   /* row dropped */
		uint32_t row = CHUNKY_BUF + (uint32_t)(y * w);
		int kept = 0;
		for (int x = 0; x < w; x++) {
			if (!((ROM16(xmask + 2 * (x >> 4)) << (x & 15)) & 0x8000))
				continue;                               /* column dropped */
			acc = (acc << 4) | RAM8(row + x);
			if ((++kept & 7) == 0)
				RAM32_W(dst + (int16_t)ROM16(offs + 2 * ofs_i++), acc);
		}
		if (kept & 7) {                                  /* partial group, padded with colour 0 */
			acc <<= 4 * (8 - (kept & 7));
			RAM32_W(dst + (int16_t)ROM16(offs + 2 * ofs_i++), acc);
		}
		for (int g = (kept + 7) / 8; g < groups; g++)   /* the rest of the row is empty */
			RAM32_W(dst + (int16_t)ROM16(offs + 2 * ofs_i++), 0);
		out_rows++;
	}
	for (int y = out_rows; y < h; y++)                  /* dropped rows: empty at the bottom */
		for (int g = 0; g < groups; g++)
			RAM32_W(dst + (int16_t)ROM16(offs + 2 * ofs_i++), 0);
	ctx->d[7] = acc;
	ctx->a[4] = offs + 2 * (uint32_t)ofs_i;
	ctx->a[1] = CHUNKY_BUF + (uint32_t)(w * h);
}

/* charges: difftest averages of the originals */
#define COST_PACK     94000
#define COST_SCALE_T 143852
#define COST_SCALE_W 122489
HLE_FUNC(0x00D3F0, frame_pack_48x80, COST_PACK) { frame_pack(ctx, 0x00BC70); }
HLE_FUNC(0x00D42C, frame_pack_80x48, COST_PACK) { frame_pack(ctx, 0x00C030); }
HLE_FUNC(0x00D468, frame_scale_48x80, COST_SCALE_T) { frame_scale(ctx, 48, 80, 0x00BC70); }
HLE_FUNC(0x00DA6A, frame_scale_80x48, COST_SCALE_W) { frame_scale(ctx, 80, 48, 0x00C030); }

/* ================================================================== sprite_build */
/* Piece layouts by frame size (frame header +4 width, +5 height). Each piece is one
 * hardware sprite; the tables hold deltas that are accumulated onto the position:
 * dx (unflipped), dx (h-flipped), dy, tile offset in the slot's VRAM, SAT size word. */
typedef struct { uint32_t dx, dx_flip, dy, tile, size; int pieces; int16_t y_adjust; } piece_layout;

static piece_layout layout_for(uint32_t frame)
{
	uint8_t w = rd8(frame + 4), h = rd8(frame + 5);
	if (w == 0x70) return (piece_layout){ 0xFC86C, 0xFC87C, 0xFC88C, 0xFC968, 0xFC9C0, 8, 0 };  /* 112x64 */
	if (w == 0x40) return (piece_layout){ 0xFC92C, 0xFC934, 0xFC93C, 0xFC984, 0xFC9DC, 4, 0 };  /* 64x64 */
	if (w == 0x30) return (piece_layout){ 0xFC8C0, 0xFC8CC, 0xFC8D8, 0xFC950, 0xFC9A8, 6, 0 };  /* 48x80 */
	if (w != 0x20) return (piece_layout){ 0xFC89C, 0xFC8A8, 0xFC8B4, 0xFC944, 0xFC99C, 6, 0 };  /* 80x48 */
	if (h == 0x50) return (piece_layout){ 0xFC8E4, 0xFC8F0, 0xFC8FC, 0xFC95C, 0xFC9B4, 6, 0 };  /* 32x80 */
	/* 32 wide, other height: six 32x32 pieces stacked, 76 px higher (table never used by
	 * the frames in the ROM) */
	return (piece_layout){ 0xFC908, 0xFC914, 0xFC920, 0xFC978, 0xFC9D0, 6, 0x4C };
}

typedef struct {        /* registers sprite_build hands back to its caller (exact contract) */
	uint32_t d[8], a[8];
	unsigned x;         /* X flag */
} regs;

/* $00AD16: latch the slot's new frame if its DMA happened, then draw its pieces.
 * In: slot d4, next SAT link d5, SAT write pointer a0, a2 = &ACTOR_SHOWN_FRAME(slot),
 * a3 = &ACTOR_FRAME(slot), a5 = &SLOT_VRAM_TBL[slot]. */
static void actor_draw(regs *r)
{
	int slot = (int)(r->d[4] & 0xFFFF);
	uint16_t vram = ROM16(r->a[5]);
	setw(&r->d[6], vram >> 5);                  /* first tile of the slot */
	r->x = (vram >> 4) & 1;
	if (!bit8(ACTORS_DMA_DONE, slot) && !bit8(ACTORS_NEED_SCALE, slot) && !bit8(ACTORS_TILES_READY, slot)) {
		/* the frame decoded last is in VRAM now: show it */
		set8(ACTORS_DMA_DONE, slot);
		RAM32_W(ACTOR_SHOWN_FRAME(slot), RAM32(ACTOR_NEXT_FRAME(slot)));
		RAM16(ACTOR_SHOWN_DEPTH_X(slot)) = RAM16(ACTOR_DEPTH_X(slot));
		RAM16(ACTOR_SHOWN_DEPTH_Y(slot)) = RAM16(ACTOR_DEPTH_Y(slot));
		RAM16(ACTOR_SHOWN_HOT_X(slot)) = RAM16(ACTOR_HOT_X(slot));
		RAM16(ACTOR_SHOWN_HOT_Y(slot)) = RAM16(ACTOR_HOT_Y(slot));
	}
	if (RAM8(ACTOR_SHOWN_FRAME(slot)) & 0x80)
		return;                                 /* no frame yet */
	uint32_t frame = RAM32(ACTOR_SHOWN_FRAME(slot));
	r->a[1] = frame;
	/* screen position of the hot spot (world pixel - camera), culled 80 px outside */
	uint16_t x = RAM16(ACTOR_X(slot)), y = RAM16(ACTOR_Y(slot));
	uint16_t cx = RAM16(CAMERA_X), cy = RAM16(CAMERA_Y);
	r->x = x_sub16(x, cx);
	x = (uint16_t)(x - cx);
	r->x = x_sub16(y, cy);
	y = (uint16_t)(y - cy);
	setw(&r->d[2], x);
	setw(&r->d[3], y);
	if ((int16_t)x < -0x50 || (int16_t)y < -0x50 || (int16_t)x > 0x150 || (int16_t)y > 0xE8)
		return;
	/* SAT coordinates are +128 (y +144: the room view starts at screen line 16) */
	r->x = x_add16(x, 0x80);
	x = (uint16_t)(x + 0x80);
	r->x = x_add16(y, 0x90);
	y = (uint16_t)(y + 0x90);
	/* minus the hot spot: depth-scaled actors use the latched scaled one */
	uint16_t hx, hy;
	if (bit8(ACTORS_DEPTH_ON, slot)) {
		hx = RAM16(ACTOR_SHOWN_HOT_X(slot));
		hy = RAM16(ACTOR_SHOWN_HOT_Y(slot));
	} else {
		hx = rd16(frame);
		hy = rd16(frame + 2);
	}
	r->x = x_sub16(x, hx);
	x = (uint16_t)(x - hx);
	r->x = x_sub16(y, hy);
	y = (uint16_t)(y - hy);

	piece_layout L = layout_for(frame);
	if (L.y_adjust) {
		r->x = x_sub16(y, (uint16_t)L.y_adjust);
		y = (uint16_t)(y - L.y_adjust);
	}
	uint16_t attr = rd16(frame + 10);           /* palette, priority, bit 11 = h-flip */
	uint32_t dx = (attr & 0x0800) ? L.dx_flip : L.dx;
	uint32_t a0 = r->a[0];
	uint16_t link = (uint16_t)r->d[5], d0 = 0, tile = (uint16_t)r->d[6];
	for (int p = 0; p < L.pieces; p++) {
		x = (uint16_t)(x + ROM16(dx + 2 * p));
		y = (uint16_t)(y + ROM16(L.dy + 2 * p));
		RAM16(a0) = y;
		RAM16(a0 + 6) = x;
		d0 = (uint16_t)((tile + ROM16(L.tile + 2 * p)) | attr);
		if (bit8(ACTORS_BEHIND, slot))
			d0 |= 0x8000;                       /* sprite priority: in front of the high plane */
		RAM16(a0 + 4) = d0;
		d0 = link | ROM16(L.size + 2 * p);      /* size bits + link to the next sprite */
		RAM16(a0 + 2) = d0;
		a0 += 8;
		r->x = x_add16(link, 1);
		link++;
	}
	r->a[0] = a0;
	setw(&r->d[0], d0);
	r->d[1] = 0xFFFF;                           /* moveq #n,d1 ... dbra */
	setw(&r->d[2], x);
	setw(&r->d[3], y);
	setw(&r->d[5], link);
	r->a[2] = dx + 2 * (uint32_t)L.pieces;
	r->a[3] = L.dy + 2 * (uint32_t)L.pieces;
	r->a[4] = L.tile + 2 * (uint32_t)L.pieces;
	r->a[5] = L.size + 2 * (uint32_t)L.pieces;
}

/* $00A984 sprite_build: write the sprite attribute table to SAT_BUF, in priority order:
 *   1. the pointer: the crosshair in the room, or the cell cursor on the verb bar
 *   2. the effect sprite ($09E8 bit 4: walks the (x, y) table at FX_SPRITE_PTR, blinking)
 *   3. the inventory highlight (8 sprites, VERB_STATE bit 2)
 *   4. four fixed sprites at $FF09EA bit 0
 *   5. four 8x32 masking sprites at the verb bar's lines (x = 1 then x = 0): the VDP stops
 *      drawing lower-priority sprites on lines where an x = 0 sprite follows another one,
 *      so actors never cover the verb bar
 *   6. the actors that are active and not frozen, nearest (largest y) first, each only if
 *      it was drawn last frame or its tiles were just DMA'd (ANIM_LOOPED)
 * The last entry's link is set to 0. */
HLE_FUNC(0x00A984, sprite_build, 5279)
{
	regs R;
	for (int i = 0; i < 8; i++) { R.d[i] = ctx->d[i]; R.a[i] = ctx->a[i]; }
	R.x = (ctx->sr & CC_X) ? 1 : 0;
	uint32_t *d = R.d, *a = R.a;

	RAM8(ACTORS_DRAWN_PREV) = RAM8(ACTORS_DRAWN);
	RAM8(ACTORS_DRAWN) = 0;
	d[5] = 1;
	a[0] = SAT_BUF;

	/* ---- 1. pointer */
	if (RAM8(VBLANK_FLAGS) & VBF_POINTER) {
		setw(&d[0], RAM16(POINTER_Y));
		if ((int16_t)d[0] < 0xA8) {
			/* crosshair in the room view */
			RAM16(POINTER_DX) = 2;
			RAM16(POINTER_DY) = 2;
			R.x = x_add16(RAM16(POINTER_X), 0x80);
			setw(&d[0], RAM16(POINTER_X) + 0x80);
			RAM16(a[0] + 6) = (uint16_t)d[0];
			R.x = x_add16(RAM16(POINTER_Y), 0x80);
			setw(&d[0], RAM16(POINTER_Y) + 0x80);
			RAM16(a[0]) = (uint16_t)d[0];
			setw(&d[0], RAM16(UI_TILE_BASE_B));
			RAM16(a[0] + 4) = (uint16_t)d[0];
			RAM16(a[0] + 2) = 0x501;            /* 2x2 */
			R.x = x_add16(d[5], 1);
			setw(&d[5], d[5] + 1);
			a[0] += 8;
		} else if (RAM8(VERB_BAR_SLIDE) & 0x80) {
			/* on the verb bar (and the bar is not sliding): snap to the 40-px cell */
			bool inv = RAM8(VBLANK_FLAGS) & VBF_INVENTORY;
			setw(&d[1], 0x128);
			if ((int16_t)d[0] >= 0xC0) {
				setw(&d[1], 0x140);
			} else if (!inv) {
				R.x = 0;
				setw(&d[1], d[1] + 8);
			}
			RAM16(a[0]) = (uint16_t)d[1];
			setw(&d[0], RAM16(POINTER_X));
			d[1] = 0;
			for (;;) {
				R.x = x_sub16(d[0], 0x28);
				setw(&d[0], d[0] - 0x28);
				if (d[0] & 0x8000)
					break;
				R.x = x_add16(d[1], 0x28);
				setw(&d[1], d[1] + 0x28);
			}
			if (!inv) {
				R.x = x_add16(d[1], 8);
				setw(&d[1], d[1] + 8);
				if ((int16_t)d[1] > 0xA8)
					setw(&d[1], 0xA8);
			} else {
				if ((int16_t)d[1] > 0xC8)
					setw(&d[1], 0xC8);
				if (!(d[1] & 0xFFFF))
					setw(&d[1], 0x28);
			}
			if (RAM16(POINTER_DX) != 2) {
				RAM16(POINTER_X) = (uint16_t)d[1];
				R.x = x_add16(RAM16(POINTER_X), 0x14);
				RAM16(POINTER_X) += 0x14;
			}
			R.x = x_add16(d[1], 0x88);
			setw(&d[1], d[1] + 0x88);
			RAM16(a[0] + 6) = (uint16_t)d[1];
			RAM16(a[0] + 2) = inv ? 0xE01 : 0xD01;  /* 4x3 / 4x2 */
			RAM16(a[0] + 4) = RAM16(inv ? UI_TILE_BASE_D : UI_TILE_BASE_C);
			a[0] += 8;
			R.x = x_add16(d[5], 1);
			setw(&d[5], d[5] + 1);
		}
	}

	/* ---- 2. effect sprite */
	if (RAM8(GAME_FLAGS_09E8) & 0x10) {
		a[1] = RAM32(FX_SPRITE_PTR);
		RAM16(a[0]) = rd16(a[1] + 0x48);
		RAM16(a[0] + 6) = rd16(a[1]);
		a[1] += 2;
		RAM32_W(FX_SPRITE_PTR, a[1]);
		RAM16(a[0]) += 0x90;
		R.x = x_add16(RAM16(a[0] + 6), 0x90);
		RAM16(a[0] + 6) += 0x90;
		if (RAM8(GAME_FLAGS_09E8) & 0x20) {     /* hold on the last entry */
			RAM32_W(FX_SPRITE_PTR, a[1]);
			if (a[1] == 0x032406) {
				R.x = RAM32(FX_SPRITE_PTR) < 2;
				RAM32_W(FX_SPRITE_PTR, RAM32(FX_SPRITE_PTR) - 2);
			}
		}
		setw(&d[0], d[5] | 0x500);
		RAM16(a[0] + 2) = (uint16_t)d[0];
		R.x = RAM8(FX_BLINK_TIMER) < 1;
		RAM8(FX_BLINK_TIMER)--;
		if (RAM8(FX_BLINK_TIMER) & 0x80) {
			RAM8(FX_BLINK_TIMER) = 7;
			RAM8(GAME_FLAGS_09E8) ^= 0x40;
		}
		d[0] = (RAM8(GAME_FLAGS_09E8) & 0x40) ? 4 : 0;     /* blink between two tiles */
		R.x = x_add16(d[0], RAM16(FX_SPRITE_TILE));
		setw(&d[0], d[0] + RAM16(FX_SPRITE_TILE));
		RAM16(a[0] + 4) = (uint16_t)d[0];
		R.x = x_add16(d[5], 1);
		setw(&d[5], d[5] + 1);
		a[0] += 8;
	}

	/* ---- 3. inventory highlight: a row of 8 sprites 32 px apart */
	if (RAM8(VERB_STATE) & 0x04)
		setw(&d[2], RAM16(INV_HL_X));
	if ((RAM8(VERB_STATE) & 0x04) && !(RAM16(INV_HL_X) & 0x8000)) {
		uint16_t hx = RAM16(INV_HL_X), hy = RAM16(INV_HL_Y);
		R.x = (hx >> 13) & 1;
		setw(&d[2], (uint16_t)(hx << 3));
		R.x = x_add16(d[2], 0x80);
		setw(&d[2], d[2] + 0x80);
		setw(&d[3], (uint16_t)(hy << 3));
		R.x = x_add16(d[3], 0x128);
		setw(&d[3], d[3] + 0x128);
		d[1] = 7;
		for (int k = 0; k < 8; k++) {
			RAM16(a[0] + 4) = RAM16(UI_TILE_BASE_D);
			setw(&d[0], d[5] | 0xC00 | RAM16(INV_HL_ATTR));
			RAM16(a[0] + 2) = (uint16_t)d[0];
			RAM16(a[0] + 6) = (uint16_t)d[2];
			RAM16(a[0]) = (uint16_t)d[3];
			R.x = x_add16(d[5], 1);
			setw(&d[5], d[5] + 1);
			a[0] += 8;
			R.x = x_add16(d[2], 0x20);
			setw(&d[2], d[2] + 0x20);
		}
		setw(&d[1], 0xFFFF);
	}

	/* ---- 4. four fixed 32x8 sprites on line $130 */
	if (RAM8(GAME_FLAGS_09EA) & 0x01) {
		static const uint16_t tile[4] = { 0x518, 0x51C, 0x520, 0x524 }, xs[4] = { 0xB0, 0xD0, 0x110, 0x130 };
		setw(&d[0], d[5] | 0xC00);
		setw(&d[5], d[5] + 4);
		for (int k = 0; k < 4; k++) {
			if (k) setw(&d[0], d[0] + 1);
			RAM16(a[0]) = 0x130;
			RAM16(a[0] + 2) = (uint16_t)d[0];
			RAM16(a[0] + 4) = tile[k];
			RAM16(a[0] + 6) = xs[k];
			a[0] += 8;
		}
		R.x = x_add16(d[0] - 1, 1);
	}

	/* ---- 5. verb-bar masking sprites: (y $128, x 1), (y $128, x 0), (y $148, x 1), (y $148, x 0) */
	setw(&d[0], d[5] | 0x300);                  /* 8x32 */
	for (int k = 0; k < 4; k++) {
		if (k) setw(&d[0], d[0] + 1);
		RAM16(a[0]) = k < 2 ? 0x128 : 0x148;
		RAM16(a[0] + 2) = (uint16_t)d[0];
		RAM32_W(a[0] + 4, (k & 1) ? 0 : 1);
		a[0] += 8;
	}
	R.x = x_add16(d[5], 4);
	setw(&d[5], d[5] + 4);

	/* ---- 6. actors, nearest first */
	RAM8(ACTORS_DRAW_TODO) = (uint8_t)(~RAM8(ACTORS_FROZEN) & RAM8(ACTORS_ACTIVE));
	set_b(&d[0], RAM8(ACTORS_ACTIVE));
	uint16_t saved_link = (uint16_t)d[5];       /* move.w d5,-(sp) */
	while (RAM8(ACTORS_DRAW_TODO)) {
		/* pick the remaining slot with the largest y (ties: the higher slot); with
		 * $09E9 bit 2 set simply the highest remaining slot */
		d[1] = 5;
		setw(&d[4], 5);
		setw(&d[0], 0x8001);
		a[1] = ACTOR_Y(5);
		for (;;) {
			int s = (int)(d[1] & 0xFFFF);
			if (bit8(ACTORS_DRAW_TODO, s)) {
				if (RAM8(GAME_FLAGS_09E9) & 0x04) {
					setw(&d[4], d[1]);
					break;
				}
				if ((int16_t)RAM16(a[1]) > (int16_t)d[0]) {
					setw(&d[4], d[1]);
					setw(&d[0], RAM16(a[1]));
				}
			}
			a[1] -= 4;
			setw(&d[1], d[1] - 1);
			if ((d[1] & 0xFFFF) == 0xFFFF)
				break;
		}
		int slot = (int)(d[4] & 7);
		clr8(ACTORS_DRAW_TODO, slot);
		if (!bit8(ACTORS_DRAWN_PREV, slot) && !bit8(ACTORS_ANIM_LOOPED, slot))
			continue;                           /* not on screen yet: wait for its first DMA */
		set8(ACTORS_DRAWN, slot);
		setw(&d[0], (d[4] & 0xFFFF) * 2);
		setw(&d[1], (d[4] & 0xFFFF) * 4);
		a[5] = SLOT_VRAM_TBL + (int16_t)(uint16_t)d[0];
		a[2] = ACTOR_SHOWN_FRAME(0) + (int16_t)(uint16_t)d[1];
		a[3] = ACTOR_FRAME(0) + (int16_t)(uint16_t)d[0];
		R.x = 0;
		actor_draw(&R);
	}
	setw(&d[0], saved_link);
	if (a[0] == SAT_BUF) {                      /* (unreachable: the masking sprites are always there) */
		RAM32_W(SAT_BUF, 0);
		RAM32_W(TITLE_STRIP_IDX, 0);
		cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	} else {
		RAM8(a[0] - 5) = 0;                     /* end of the sprite list */
		cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	}
	for (int i = 0; i < 7; i++) { ctx->d[i] = R.d[i]; ctx->a[i] = R.a[i]; }
	ctx->d[7] = R.d[7];
	cc_set(ctx, CC_X, R.x ? CC_X : 0);
}

/* ================================================================== actor_dma */
/* $00AF24 actor_dma (VBlank, from vblank_main): upload the tiles of at most one slot,
 * then build and upload the sprite table and count down the script timers.
 * Slots are tried in order 0..5; the first active slot with TILES_READY is DMA'd, but only
 * while the raster is in lines $E0..$F0 (HV counter $E000..$F000), i.e. early in VBlank,
 * so that the DMA fits. D6 keeps the HV value read (C reads it at entry, earlier than the
 * 68k): it is dead - vblank_main's following callees (meters_draw, object_timers,
 * player_update, pad_read) all write D6 before reading it, and the VBlank vector restores
 * it; difftest ignores D6 (dt_ignores). If the first active slot finds the raster outside the window, no
 * tiles go up this frame. In follow mode ($09E8 bit 3) Shaggy's partner slot and any slot
 * entered with D0.w = 0 skip the raster test.
 * After a DMA: DMA_DONE cleared (sprite_build latches the frame next) and ANIM_LOOPED set. */
HLE_FUNC(0x00AF24, actor_dma, 1500)
{
	uint8_t active = RAM8(ACTORS_ACTIVE);
	uint8_t ready = RAM8(ACTORS_TILES_READY);
	uint8_t looped = RAM8(ACTORS_ANIM_LOOPED);
	ctx->d[2] = 0;
	ctx->a[1] = SLOT_VRAM_TBL;
	ctx->a[2] = ACTOR_SRC(0);
	ctx->a[3] = ACTOR_DMA_WORDS(0);
	set_w(&ctx->d[3], 0xD800);
	set_b(&ctx->d[4], active);
	set_b(&ctx->d[5], ready);
	set_b(&ctx->d[7], looped);
	for (int slot = 0; slot < 6; slot++) {
		set_w(&ctx->d[2], (uint32_t)slot);
		if (active & (1u << slot)) {
			bool raster_test = true;
			if (RAM8(GAME_FLAGS_09E8) & 0x08)
				raster_test = !(slot == RAM16(FOLLOW_SLOT) || (ctx->d[0] & 0xFFFF) == 0);
			if (raster_test) {
				uint16_t hv = bus_r16(0xC00008);
				set_w(&ctx->d[6], hv);
				if ((int16_t)hv < (int16_t)0xE000 || (int16_t)hv > (int16_t)0xF000)
					break;
			}
			if (ready & (1u << slot)) {
				ctx->a[0] = RAM32(ACTOR_SRC(slot));
				set_w(&ctx->d[1], RAM16(ACTOR_DMA_WORDS(slot)));
				set_w(&ctx->d[0], ROM16(SLOT_VRAM_TBL + 2 * slot));
				ready &= (uint8_t)~(1u << slot);
				set_b(&ctx->d[5], ready);
				hle_call68k(ctx, VDP_DMA_VRAM);
				clr8(ACTORS_DMA_DONE, slot);
				looped |= (uint8_t)(1u << slot);
				break;
			}
		}
		ctx->a[1] += 2;
		ctx->a[2] += 4;
		ctx->a[3] += 2;
		if (slot == 5)
			set_w(&ctx->d[2], 6);
	}
	set_b(&ctx->d[5], ready);
	set_b(&ctx->d[7], looped);
	RAM8(ACTORS_TILES_READY) = ready;
	RAM8(ACTORS_ANIM_LOOPED) = looped;
	sprite_build(ctx);
	ctx->a[0] = SAT_BUF;
	set_w(&ctx->d[1], 0xC8);                    /* 80 sprites * 4 words */
	set_w(&ctx->d[0], 0xD800);
	hle_call68k(ctx, VDP_DMA_VRAM);
	/* script timers: count down the positive ones */
	for (int slot = 0; slot < 6; slot++) {
		uint16_t t = RAM16(ACTOR_TIMER(slot));
		cc_logic16(ctx, t);
		if ((int16_t)t > 0)
			RAM16(ACTOR_TIMER(slot)) = (uint16_t)cc_sub(ctx, t, 1, 16, true);
	}
	ctx->a[0] = ACTOR_TIMER(6);
	ctx->d[0] = 0xFFFF;
	/* measured total of the original (incl. sprite_build) minus the nested DMA calls */
	ctx->cycles = 6977 - hle_nested_cycles();
	if (ctx->cycles < 500)
		ctx->cycles = 500;
}

/* ================================================================== actor_animate */
/* Animation script commands: (u16 op, u16 arg) pairs. */
enum {
	AOP_FRAME = 2,    /* show frame `arg` (offset of its header in the set); yields */
	AOP_GOTO = 3,     /* continue at `arg` (loop); with PATH_ONCE: like AOP_ANIM once */
	AOP_DELAY = 4,    /* frames per animation frame */
	AOP_WAIT = 5,     /* pause `arg` frames; yields */
	AOP_MOVE = 6,     /* move the actor by (s8 low byte, s8 high byte) pixels */
	AOP_END = 9,      /* ANIM_DONE; the script stops here */
	AOP_ANIM = 0x0D,  /* set ACTOR_ANIM = arg, clear ANIM_LOOPED (the script continues) */
	AOP_SOUND = 0x11, /* sound_play(arg), or sound_stop(arg & $7FFF) if bit 15 is set */
};

/* nested 68k call with the registers the original passes (the rest is saved by movem) */
static void call68k(hle_ctx *ctx, uint32_t addr, uint32_t d0, uint32_t a0, uint32_t a1, uint32_t a2)
{
	hle_ctx t = *ctx;
	t.d[0] = d0;
	t.a[0] = a0;
	t.a[1] = a1;
	t.a[2] = a2;
	t.a[7] = ctx->a[7] - 60;                    /* below the movem.l d0-d7/a0-a6 frame */
	hle_call68k(&t, addr);
	ctx->sr = (uint16_t)((ctx->sr & 0xFF00) | (t.sr & 0xFF));
}

/* frame header -> tile DMA length in words (number of tiles * 16) */
static uint16_t frame_dma_words(uint32_t frame)
{
	uint8_t w = rd8(frame + 4), h = rd8(frame + 5);
	if (w == 0x70) return 0x700;
	if (w == 0x40) return 0x400;
	if (w == 0x20) return h == 0x50 ? 0x280 : 0x200;
	return 0x3C0;
}

/* Run slot's script until it yields. Returns after a frame was started, a wait, the end. */
static void actor_run_script(hle_ctx *ctx, int slot)
{
	uint32_t set = RAM32(ACTOR_ANIMSET(slot));
	uint16_t frame_off;
	if (bit8(ACTORS_ANIM_NEW, slot)) {
		/* a new animation was requested; not while the last frame is still in the pipeline */
		if (bit8(ACTORS_TILES_READY, slot) || bit8(ACTORS_NEED_SCALE, slot))
			return;
		clr8(ACTORS_ANIM_DONE, slot);
		clr8(ACTORS_ANIM_LOOPED, slot);
		RAM16(ACTOR_SCRIPT_POS(slot)) = rd16(set + (uint16_t)(rd16(set + 2) + 2 * RAM16(ACTOR_ANIM(slot))));
		RAM16(ACTOR_TIMER(slot)) = 0xFFFF;
		clr8(ACTORS_ANIM_NEW, slot);
	}
	if (bit8(ACTORS_ANIM_DONE, slot)) {
		/* stopped on its last frame: only redraw it when the depth changed */
		if (!bit8(ACTORS_DEPTH_NEW, slot))
			return;
		frame_off = RAM16(ACTOR_FRAME(slot));
		clr8(ACTORS_DEPTH_NEW, slot);
	} else {
		for (;;) {
			uint16_t pos = RAM16(ACTOR_SCRIPT_POS(slot));
			RAM16(ACTOR_SCRIPT_POS(slot)) = (uint16_t)(pos + 4);
			uint16_t op = rd16(set + pos), arg = rd16(set + (uint16_t)(pos + 2));
			if (op == AOP_DELAY) {
				RAM16(ACTOR_DELAY(slot)) = arg;
			} else if (op == AOP_WAIT) {
				RAM16(ACTOR_TIMER(slot)) = arg;
				return;
			} else if (op == AOP_GOTO && !bit8(ACTORS_PATH_ONCE, slot)) {
				RAM16(ACTOR_SCRIPT_POS(slot)) = arg;
			} else if (op == AOP_GOTO || op == AOP_ANIM) {
				if (op == AOP_GOTO)
					clr8(ACTORS_PATH_ONCE, slot);
				clr8(ACTORS_ANIM_LOOPED, slot);
				RAM16(ACTOR_ANIM(slot)) = arg;
			} else if (op == AOP_FRAME) {
				RAM16(ACTOR_FRAME(slot)) = arg;
				if (slot == 0 || slot == RAM16(FOLLOW_SLOT))
					set8(ACTORS_FOLLOW_SYNC, slot);
				frame_off = arg;
				break;
			} else if (op == AOP_END) {
				set8(ACTORS_ANIM_DONE, slot);
				return;
			} else if (op == AOP_MOVE) {
				RAM16(ACTOR_X(slot)) += (uint16_t)(int8_t)(arg & 0xFF);
				RAM16(ACTOR_Y(slot)) += (uint16_t)((int16_t)arg >> 8);
			} else if (op == AOP_SOUND) {
				if (arg & 0x8000)
					call68k(ctx, SOUND_STOP, arg & 0x7FFF, ctx->a[0], ctx->a[1], ctx->a[2]);
				else
					call68k(ctx, SOUND_PLAY, arg, ctx->a[0], ctx->a[1], ctx->a[2]);
			}
			/* other ops: ignored */
		}
	}
	/* start decoding the frame */
	uint32_t frame = set + frame_off;
	uint32_t rle = rd32(frame + 6) + set;
	RAM16(ACTOR_DMA_WORDS(slot)) = frame_dma_words(frame);
	if (bit8(ACTORS_DEPTH_ON, slot)) {
		/* chunky frame: decoded and scaled in the second pass (actor_scale_frames) */
		if (bit8(ACTORS_TILES_READY, slot))
			return;                             /* previous tiles not uploaded yet */
		RAM32_W(ACTOR_SRC(slot), rle);
		set8(ACTORS_NEED_SCALE, slot);
	} else {
		/* tiles frame: decode straight into the slot buffer */
		uint32_t buf = ROM32(SLOT_BUFFER_TBL + 4 * slot);
		RAM32_W(ACTOR_SRC(slot), buf);
		call68k(ctx, RLE_DECOMPRESS, ctx->d[0], rle, rle, buf);
		set8(ACTORS_TILES_READY, slot);
	}
	RAM32_W(ACTOR_NEXT_FRAME(slot), frame);
	RAM16(ACTOR_TIMER(slot)) = RAM16(ACTOR_DELAY(slot));
}

/* Second pass: decode, depth-scale and pack the chunky frames of NEED_SCALE slots. */
static void actor_scale_frame(hle_ctx *ctx, int slot)
{
	uint32_t buf = ROM32(SLOT_BUFFER_TBL + 4 * slot);
	uint32_t dst = CHUNKY_BUF;
	/* Shaggy's 112-px frames would go to $FF5000 (the scalers still read $FF6000) */
	if (slot == 0 && rd8(RAM32(ACTOR_ANIMSET(0)) + RAM16(ACTOR_FRAME(0)) + 4) == 0x70)
		dst = 0xFF5000;
	call68k(ctx, RLE_DECOMPRESS, ctx->d[0], RAM32(ACTOR_SRC(slot)), buf, dst);
	uint16_t dx = RAM16(ACTOR_DEPTH_X(slot)), dy = RAM16(ACTOR_DEPTH_Y(slot));
	RAM16(SCALE_DEPTH_X) = dx;
	RAM16(SCALE_DEPTH_Y) = dy;

	/* scaled hot spot: the frame shrinks towards its top left (right edge if h-flipped) */
	uint32_t frame = RAM32(ACTOR_ANIMSET(slot)) + RAM16(ACTOR_FRAME(slot));
	uint16_t keep_x = (uint16_t)(0x100 - dx), keep_y = (uint16_t)(0x100 - dy);
	if (rd8(frame + 10) & 0x08) {
		uint16_t w = rd8(frame + 4);
		uint16_t right = (uint16_t)(w - rd16(frame));
		RAM16(ACTOR_HOT_X(slot)) = (uint16_t)(w - (uint16_t)((uint16_t)(right * keep_x) >> 8));
	} else {
		RAM16(ACTOR_HOT_X(slot)) = (uint16_t)((uint16_t)(rd16(frame) * keep_x) >> 8);
	}
	uint16_t hy = (uint16_t)((uint16_t)(rd16(frame + 2) * keep_y) >> 8);
	RAM16(ACTOR_HOT_Y(slot)) = hy;

	hle_ctx t = *ctx;
	t.a[0] = buf;
	t.a[7] = ctx->a[7] - 76;
	bool tall = rd8(frame + 4) == 0x30;
	/* called as C: charge what the 68k scaler would have cost */
	if ((RAM8(GAME_FLAGS_09E9) & 0x08) && (int16_t)hy < 2) {
		if (tall) frame_pack_48x80(&t); else frame_pack_80x48(&t);
		ctx->cycles += COST_PACK;
	} else {
		if (tall) frame_scale_48x80(&t); else frame_scale_80x48(&t);
		ctx->cycles += tall ? COST_SCALE_T : COST_SCALE_W;
	}
	RAM32_W(ACTOR_SRC(slot), buf);
	if ((RAM8(GAME_FLAGS_09E8) & 0x08) && (slot == 0 || slot == RAM16(FOLLOW_SLOT)))
		return;                                 /* follow mode: released together below */
	set8(ACTORS_TILES_READY, slot);
	clr8(ACTORS_NEED_SCALE, slot);
}

/* $00B0B8 actor_animate: one step of every actor's animation (called once per main-loop
 * pass and by every script command that waits for an animation).
 * 1. Follow mode with $09E9 bit 3: when Shaggy starts an animation, slot 2 starts the
 *    matching one (FOLLOW_ANIM_TBL) with Shaggy's timer.
 * 2. For slots 5..0 that are active, whose timer ran out, and that are on screen (or
 *    Scooby, or have a new animation / depth): run the script (actor_run_script).
 * 3. For slots 5..0 with NEED_SCALE: decode + scale (actor_scale_frame).
 * 4. Follow mode: once Shaggy and the partner both have a new frame, place the partner at
 *    Shaggy's attachment point (frame header +$0E/+$10, depth-scaled), release both frames
 *    and wait (spin, interrupts on) until VBlank has uploaded both.
 * Register contract: movem.l d0-d7/a0-a6 saves everything; the final flags are dead (every
 * caller's next instruction is a btst/tst/move/bset/bra or rts - with the same caller
 * pattern - that redefines the flags it tests), so difftest ignores the CCR. */
HLE_FUNC(0x00B0B8, actor_animate, 1700)
{
	if ((RAM8(GAME_FLAGS_09E9) & 0x08) && bit8(ACTORS_ANIM_NEW, 0)) {
		RAM16(ACTOR_TIMER(2)) = RAM16(ACTOR_TIMER(0));
		RAM16(ACTOR_ANIM(2)) = ROM16(FOLLOW_ANIM_TBL + (uint16_t)(RAM16(ACTOR_ANIM(0)) * 2));
		set8(ACTORS_ANIM_NEW, 2);
	}
	for (int slot = 5; slot >= 0; slot--) {
		if (!bit8(ACTORS_ACTIVE, slot) || S16(ACTOR_TIMER(slot)) > 0)
			continue;
		if ((RAM8(GAME_FLAGS_09E8) & 0x08) && bit8(ACTORS_FOLLOW_SYNC, slot))
			continue;
		if (!bit8(ACTORS_ANIM_NEW, slot) && !bit8(ACTORS_DEPTH_NEW, slot) && slot != 1) {
			int16_t x = (int16_t)(RAM16(ACTOR_X(slot)) - RAM16(CAMERA_X));
			int16_t y = (int16_t)(RAM16(ACTOR_Y(slot)) - RAM16(CAMERA_Y));
			if (x < -0x50 || x > 0x150 || y < -0x50 || y > 0xE8)
				continue;                       /* off screen: the animation pauses */
		}
		actor_run_script(ctx, slot);
	}
	for (int slot = 5; slot >= 0; slot--) {
		if (!bit8(ACTORS_NEED_SCALE, slot))
			continue;
		if ((RAM8(GAME_FLAGS_09E8) & 0x08) && (slot == 0 || slot == RAM16(FOLLOW_SLOT))) {
			int partner = RAM16(FOLLOW_SLOT) & 7;
			if (!bit8(ACTORS_FOLLOW_SYNC, 0) || !bit8(ACTORS_FOLLOW_SYNC, partner))
				continue;
		}
		actor_scale_frame(ctx, slot);
	}
	if ((RAM8(GAME_FLAGS_09E8) & 0x08) && bit8(ACTORS_FOLLOW_SYNC, 0)
	    && bit8(ACTORS_FOLLOW_SYNC, RAM16(FOLLOW_SLOT) & 7)) {
		uint16_t partner = RAM16(FOLLOW_SLOT);
		RAM8(ACTORS_FOLLOW_SYNC) = 0;
		uint32_t frame = RAM32(ACTOR_ANIMSET(0)) + RAM16(ACTOR_FRAME(0));
		int32_t ox = (int16_t)rd16(frame + 0x0E), oy = (int16_t)rd16(frame + 0x10);
		ox = (ox * (int16_t)(0xFF - RAM16(ACTOR_DEPTH_X(0)))) >> 8;
		oy = (oy * (int16_t)(0xFF - RAM16(ACTOR_DEPTH_Y(0)))) >> 8;
		uint16_t px = (uint16_t)(RAM16(ACTOR_X(0)) - (uint16_t)ox);
		uint16_t py = (uint16_t)(RAM16(ACTOR_Y(0)) - (uint16_t)oy);
		RAM16(ACTOR_X(0) + 4 * (uint16_t)(partner << 0)) = px;
		RAM16(ACTOR_Y(0) + 4 * (uint16_t)(partner << 0)) = py;
		clr8(ACTORS_NEED_SCALE, 0);
		clr8(ACTORS_NEED_SCALE, partner);
		set8(ACTORS_TILES_READY, 0);
		set8(ACTORS_TILES_READY, partner);
		/* The original now spins at $00B5A4 with interrupts on until VBlank's actor_dma has
		 * uploaded both. Rebuild its movem frame and let the 68k wait and return. */
		for (int i = 6; i >= 0; i--) push32(ctx, ctx->a[i]);
		for (int i = 7; i >= 0; i--) push32(ctx, ctx->d[i]);
		ctx->d[0] = partner;
		ctx->sr = 0x2000 | (ctx->sr & 0x1F);
		hle_jump(ctx, 0x00B5A4);
	}
}

/* ================================================================== actors_init */
/* $00801A actors_init (game start, after the mystery is loaded): Shaggy uses animation set
 * $032700, Scooby $0642D0; slots 0 and 1 active, depth-scaled, animation 0 requested; all
 * timers -1 and no frames shown. Ends with `bsr actor_animate; rts` - kept in 68k (tail
 * jump to the bsr) because actor_animate can block in follow mode. */
HLE_FUNC(0x00801A, actors_init, 500)
{
	RAM32_W(ACTOR_ANIMSET(0), 0x032700);
	RAM32_W(ACTOR_ANIMSET(1), 0x0642D0);
	RAM16(ACTOR_FRAME(0)) = 0;
	RAM16(ACTOR_FRAME(1)) = 0;
	RAM8(ACTORS_ACTIVE) = 3;
	RAM8(ACTORS_ANIM_NEW) = 3;
	RAM8(ACTORS_DEPTH_ON) = 3;
	RAM8(0xFF0AC8) = 0;
	RAM8(ACTORS_TILES_READY) = 0;
	RAM8(ACTORS_MOVING) = 0;
	RAM8(ACTORS_NEED_SCALE) = 0;
	RAM8(ACTORS_BEHIND) = 0;
	RAM8(ACTORS_FROZEN) = 0;
	RAM8(ACTORS_NO_ZONE) = 0;
	RAM16(ACTOR_ANIM(0)) = 4;                   /* stand */
	RAM16(ACTOR_ANIM(1)) = 4;
	RAM16(ACTOR_ANIM(2)) = 0;
	for (int i = 3; i < 6; i++) RAM16(ACTOR_ANIM(i)) = 0;
	RAM16(SHAGGY_FACING) = 0;
	RAM16(SCOOBY_FACING) = 0;
	for (int i = 0; i < 4; i++) RAM16(0xFF05BC + 2 * i) = 0;
	for (int i = 0; i < 6; i++) RAM16(ACTOR_TIMER(i)) = 0xFFFF;
	for (int i = 0; i < 12; i++) RAM8(ACTOR_SHOWN_FRAME(0) + 4 * i) = 0xFF;  /* shown + next */
	RAM16(ACTOR_SHOWN_DEPTH_X(0)) = 0;
	RAM16(ACTOR_SHOWN_DEPTH_Y(0)) = 0;
	RAM16(ACTOR_SHOWN_DEPTH_X(1)) = 0;
	RAM16(ACTOR_SHOWN_DEPTH_Y(1)) = 0;
	/* the original then walks the object table doing nothing (a `nop` for each actor) */
	uint16_t n = RAM16(OBJ_LAST);
	uint32_t obj = OBJECTS;
	unsigned z = CC_Z;
	for (uint32_t k = 0; k <= n; k++, obj += OBJ_SIZE)
		z = (RAM8(obj + 0x18) & 0x02) ? 0 : CC_Z;
	set_w(&ctx->d[0], 0xFFFF);
	ctx->a[0] = obj;
	cc_set(ctx, CC_Z, z);
	hle_jump(ctx, 0x008190);                    /* bsr actor_animate / rts */
}
