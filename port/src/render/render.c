/* Native renderer core (GitLab #25): one mode-5 line at a time from a render_vdp view.
 * See render.h for the calling sequence and docs/subsystems/render.md for the design.
 *
 * Pixels on the way through a line are bytes 'IPpp cccc':
 *   cccc = colour index in the palette (0 = transparent),
 *   pp   = palette (0-3),
 *   P    = the tile's priority bit (planes); after sprites are merged, bit 6/7 hold the
 *          intensity in shadow/highlight mode (00 shadow, 01 normal, 10 highlight),
 *   I    = in normal mode: "a sprite pixel was drawn here" (the first opaque sprite pixel of a
 *          line wins, later sprites are hidden behind it even where the plane hides the first).
 * Layer merging is done with lookup tables built from the priority rules below; the rules
 * reproduce the VDP (and GPGX, whose output we must match pixel for pixel) exactly, including
 * the cases that look odd: transparent pixels keep their priority bit for the plane merge, a
 * sprite pixel loses its priority bit once merged, palette 3 colours 14/15 are the
 * shadow/highlight operators and colour 14 of the other palettes is never shadowed. */
#include "render.h"

#include <string.h>

/* lookup tables: [(below << 8) | above] */
static uint8_t lut_planes[2][0x10000];   /* [sh] plane B pixel, plane A/window pixel */
static uint8_t lut_sprite[0x10000];      /* normal mode: merged line pixel, sprite pixel */
static uint8_t lut_sprites_sh[0x10000];  /* S/H: sprite line pixel, next sprite pixel */
static uint8_t lut_final_sh[0x10000];    /* S/H: merged planes pixel, sprite line pixel */
static bool g_init;

/* plane B pixel b under plane A (or window) pixel a */
static uint8_t merge_planes(uint8_t b, uint8_t a, bool sh)
{
	bool ap = a & 0x40, bp = b & 0x40;
	uint8_t c;
	if (ap)
		c = (a & 0x0F) ? a : b;          /* A has priority: A unless transparent */
	else if (bp)
		c = (b & 0x0F) ? b : a;          /* only B has priority */
	else
		c = (a & 0x0F) ? a : b;          /* same level: A in front */
	c &= 0x7F;
	if (sh && (ap || bp))
		c |= 0x80;                       /* S/H: a plane with priority keeps the line normal */
	if (!(c & 0x0F))
		c &= 0x80;                       /* transparent: backdrop (keep the intensity) */
	return c;
}

/* normal mode: sprite pixel s (opaque) drawn onto merged line pixel p */
static uint8_t merge_sprite(uint8_t p, uint8_t s)
{
	if (!(s & 0x0F) || (p & 0x80))      /* transparent, or an earlier sprite owns the pixel */
		return p;
	uint8_t c;
	if (s & 0x40)
		c = s & 0x3F;                    /* sprite with priority */
	else if ((p & 0x40) && (p & 0x0F))
		c = p & 0x3F;                    /* opaque plane pixel with priority hides it */
	else
		c = s & 0x3F;
	if (!(c & 0x0F))
		c = 0;
	return (uint8_t)(c | 0x80);
}

/* S/H: sprite s onto the sprite-only line pixel p (the first sprite wins) */
static uint8_t merge_sprites_sh(uint8_t p, uint8_t s)
{
	if (!(s & 0x0F))
		return p;
	uint8_t c = (p & 0x80) ? (p & 0x7F) : (s & 0x7F);
	if (!(c & 0x0F))
		c &= 0xC0;
	return (uint8_t)(c | 0x80);
}

/* S/H: the sprite line pixel s over the merged planes pixel p (bit 7 of p = normal intensity) */
static uint8_t merge_final_sh(uint8_t p, uint8_t s)
{
	uint8_t pf = p & 0x3F, pi = (p & 0x80) >> 1;        /* plane colour, 0x40 if normal */
	uint8_t sf = s & 0x3F, si = (uint8_t)((s & 0x40) | pi);
	bool sprite_wins;
	if (s & 0x40)
		sprite_wins = s & 0x0F;
	else if (p & 0x40)
		sprite_wins = !(p & 0x0F) && (s & 0x0F);
	else
		sprite_wins = s & 0x0F;
	uint8_t c;
	if (!sprite_wins) {
		c = pf | pi;
	} else if ((sf & 0x3E) == 0x3E) {                   /* palette 3, colours 14/15: operators */
		if (sf & 1)
			c = pf;                                     /* 15: shadow what is below */
		else
			c = (p & 0x80) ? (pf | 0x80) : (pf | 0x40); /* 14: highlight it (shadow -> normal) */
	} else if (sf == 0x0E || sf == 0x1E || sf == 0x2E) {
		c = sf | 0x40;                                  /* colour 14: always normal */
	} else {
		c = sf | si;
	}
	if (!(c & 0x0F))
		c &= 0xC0;
	return c;
}

