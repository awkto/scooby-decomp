/* Widescreen (GitLab #26), presentation only. See widescreen.h and docs/subsystems/widescreen.md.
 *
 * Per frame (ws_frame_begin, at line 0 of the display):
 *   1. the per-screen rules (ws_rules) decide whether this frame is a room view that can be
 *      shown wide; anything else is drawn 4:3, centred in the wide frame;
 *   2. the camera of the frame is the one its scroll values show (plane B's h-scroll is
 *      -camera x, v-scroll camera y); the view's left edge is the camera less the margin,
 *      clamped to the room: rcam = clamp(cam - margin, 0, room width - view width), centred
 *      when the room is narrower than the view (bars on both sides);
 *   3. the game keeps only the 4:3 window drawn in the 64 x 32-cell planes (room_draw_view
 *      draws 32 x 19 cells, the column/row streamers one new column/row as the camera crosses
 *      a cell), so the other plane cells hold stale tiles. The view takes the cells the 4:3
 *      picture shows from VRAM (so the middle is the 4:3 picture, pixel for pixel) and every
 *      other cell from the game's working copies of the room's name tables in RAM
 *      ($FF7000 plane B, $FF8C00 plane A: the streamers' source, which the overlays patch).
 * Nothing is written to the emulated machine. */
#include "widescreen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu.h"

/* game RAM (docs/formats.md, docs/subsystems/walking.md) */
#define VBLANK_HOOK   0xFF0000   /* l: VBlank work routine */
#define HBLANK_HOOK   0xFF0004   /* l: HBlank routine */
#define VBLANK_MAIN   0x00A65C   /* the in-game VBlank handler */
#define HBLANK_VERB   0x01006C   /* the verb-bar split */
#define MYSTERY_IDX   0xFF06AA
#define ROOM_INDEX    0xFF06AC
#define MYSTERY_HDR   0xFF068E
#define MAP_WIDTH     0xFF06C6   /* cells */
#define MAP_HEIGHT    0xFF06C8
#define CAMERA_X      0xFF06CA
#define CAMERA_Y      0xFF06CE
#define VERB_STATE    0xFF09DE   /* bit 6: close-up picture layout (plane A follows Shaggy) */
#define PLANEB_MAP    0xFF7000   /* working copies of the room's name tables, width x height words */
#define PLANEA_MAP    0xFF8C00

#define SCREEN_TOP    16         /* first line of the room band (the name band is the window) */
#define BAR_LINE      168        /* the verb bar's band, when no band was recorded */
#define ROOM_ROWS     20         /* plane rows the room band can show (19 + a partial one) */

/* ------------------------------------------------------------------ options */
static bool g_on;                /* widescreen requested */
static bool g_auto;              /* width from the window's aspect */
static int g_width = 398;        /* output width when on */
static bool g_stats;

static int width_for_aspect(double a)
{
	int w = (int)lround(224.0 * a);
	if (w < 256)
		w = 256;
	if (w > RENDER_MAX_W)
		w = RENDER_MAX_W;
	return w;
}

bool ws_configure(const char *spec)
{
	if (!spec || !*spec)
		return false;
	if (!strcmp(spec, "off") || !strcmp(spec, "4:3") || !strcmp(spec, "0")) {
		g_on = false;
		return true;
	}
	if (!strcmp(spec, "auto")) {
		g_on = g_auto = true;
		return true;
	}
	int a, b;
	if (sscanf(spec, "%d:%d", &a, &b) == 2 && a > 0 && b > 0) {
		g_width = width_for_aspect((double)a / b);
	} else if (sscanf(spec, "%d", &a) == 1 && a >= 256 && a <= RENDER_MAX_W) {
		g_width = a;
	} else {
		return false;
	}
	g_auto = false;
	g_on = g_width > 256;
	return true;
}

bool ws_enabled(void) { return g_on; }
void ws_set_enabled(bool on) { g_on = on; }
void ws_toggle(void) { g_on = !g_on; }
int ws_width(void) { return g_on ? g_width : 0; }
void ws_set_stats(bool on) { g_stats = on; }

void ws_set_window_aspect(int out_w, int out_h)
{
	if (g_auto && out_w > 0 && out_h > 0)
		g_width = width_for_aspect((double)out_w / out_h);
}

