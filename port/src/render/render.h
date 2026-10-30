/* Native renderer (GitLab #25): draws Genesis VDP mode-5 frames in C from VDP state
 * (VRAM / CRAM / VSRAM / registers / sprite cache) into an RGBA buffer.
 *
 * The renderer does not know about the emulator. It reads a `render_vdp` view of the VDP
 * memories and is driven line by line:
 *
 *     render_parse_sprites(rc, v, -1);              sprites for line 0 (end of VBlank)
 *     for each line l:
 *         render_line_pixels(rc, v, band, l);       planes, window, sprites -> rc->idx
 *         render_parse_sprites(rc, v, l);           sprites for line l+1 (if l < h-1)
 *         render_line_colors(rc, v, fb + l*pitch);  rc->idx -> RGBA
 *
 * Two ways to call it (docs/subsystems/render.md):
 *   - raster mode (render_emu.c): the emulator calls the three steps at the moments its own
 *     VDP renders, with the live VDP state. Mid-frame changes are seen exactly as GPGX sees
 *     them, so the output is pixel-identical to GPGX's (port/tools/rendertest.sh);
 *   - frame mode (render_frame): one call per frame from a `render_scene` (a snapshot plus the
 *     optional verb-bar band). This is the CPU-less contract: the scene is all a frame needs.
 *
 * Pixel semantics follow the VDP (and match GPGX bit for bit): 2-cell columns, h-scroll per
 * screen / cell / line, full or 2-cell v-scroll, window with its horizontal/vertical split,
 * sprites with the link list, 16/20 sprites and 256/320 pixels per line, sprite masking (x=0),
 * left column blanking, shadow/highlight, the 3-bit palette mode, H32 and H40.
 * Not supported (the game never uses them): interlace modes, mode 4 and below, 128 KiB VRAM.
 */
#ifndef SCOOBY_RENDER_H
#define SCOOBY_RENDER_H
#include <stdbool.h>
#include <stdint.h>

#define RENDER_MAX_W 640          /* widest output: 4:3 is 256/320, widescreen views up to 640 (#26) */
#define RENDER_MAX_H 240
#define RENDER_VIEW_CELLS (RENDER_MAX_W / 8 + 8)   /* cells of a render_view row, with margins */

/* The VDP state the renderer reads. All pointers are read-only. */
typedef struct render_vdp {
	const uint16_t *vram;   /* 32 Ki words: VRAM byte address a is the word vram[a >> 1] */
	const uint16_t *cram;   /* 64 colours in the VDP's 9-bit form: bits 8-6 blue, 5-3 green, 2-0 red */
	const uint16_t *vsram;  /* 40 words: v-scroll, A/B alternating (word 0 = plane A, 1 = plane B) */
	const uint8_t *reg;     /* VDP registers 0..23 */
	const uint16_t *sat;    /* the VDP's internal sprite cache: 4 words per sprite, of which word 0
	                         * (y) and word 1 (size, link) are used; x and attribute come from VRAM.
	                         * It is updated by VRAM writes that hit the sprite table. */
	int width;              /* active width latched for this frame: 256 (H32) or 320 (H40) */
} render_vdp;

/* A screen band (the verb-bar split, docs/subsystems/render.md "Bands"): from line `line` to the
 * end of the frame these values replace the VDP's plane A / plane B nametable registers, the
 * window register and the full-screen scroll values. `line` < 0: no band. */
typedef struct render_band {
	int line;
	uint8_t reg_plane_a;     /* register 2 */
	uint8_t reg_plane_b;     /* register 4 */
	uint8_t reg_window_v;    /* register 18 ($12) */
	uint16_t hscroll_a, hscroll_b;   /* h-scroll of planes A and B (full-screen h-scroll mode) */
	uint16_t vscroll_a, vscroll_b;   /* v-scroll of planes A and B (full-screen v-scroll mode) */
} render_band;

/* Everything one frame needs (frame mode). Future work (#26 widescreen, #27 interpolation) adds
 * fields here - a wider viewport, sub-pixel scroll and sprite positions - see render.md. */
typedef struct render_scene {
	render_vdp vdp;
	render_band band;
	int height;              /* active lines: 224 (V28) or 240 (V30) */
} render_scene;