void render_tables_init(void)
{
	if (g_init)
		return;
	for (int below = 0; below < 256; below++)
		for (int above = 0; above < 256; above++) {
			int i = below << 8 | above;
			lut_planes[0][i] = merge_planes((uint8_t)below, (uint8_t)above, false);
			lut_planes[1][i] = merge_planes((uint8_t)below, (uint8_t)above, true);
			lut_sprite[i] = merge_sprite((uint8_t)below, (uint8_t)above);
			lut_sprites_sh[i] = merge_sprites_sh((uint8_t)below, (uint8_t)above);
			lut_final_sh[i] = merge_final_sh((uint8_t)below, (uint8_t)above);
		}
	g_init = true;
}

void render_ctx_reset(render_ctx *rc)
{
	memset(rc, 0, sizeof *rc);
}

/* ------------------------------------------------------------------ per-line setup */
static const uint8_t pf_shift_tbl[4] = { 6, 7, 0, 8 };            /* name table row stride, log2 bytes */
static const uint8_t pf_col_mask_tbl[4] = { 0x0F, 0x1F, 0x0F, 0x3F }; /* in 2-cell columns */
static const uint16_t pf_row_mask_tbl[4] = { 0x0FF, 0x1FF, 0x2FF, 0x3FF };
static const uint8_t hscroll_mask_tbl[4] = { 0x00, 0x07, 0xF8, 0xFF };

typedef struct { uint8_t left, right, enable; } clip_t;

/* register 17: the window's horizontal split. clip[0] = plane A columns, clip[1] = window
 * columns (2-cell units); a disabled entry keeps whatever it had. */
static void window_clip(clip_t clip[2], uint8_t r17, bool h40)
{
	int hp = r17 & 0x1F, hf = (r17 >> 7) & 1;
	int a = hf, w = hf ^ 1;      /* which entry gets the left part: the window unless 'right' */
	int sw = h40 ? 20 : 16;
	if (hp) {
		if (hp > sw) {
			clip[w] = (clip_t){ 0, (uint8_t)sw, 1 };
			clip[a].enable = 0;
		} else {
			clip[w].left = 0;
			clip[a].right = (uint8_t)sw;
			clip[a].left = clip[w].right = (uint8_t)hp;
			clip[0].enable = clip[1].enable = 1;
		}
	} else {
		clip[a] = (clip_t){ 0, (uint8_t)sw, 1 };
		clip[w].enable = 0;
	}
}

static inline uint16_t vram_w(const render_vdp *v, uint32_t byte_addr)
{
	return v->vram[(byte_addr & 0xFFFF) >> 1];
}

/* one cell row: 8 pixels of the tile in name-table word `attr`, pattern row `row` */
static inline void draw_cell(uint8_t *dst, const render_vdp *v, uint16_t attr, int row)
{
	uint8_t atex = (uint8_t)(((attr >> 13) & 7) << 4);          /* priority + palette */
	int r = (attr & 0x1000) ? 7 - row : row;
	uint32_t addr = ((uint32_t)(attr & 0x7FF) << 5) | (uint32_t)(r << 2);
	uint32_t bits = (uint32_t)vram_w(v, addr) << 16 | vram_w(v, addr + 2);
	if (attr & 0x0800) {
		for (int x = 0; x < 8; x++, bits >>= 4)
			dst[x] = (uint8_t)((bits & 15) | atex);
	} else {
		for (int x = 7; x >= 0; x--, bits >>= 4)
			dst[x] = (uint8_t)((bits & 15) | atex);
	}
}

/* a 2-cell column: the name-table long at byte address `nt` (two tile words, left first) */
static inline void draw_column(uint8_t *dst, const render_vdp *v, uint32_t nt, int row)
{
	draw_cell(dst, v, vram_w(v, nt), row);
	draw_cell(dst + 8, v, vram_w(v, nt + 2), row);
}