/* ------------------------------------------------------------------ per-screen rules */
/* Which screens are drawn wide. Checked in order; the first that matches decides. */
typedef enum {
	WS_OK = 0,           /* a room view: wide */
	WS_NOT_ROOM,         /* title, menus, logos, password, intro pages, credits: not vblank_main */
	WS_NO_MAP,           /* no mystery loaded / map size not a room's */
	WS_CLOSEUP,          /* close-up picture (script sub-op $07): plane A follows Shaggy */
	WS_SCROLL_MODE,      /* line/cell h-scroll or 2-cell v-scroll: not the room camera */
	WS_PLANE_SPLIT,      /* planes A and B scrolled differently (cut-scene layouts) */
	WS_CAMERA_OUTSIDE,   /* the scroll shows a part of the planes outside the room (full-screen picture) */
	WS_PLANES_NOT_MAP,   /* the planes do not show the room map (full-screen picture, ...) */
	WS_REASONS
} ws_reason;

static const struct { ws_reason r; const char *name, *why; } ws_rules[WS_REASONS] = {
	{ WS_OK, "room", "room view: the room band is wide, name band / text box / verb bar 4:3" },
	{ WS_NOT_ROOM, "not-room", "VBlank hook is not vblank_main ($A65C): title, menus, password, intro pages, credits" },
	{ WS_NO_MAP, "no-map", "no mystery header or the map size is not a room's" },
	{ WS_CLOSEUP, "closeup", "close-up picture ($FF09DE bit 6): plane A is the picture, placed at Shaggy" },
	{ WS_SCROLL_MODE, "scroll-mode", "h-scroll per line/cell or 2-cell v-scroll: not the room camera" },
	{ WS_PLANE_SPLIT, "plane-split", "planes A and B scrolled differently" },
	{ WS_CAMERA_OUTSIDE, "camera-outside", "the h-scroll shows plane columns outside the room (full-screen pictures animate by flipping it)" },
	{ WS_PLANES_NOT_MAP, "planes-not-map", "the 4:3 window of the planes does not show the room map (full-screen picture, ...)" },
};

/* ------------------------------------------------------------------ frame state */
static uint16_t g_names[2][32][RENDER_VIEW_CELLS];  /* [A, B][plane row][view cell + 2]: the room map */
static uint8_t g_from_vram[2][RENDER_VIEW_CELLS];   /* view cells the 4:3 picture shows */
static render_view g_view;
static bool g_frame_wide;
static ws_reason g_reason = WS_NOT_ROOM;
static int g_bar_l, g_bar_r;             /* room band: output x outside [l, r) is a bar */
static int g_screen_w = 256;             /* the VDP's width this frame */

/* layout of the last finished frame (mouse mapping) */
static struct { bool on, wide; int width, x0, centre, screen_w; } g_last, g_cur;

/* counters */
static long c_frames, c_wide, c_reason[WS_REASONS], c_flips;
static long c_cells, c_mismatch, c_mismatch_frames, c_stale;
static long c_bar_frames, c_unneeded_bars;
static long c_wrapped, c_lost, c_wrapped_frames;
static long c_centre_lines, c_centre_bad;     /* room lines whose 4:3 part differs from the 4:3 line */
typedef struct { int myst, room; long frames, wide, mismatch, wrapped, lost, bars, unneeded, stale, reason[WS_REASONS]; } room_stat;
static room_stat g_rooms[64];
static int g_nrooms;

static room_stat *room_entry(void)
{
	int m = emu_ram16(MYSTERY_IDX), r = emu_ram16(ROOM_INDEX);
	for (int i = 0; i < g_nrooms; i++)
		if (g_rooms[i].myst == m && g_rooms[i].room == r)
			return &g_rooms[i];
	if (g_nrooms == 64)
		return NULL;
	g_rooms[g_nrooms] = (room_stat){ .myst = m, .room = r };
	return &g_rooms[g_nrooms++];
}

static inline uint16_t vram_word(const render_vdp *v, uint32_t a) { return v->vram[(a & 0xFFFF) >> 1]; }

/* the nearest x to `near` with x == want (mod m) */
static int resolve(int near, int want, int m)
{
	int d = ((want - near) % m + m) % m;
	if (d >= m / 2)
		d -= m;
	return near + d;
}