/* A wider view of the room band (#26 widescreen, docs/subsystems/widescreen.md). Presentation
 * only: the renderer draws more of the planes and opens the sprites' x range; nothing it reads
 * is changed. A line drawn with a view shows the 4:3 screen's x in [x0, x0 + width). */
typedef struct render_view {
	int x0;                  /* 4:3 screen x drawn at output pixel 0 (<= 0 for a wider view) */
	int width;               /* output pixels (<= RENDER_MAX_W) */
	/* name table words for the cells the game has not drawn. The view's cells are counted from
	 * its left edge: output pixel x is in view cell (cell_ofs + x) >> 3, stored at index
	 * cell + 2 (the partly shown columns start left of pixel 0). names[p] (p 0 = plane A,
	 * 1 = plane B) is indexed [plane row][view cell + 2]; it replaces VRAM in the view cells
	 * whose from_vram[p][cell + 2] is 0. NULL: VRAM everywhere. Only for 64 x 32-cell planes. */
	int cell_ofs;
	const uint16_t (*names[2])[RENDER_VIEW_CELLS];
	const uint8_t *from_vram[2];
	long sprites_wrapped;    /* out: sprite pieces hidden because their x word is not 0..511 */
	long sprites_lost;       /* out: ... of which the true position was inside the view */
} render_view;

/* Renderer state that lives across lines and frames, like the VDP's internal state. */
typedef struct render_sprite { uint16_t y, x, attr, size, xraw; } render_sprite;
typedef struct render_ctx {
	render_sprite obj[2][20];   /* sprites of the next line / this line (parity of the line) */
	uint8_t obj_count[2];
	uint8_t spr_masking;        /* sprite masking armed (a sprite with x > 0 seen on the line) */
	int width;                  /* width the last line was drawn with */
	uint8_t idx[RENDER_MAX_W];  /* the last line: per pixel 'IPpp cccc' (intensity / priority,
	                             * palette, colour); see render.c */
} render_ctx;

void render_tables_init(void);                       /* build the lookup tables (idempotent) */
void render_ctx_reset(render_ctx *rc);

/* Draw line `line` into rc->idx. `band` may be NULL (or band->line < 0). */
void render_line_pixels(render_ctx *rc, const render_vdp *v, const render_band *band, int line);
/* The same line drawn through a wider view (rc->width = view->width). Uses the sprite list
 * parsed into rc like render_line_pixels; give the view its own render_ctx. */
void render_line_pixels_view(render_ctx *rc, const render_vdp *v, const render_band *band, int line,
                             render_view *view);
/* true if the window plane covers the whole of `line` (register 18 with the band applied), or
 * the window splits it horizontally (register 17): such a line is not a room line */
bool render_line_is_window(const render_vdp *v, const render_band *band, int line);
/* Parse the sprite table for line+1 (line = -1: for line 0). */
void render_parse_sprites(render_ctx *rc, const render_vdp *v, int line);
/* Convert rc->idx (rc->width pixels) to RGBA (bytes R, G, B, A in memory). */
void render_line_colors(const render_ctx *rc, const render_vdp *v, uint32_t *dst);

/* Frame mode: render a whole frame from a scene into fb (pitch in pixels). Returns the width. */
int render_frame(render_ctx *rc, const render_scene *s, uint32_t *fb, int pitch);

/* RGBA pixel helpers: the renderer's colours have 4-bit channels (c4 * 17 per byte). */
static inline uint32_t render_rgba(unsigned r4, unsigned g4, unsigned b4)
{
	return (r4 * 17u) | (g4 * 17u) << 8 | (b4 * 17u) << 16 | 0xFF000000u;
}
/* the same colour in GPGX's RGB565 encoding (for comparisons with its output) */
static inline uint16_t render_rgba_to_565(uint32_t c)
{
	unsigned r = (c & 0xFF) / 17, g = ((c >> 8) & 0xFF) / 17, b = ((c >> 16) & 0xFF) / 17;
	return (uint16_t)(r << 12 | (r >> 3) << 11 | g << 7 | (g >> 2) << 5 | b << 1 | b >> 3);
}
static inline uint32_t render_565_to_rgba(uint16_t p)
{
	return render_rgba((p >> 12) & 15, (p >> 7) & 15, (p >> 1) & 15);
}

#endif