/* a 2-cell column of plane p (0 = A, 1 = B) through a wider view: the cells the game has not
 * drawn come from the view's name table (render_view.names), the others from VRAM */
static inline void view_column(uint8_t *dst, const uint8_t *line0, const render_vdp *v,
                               const render_view *vw, int p, uint32_t nt, uint32_t vl)
{
	/* dst - line0 = output x of the column's left pixel (>= -16), on a cell boundary */
	int k = (vw->cell_ofs + (int)(dst - line0) + 16) >> 3;      /* view cell + 2 */
	if (k < 0 || k + 1 >= RENDER_VIEW_CELLS) {
		draw_column(dst, v, nt, (int)(vl & 7));
		return;
	}
	const uint8_t *fv = vw->from_vram[p];
	unsigned row = (vl >> 3) & 31;
	uint16_t w0 = fv[k] ? vram_w(v, nt) : vw->names[p][row][k];
	uint16_t w1 = fv[k + 1] ? vram_w(v, nt + 2) : vw->names[p][row][k + 1];
	draw_cell(dst, v, w0, (int)(vl & 7));
	draw_cell(dst + 8, v, w1, (int)(vl & 7));
}

/* ------------------------------------------------------------------ planes */
static void render_planes(const render_vdp *v, const uint8_t *reg, const uint16_t *vsram,
                          const render_band *band, int line, int width, uint8_t *out,
                          const render_view *vw)
{
	/* line buffers: pixel x of the screen is at [0x20 + x]; the margin takes the partly
	 * scrolled-in column on the left and sprites hanging off either side */
	uint8_t bufB[0x20 + RENDER_MAX_W + 0x40], bufA[0x20 + RENDER_MAX_W + 0x40];
	bool h40 = reg[12] & 1;
	uint32_t ntab = ((uint32_t)reg[2] << 10) & 0xE000;
	uint32_t ntbb = ((uint32_t)reg[4] << 13) & 0xE000;
	uint32_t ntwb = ((uint32_t)reg[3] << 10) & (h40 ? 0xF000 : 0xF800);
	uint32_t hscb = ((uint32_t)reg[13] << 10) & 0xFC00;
	unsigned pf_shift = pf_shift_tbl[reg[16] & 3];
	uint32_t col_mask = pf_col_mask_tbl[reg[16] & 3];
	uint32_t row_mask = pf_row_mask_tbl[(reg[16] >> 4) & 3];
	bool vs_cells = reg[11] & 4;

	uint32_t hs_off = hscb + (((uint32_t)line & hscroll_mask_tbl[reg[11] & 3]) << 2);
	uint16_t hs_a = vram_w(v, hs_off), hs_b = vram_w(v, hs_off + 2);
	if (band) {
		hs_a = band->hscroll_a;
		hs_b = band->hscroll_b;
	}
	int columns = width >> 4;
	bool ovr = false;              /* a view with its own name table for undrawn cells */
	if (vw) {
		/* the view starts x0 pixels left of the screen: scroll the planes by -x0 more */
		hs_a = (uint16_t)(hs_a - vw->x0);
		hs_b = (uint16_t)(hs_b - vw->x0);
		columns = (vw->width + 15) >> 4;
		ovr = vw->names[0] && vw->names[1] && pf_shift == 7 && col_mask == 0x1F;
	}
#define VS_COL(c) ((c) < 20 ? (c) : 19)    /* 2-cell v-scroll entries exist for 20 columns */

	/* plane B */
	uint32_t shift = hs_b & 0x0F;
	uint32_t index = col_mask + 1 - ((hs_b >> 4) & col_mask);
	uint32_t edge_vs = 0;     /* v-scroll of a partly shown left column in 2-cell mode */
	if (vs_cells && h40)
		edge_vs = vsram[38] & vsram[39];
	uint8_t *dst;
	if (shift) {
		uint32_t vl = (line + (vs_cells ? edge_vs : vsram[1])) & row_mask;
		uint32_t nt = ntbb + (((vl >> 3) << pf_shift) & 0x1FC0);
		if (ovr)
			view_column(bufB + 0x10 + shift, bufB + 0x20, v, vw, 1, nt + (((index - 1) & col_mask) << 2), vl);
		else
			draw_column(bufB + 0x10 + shift, v, nt + (((index - 1) & col_mask) << 2), (int)(vl & 7));
	}
	dst = bufB + 0x20 + shift;
	for (int col = 0; col < columns; col++, index++, dst += 16) {
		uint32_t vl = (line + vsram[vs_cells ? 2 * VS_COL(col) + 1 : 1]) & row_mask;
		uint32_t nt = ntbb + (((vl >> 3) << pf_shift) & 0x1FC0);
		if (ovr)
			view_column(dst, bufB + 0x20, v, vw, 1, nt + ((index & col_mask) << 2), vl);
		else
			draw_column(dst, v, nt + ((index & col_mask) << 2), (int)(vl & 7));
	}

	/* plane A and the window */
	clip_t clip[2] = { { 0, 0, 0 }, { 0, 0, 0 } };
	window_clip(clip, reg[17], h40);
	int a_line = (reg[18] & 0x1F) << 3;
	int w_down = (reg[18] >> 7) & 1;
	int a_on, w_on, start = 0, end = columns;
	if (w_down == (line >= a_line)) {
		a_on = 0;                      /* the window takes the whole line */
		w_on = 1;
	} else {
		a_on = clip[0].enable;
		w_on = clip[1].enable;
	}
	if (a_on) {
		start = clip[0].left;
		end = clip[0].right;
		if (vw && !w_on) {             /* a view draws plane A across its whole width */
			start = 0;
			end = columns;
		}
		shift = hs_a & 0x0F;
		index = col_mask + (uint32_t)start + 1 - ((hs_a >> 4) & col_mask);
		if (shift) {
			uint32_t vl = (line + (vs_cells ? edge_vs : vsram[0])) & row_mask;
			uint32_t nt = ntab + (((vl >> 3) << pf_shift) & 0x1FC0);
			/* the VDP's "window bug": next to the window the partial column is fetched one
			 * column too far right */
			uint32_t idx = start ? index : index - 1;
			if (ovr)
				view_column(bufA + 0x10 + shift + (start << 4), bufA + 0x20, v, vw, 0, nt + ((idx & col_mask) << 2), vl);
			else
				draw_column(bufA + 0x10 + shift + (start << 4), v, nt + ((idx & col_mask) << 2), (int)(vl & 7));
		}
		dst = bufA + 0x20 + shift + (start << 4);
		for (int col = start; col < end; col++, index++, dst += 16) {
			uint32_t vl = (line + vsram[vs_cells ? 2 * VS_COL(col) : 0]) & row_mask;
			uint32_t nt = ntab + (((vl >> 3) << pf_shift) & 0x1FC0);
			if (ovr)
				view_column(dst, bufA + 0x20, v, vw, 0, nt + ((index & col_mask) << 2), vl);
			else
				draw_column(dst, v, nt + ((index & col_mask) << 2), (int)(vl & 7));
		}
		start = clip[1].left;
		end = clip[1].right;
	}
	if (w_on) {
		uint32_t nt = ntwb | ((uint32_t)(line >> 3) << (6 + h40));
		dst = bufA + 0x20 + (start << 4);
		for (int col = start; col < end; col++, dst += 16)
			draw_column(dst, v, nt + ((uint32_t)col << 2), line & 7);
	}

	const uint8_t *lut = lut_planes[(reg[12] >> 3) & 1];
	if (vw)
		width = vw->width;
	for (int x = 0; x < width; x++)
		out[0x20 + x] = lut[bufB[0x20 + x] << 8 | bufA[0x20 + x]];
#undef VS_COL
}