static ws_reason decide(const render_vdp *v, int *cam_x)
{
	const uint8_t *reg = v->reg;
	if (emu_ram32(VBLANK_HOOK) != VBLANK_MAIN)
		return WS_NOT_ROOM;
	int w = emu_ram16(MAP_WIDTH), h = emu_ram16(MAP_HEIGHT);
	if (!emu_ram32(MYSTERY_HDR) || w < 32 || w > 128 || h < 19 || h > 64)
		return WS_NO_MAP;
	if (emu_ram8(VERB_STATE) & 0x40)
		return WS_CLOSEUP;
	if ((reg[11] & 7) != 0 || (reg[16] & 0x33) != 0x01)   /* full-screen scroll, 64 x 32 planes */
		return WS_SCROLL_MODE;
	uint32_t hs = ((uint32_t)reg[13] << 10) & 0xFC00;
	uint16_t hs_a = vram_word(v, hs), hs_b = vram_word(v, hs + 2);
	if (((hs_a ^ hs_b) & 0x1FF) || ((v->vsram[0] ^ v->vsram[1]) & 0xFF))
		return WS_PLANE_SPLIT;
	*cam_x = resolve((int16_t)emu_ram16(CAMERA_X), (-(int)hs_b) & 0x1FF, 512);
	return WS_OK;
}

/* the room map in plane coordinates, the VRAM columns of the 4:3 picture, and the terrain check
 * of those columns (VRAM against the map); returns the mismatching cells */
static long build_names(const render_vdp *v, int cam_x, int rcam, room_stat *rs)
{
	int w = emu_ram16(MAP_WIDTH), h = emu_ram16(MAP_HEIGHT);
	int vs = v->vsram[1] & 0xFF;
	int cam_y = resolve((int16_t)emu_ram16(CAMERA_Y), vs, 256);   /* v-scroll = camera y (+ shake) */
	int base = (cam_y >> 3) - 4;
	int wrow[32];
	for (int r = 0; r < 32; r++)          /* plane row r shows world row wr with (wr + 2) % 32 == r */
		wrow[r] = base + (((r - 2 - base) % 32) + 32) % 32;
	/* view cell k (index k + 2) is world column c0 + k; the 4:3 picture shows world columns
	 * [cam_x / 8, (cam_x + 255) / 8], which the game has drawn: those come from VRAM */
	int c0 = rcam >> 3;                   /* arithmetic shift: floor */
	int first = cam_x >> 3, last = (cam_x + g_screen_w - 1) >> 3;
	for (int i = 0; i < RENDER_VIEW_CELLS; i++) {
		int wc = c0 + i - 2;
		g_from_vram[0][i] = g_from_vram[1][i] = wc >= first && wc <= last;
	}
	for (int p = 0; p < 2; p++) {
		uint32_t map = p ? PLANEB_MAP : PLANEA_MAP;
		for (int r = 0; r < 32; r++) {
			int wr = wrow[r];
			for (int i = 0; i < RENDER_VIEW_CELLS; i++) {
				int wc = c0 + i - 2;
				g_names[p][r][i] = (wr >= 0 && wr < h && wc >= 0 && wc < w)
				                       ? emu_ram16(map + 2 * (uint32_t)(wr * w + wc)) : 0;
			}
		}
	}
	/* terrain check: in the plane rows of the room band, the 4:3 columns must hold the map
	 * (they are the streamers' output); the other columns of the view are stale in VRAM (not
	 * checked, counted: what the view would show wrong without the map) */
	uint32_t nt[2] = { ((uint32_t)v->reg[2] << 10) & 0xE000, ((uint32_t)v->reg[4] << 13) & 0xE000 };
	long bad = 0, cells = 0, stale = 0;
	int prow0 = ((SCREEN_TOP + vs) & 0xFF) >> 3;
	int ncells = (g_cur.width + 7) / 8 + 1;
	for (int p = 0; p < 2; p++)
		for (int k = 0; k < ROOM_ROWS; k++) {
			int r = (prow0 + k) & 31, wr = wrow[r];
			if (wr < 0 || wr >= h)
				continue;
			for (int i = 2; i < 2 + ncells && i < RENDER_VIEW_CELLS; i++) {
				int wc = c0 + i - 2;
				if (wc < 0 || wc >= w)
					continue;
				uint16_t vw = vram_word(v, nt[p] + (uint32_t)(r * 128 + (wc & 63) * 2));
				if (g_from_vram[p][i]) {
					cells++;
					if (vw != g_names[p][r][i]) {
						bad++;
						if (getenv("SCOOBY_WS_DEBUG") && bad <= 2)
							fprintf(stderr, "ws %ld: plane %c row %d (world %d) col %d (4:3 cols %d..%d, cam %d,%d vs %d): vram %04X map %04X\n",
							        emu_frame_count(), p ? 'B' : 'A', r, wr, wc, first, last, cam_x, cam_y, vs,
							        vw, g_names[p][r][i]);
					}
				} else {
					stale += vw != g_names[p][r][i];
				}
			}
		}
	if (!cells)
		bad = 1;                          /* nothing of the room in the window: not a room view */
	c_cells += cells;
	if (rs)
		rs->stale += stale;
	c_stale += stale;
	return bad;
}