/* ------------------------------------------------------------------ sprites */
/* tile offset inside a sprite: column c / row r of a (w+1)x(h+1)-cell sprite, with flips;
 * tiles go down the columns */
static inline int sprite_tile(int c, int r, int size, uint16_t attr)
{
	int w = (size >> 2) & 3, h = size & 3;
	if (attr & 0x0800)
		c = w - c;
	if (attr & 0x1000)
		r = h - r;
	return r + c * (h + 1);
}

static void render_sprites(render_ctx *rc, const render_vdp *v, const uint8_t *reg, int line,
                           int width, uint8_t *out, render_view *vw)
{
	int screen_w = width;          /* the VDP's width (sprite masking carry-over) */
	int x0 = 0;
	bool hide = false;
	if (vw) {
		width = vw->width;
		x0 = vw->x0;
	}
	bool sh = reg[12] & 8;
	int max_pixels = (reg[12] & 1) ? 320 : 256;
	uint8_t sline[0x20 + RENDER_MAX_W + 0x40];
	uint8_t *buf = out;
	const uint8_t *lut = lut_sprite;
	if (sh) {
		memset(sline, 0, (size_t)width + 0x40);
		buf = sline;
		lut = lut_sprites_sh;
	}
	const render_sprite *o = rc->obj[line & 1];
	int count = rc->obj_count[line & 1];
	int pixels = 0, masked = 0;
	bool limit = false;
	for (; count > 0; count--, o++) {
		int xpos = o->x;
		/* a sprite at x = 0 hides the rest of the line's sprites, once a sprite with x > 0
		 * has been seen on this line (or the previous line hit the pixel limit) */
		if (xpos)
			rc->spr_masking = 1;
		else if (rc->spr_masking)
			masked = 1;
		xpos -= 0x80 + x0;
		int w = 8 + ((o->size & 0x0C) << 1);
		pixels += w;
		if (vw) {
			/* the game's x word is outside the 9 bits: in 4:3 the VDP's wrap put it out of
			 * sight, a wider view could show it 512 px away from where the game put it */
			hide = o->xraw >= 0x200;
			if (hide && xpos + w > 0 && xpos < width) {
				vw->sprites_wrapped++;
				int tx = (int16_t)o->xraw - 0x80 - x0;          /* where the game meant it */
				if (tx + w > 0 && tx < width)
					vw->sprites_lost++;
			}
		}
		if (xpos + w > 0 && xpos < width && !masked && !hide) {
			uint16_t attr = o->attr;
			uint8_t atex = (uint8_t)((attr >> 9) & 0x70);
			int name = attr & 0x7FF;
			int yoff = o->y;
			if (pixels > max_pixels)
				w -= pixels - max_pixels;
			int cells = w >> 3;
			uint8_t *lb = buf + 0x20 + xpos;
			for (int c = 0; c < cells; c++, lb += 8) {
				int tile = (name + sprite_tile(c, (yoff >> 3) & 3, o->size, attr)) & 0x7FF;
				uint8_t px[8];
				draw_cell(px, v, (uint16_t)((attr & 0x1800) | tile), yoff & 7);
				for (int i = 0; i < 8; i++)
					if (px[i] & 0x0F)
						lb[i] = lut[lb[i] << 8 | ((px[i] & 0x0F) | atex)];
			}
		}
		if (pixels >= max_pixels) {
			/* pixel limit: masking carries over to the next line if the line was full */
			rc->spr_masking = pixels >= screen_w;
			limit = true;
			break;
		}
	}
	if (!limit)
		rc->spr_masking = 0;
	if (sh)
		for (int x = 0; x < width; x++)
			out[0x20 + x] = lut_final_sh[out[0x20 + x] << 8 | sline[0x20 + x]];
}