void ws_frame_begin(const render_vdp *v)
{
	g_screen_w = v->width;
	g_cur.on = g_on;
	g_cur.width = g_on ? (g_width > v->width ? g_width : v->width) : v->width;
	g_cur.screen_w = v->width;
	g_cur.centre = (g_cur.width - v->width) / 2;
	g_cur.x0 = -g_cur.centre;
	bool was = g_frame_wide;
	g_frame_wide = false;
	if (!g_on)
		return;
	c_frames++;
	int cam_x = 0;
	g_reason = decide(v, &cam_x);
	room_stat *rs = g_reason == WS_NOT_ROOM || g_reason == WS_NO_MAP ? NULL : room_entry();
	if (rs)
		rs->frames++;
	int vw = g_cur.width;
	int rw = emu_ram16(MAP_WIDTH) * 8;
	int rcam = 0;
	if (g_reason == WS_OK) {
		if (rw >= vw) {
			rcam = cam_x - (vw - v->width) / 2;
			if (rcam > rw - vw) rcam = rw - vw;
			if (rcam < 0) rcam = 0;
		} else {
			rcam = (rw - vw) / 2;            /* narrower than the view: centred, bars both sides */
		}
		if (cam_x < 0 || cam_x > (rw > v->width ? rw - v->width : 0))
			g_reason = WS_CAMERA_OUTSIDE;
		if (rcam > cam_x) rcam = cam_x;      /* the 4:3 picture stays inside the view */
		if (rcam < cam_x + v->width - vw) rcam = cam_x + v->width - vw;
		/* the 4:3 window of the planes must be exactly the room map: otherwise the planes hold
		 * something else (a picture, cleared VRAM before a load) and the map would not continue
		 * it. Such a frame is shown 4:3 (in every room view of the 51-room test the window
		 * matches the map on every frame: the map is the streamers' source) */
		long bad = g_reason == WS_OK ? build_names(v, cam_x, rcam, rs) : 0;
		if (bad) {
			g_reason = WS_PLANES_NOT_MAP;
			c_mismatch += bad;
			c_mismatch_frames++;
			if (rs)
				rs->mismatch += bad;
		}
	}
	c_reason[g_reason]++;
	if (rs)
		rs->reason[g_reason]++;
	static int dbg = -1;
	static ws_reason last_reason = WS_REASONS;
	if (dbg < 0)
		dbg = getenv("SCOOBY_WS_DEBUG") != NULL;
	if (dbg && g_reason != last_reason)    /* SCOOBY_WS_DEBUG=1: every change of the screen rule */
		fprintf(stderr, "ws %ld: %s (room w%d r%02d, camera %d; hscroll %04X %04X vscroll %04X %04X)\n",
		        emu_frame_count(), ws_rules[g_reason].name, emu_ram16(MYSTERY_IDX), emu_ram16(ROOM_INDEX), cam_x,
		        vram_word(v, ((uint32_t)v->reg[13] << 10) & 0xFC00), vram_word(v, (((uint32_t)v->reg[13] << 10) & 0xFC00) + 2),
		        v->vsram[0], v->vsram[1]);
	last_reason = g_reason;
	if (g_reason != WS_OK) {
		if (was)
			c_flips++;
		return;
	}
	if (!was && c_frames > 1)
		c_flips++;
	g_frame_wide = true;
	c_wide++;
	if (rs)
		rs->wide++;
	g_cur.x0 = rcam - cam_x;
	g_bar_l = rcam < 0 ? -rcam : 0;
	g_bar_r = rw - rcam < vw ? rw - rcam : vw;
	if (g_bar_l > 0 || g_bar_r < vw) {
		c_bar_frames++;
		if (rs)
			rs->bars++;
		if (rw >= vw) {                      /* a bar although the room fills the view */
			c_unneeded_bars++;
			if (rs)
				rs->unneeded++;
		}
	}
	g_view.x0 = g_cur.x0;
	g_view.cell_ofs = rcam & 7;
	g_view.width = vw;
	g_view.names[0] = g_names[0];
	g_view.names[1] = g_names[1];
	g_view.from_vram[0] = g_from_vram[0];
	g_view.from_vram[1] = g_from_vram[1];
	g_view.sprites_wrapped = g_view.sprites_lost = 0;
}