void render_parse_sprites(render_ctx *rc, const render_vdp *v, int line)
{
	int max = v->width >> 4;                            /* 16 / 20 sprites per line */
	int total = ((v->reg[12] & 1) ? 320 : 256) >> 2;    /* 64 / 80 sprites in the list */
	bool h40 = v->reg[12] & 1;
	uint32_t satb = ((uint32_t)v->reg[5] << 9) & (h40 ? 0xFC00 : 0xFE00);
	render_sprite *o = rc->obj[(line + 1) & 1];
	int count = 0, link = 0;
	int y = line + 0x81;
	do {
		int ypos = v->sat[link] & 0x1FF;
		if (y >= ypos) {
			int size = v->sat[link + 1] >> 8;
			int height = 8 + ((size & 3) << 3);
			int dy = y - ypos;
			if (dy < height) {
				if (count == max)
					break;                              /* sprite overflow */
				o->attr = vram_w(v, satb + 2 * (uint32_t)(link + 2));
				o->xraw = vram_w(v, satb + 2 * (uint32_t)(link + 3));
				o->x = o->xraw & 0x1FF;
				o->y = (uint16_t)dy;
				o->size = (uint16_t)(size & 0x0F);
				count++;
				o++;
			}
		}
		link = (v->sat[link + 1] & 0x7F) << 2;
		if (link == 0 || link >= v->width)
			break;
	} while (--total);
	rc->obj_count[y & 1] = (uint8_t)count;
}

/* ------------------------------------------------------------------ the line */
static void line_pixels(render_ctx *rc, const render_vdp *v, const render_band *band, int line,
                        render_view *vw)
{
	int width = v->width > RENDER_MAX_W ? RENDER_MAX_W : v->width;
	uint8_t out[0x20 + RENDER_MAX_W + 0x40];
	const uint8_t *reg = v->reg;
	const uint16_t *vsram = v->vsram;
	uint8_t breg[24];
	uint16_t bvs[40];
	if (band && (band->line < 0 || line < band->line))
		band = NULL;
	if (band) {
		memcpy(breg, v->reg, sizeof breg);
		breg[2] = band->reg_plane_a;
		breg[4] = band->reg_plane_b;
		breg[18] = band->reg_window_v;
		memcpy(bvs, v->vsram, sizeof bvs);
		bvs[0] = band->vscroll_a;
		bvs[1] = band->vscroll_b;
		reg = breg;
		vsram = bvs;
	}
	int out_w = vw ? vw->width : width;
	rc->width = out_w;
	if (!(reg[1] & 0x40)) {
		memset(rc->idx, 0x40, (size_t)out_w);           /* display off: backdrop */
		return;
	}
	render_planes(v, reg, vsram, band, line, width, out, vw);
	render_sprites(rc, v, reg, line, width, out, vw);
	if ((reg[0] & 0x20) && !vw)
		memset(out + 0x20, 0x40, 8);                     /* left column blanking */
	memcpy(rc->idx, out + 0x20, (size_t)out_w);
}

void render_line_pixels(render_ctx *rc, const render_vdp *v, const render_band *band, int line)
{
	line_pixels(rc, v, band, line, NULL);
}

void render_line_pixels_view(render_ctx *rc, const render_vdp *v, const render_band *band, int line,
                             render_view *view)
{
	line_pixels(rc, v, band, line, view);
}

bool render_line_is_window(const render_vdp *v, const render_band *band, int line)
{
	uint8_t r18 = v->reg[18];
	if (band && band->line >= 0 && line >= band->line)
		r18 = band->reg_window_v;
	int a_line = (r18 & 0x1F) << 3;
	if (((r18 >> 7) & 1) == (line >= a_line))
		return true;                                     /* the window takes the whole line */
	return (v->reg[17] & 0x1F) != 0;                     /* a horizontal window split */
}

void render_line_colors(const render_ctx *rc, const render_vdp *v, uint32_t *dst)
{
	/* 9-bit CRAM colour -> 4-bit channels: shadow = c, normal = 2c, highlight = c + 7 */
	uint32_t pal[3][64];
	bool sh = v->reg[12] & 8;
	uint16_t mask = (v->reg[0] & 4) ? 0x1FF : 0x49;     /* palette select off: 1 bit per channel */
	for (int i = 0; i < 64; i++) {
		uint16_t c = (uint16_t)(((i & 0x0F) ? v->cram[i] : v->cram[v->reg[7] & 0x3F]) & mask);
		unsigned r = c & 7, g = (c >> 3) & 7, b = (c >> 6) & 7;
		pal[1][i] = render_rgba(r << 1, g << 1, b << 1);
		if (sh) {
			pal[0][i] = render_rgba(r, g, b);
			pal[2][i] = render_rgba(r + 7, g + 7, b + 7);
		}
	}
	for (int x = 0; x < rc->width; x++) {
		uint8_t p = rc->idx[x];
		int in = sh ? (p >> 6) : 1;
		dst[x] = pal[in > 2 ? 2 : in][p & 0x3F];
	}
}

int render_frame(render_ctx *rc, const render_scene *s, uint32_t *fb, int pitch)
{
	const render_vdp *v = &s->vdp;
	bool on = v->reg[1] & 0x40;
	if (on)
		render_parse_sprites(rc, v, -1);
	for (int line = 0; line < s->height && line < RENDER_MAX_H; line++) {
		render_line_pixels(rc, v, &s->band, line);
		if (on && line < s->height - 1)
			render_parse_sprites(rc, v, line);
		render_line_colors(rc, v, fb + (size_t)line * (size_t)pitch);
	}
	return rc->width;
}