bool ws_line_wide(const render_vdp *v, const render_band *band, int line)
{
	if (!g_frame_wide || line < SCREEN_TOP)
		return false;
	int end = band && band->line >= 0 ? band->line : BAR_LINE;
	if (line >= end)
		return false;
	return !render_line_is_window(v, band, line);
}

render_view *ws_view(void) { return &g_view; }

void ws_count_centre(bool differs)
{
	c_centre_lines++;
	c_centre_bad += differs;
}

void ws_bars(int *l, int *r)
{
	*l = g_bar_l;
	*r = g_bar_r;
}

void ws_frame_end(void)
{
	g_cur.wide = g_frame_wide;
	if (g_frame_wide) {
		c_wrapped += g_view.sprites_wrapped;
		c_lost += g_view.sprites_lost;
		c_wrapped_frames += g_view.sprites_wrapped > 0;
		if (g_view.sprites_wrapped) {
			room_stat *rs = room_entry();
			if (rs) {
				rs->wrapped += g_view.sprites_wrapped;
				rs->lost += g_view.sprites_lost;
			}
		}
	}
	g_last = g_cur;
}

/* ------------------------------------------------------------------ mouse mapping */
static bool room_line(int y) { return y >= SCREEN_TOP && y < BAR_LINE; }

void ws_frame_to_game(int fx, int fy, int *gx, int *gy)
{
	*gy = fy;
	if (!g_last.on) {
		*gx = fx;
		return;
	}
	/* the room band of a wide frame maps through the view; everything else is the centred 4:3 */
	*gx = g_last.wide && room_line(fy) ? fx + g_last.x0 : fx - g_last.centre;
}

void ws_game_to_frame(int gx, int gy, int *fx, int *fy)
{
	*fy = gy;
	if (!g_last.on) {
		*fx = gx;
		return;
	}
	*fx = g_last.wide && room_line(gy) ? gx - g_last.x0 : gx + g_last.centre;
}

/* ------------------------------------------------------------------ report */
void ws_report(void)
{
	const char *e = getenv("SCOOBY_WS_STATS");
	if (!g_stats && !(e && atoi(e)))
		return;
	fflush(stdout);
	fprintf(stderr, "ws: width %d, frames %ld, wide %ld, flips %ld\n", g_width, c_frames, c_wide, c_flips);
	for (int i = 1; i < WS_REASONS; i++)
		if (c_reason[i])
			fprintf(stderr, "ws: 4:3 %-15s %ld frames (%s)\n", ws_rules[i].name, c_reason[i], ws_rules[i].why);
	fprintf(stderr, "ws: terrain: %ld of %ld 4:3-window cells differ from the room map (%ld frames, shown 4:3); "
	        "%ld stale margin cells in VRAM (replaced from the map)\n", c_mismatch, c_cells, c_mismatch_frames, c_stale);
	fprintf(stderr, "ws: sprites: %ld wrapped pieces hidden (%ld frames), %ld lost\n", c_wrapped, c_wrapped_frames, c_lost);
	fprintf(stderr, "ws: bars: %ld frames with bars, %ld unneeded\n", c_bar_frames, c_unneeded_bars);
	fprintf(stderr, "ws: centre: %ld of %ld wide lines differ from the 4:3 line\n", c_centre_bad, c_centre_lines);
	for (int i = 0; i < g_nrooms; i++) {
		room_stat *r = &g_rooms[i];
		char why[256] = "";
		for (int k = 1; k < WS_REASONS; k++)
			if (r->reason[k])
				snprintf(why + strlen(why), sizeof why - strlen(why), " %s=%ld", ws_rules[k].name, r->reason[k]);
		fprintf(stderr, "ws-room w%d r%02d: frames %ld wide %ld mismatch %ld wrapped %ld lost %ld bars %ld unneeded %ld stale %ld;%s\n",
		        r->myst, r->room, r->frames, r->wide, r->mismatch, r->wrapped, r->lost, r->bars, r->unneeded, r->stale,
		        why[0] ? why : " -");
	}
}
