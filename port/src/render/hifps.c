/* High frame rate presentation (GitLab #27), see hifps.h and docs/subsystems/hifps.md.
 *
 * Per logic frame k (hifps_frame_end, called when the emulator has drawn R(k) in raster mode):
 *   1. the record of R(k): the VDP snapshot taken at line 0 (what the display used), the h-scroll
 *      table entries of the room band's lines, the full-screen v-scroll, the owners of the actor
 *      sprites (announced by sprite_build in the VBlank before the display) and the widescreen
 *      state of the frame;
 *   2. the plan against R(k-1): per h-scroll entry and plane the step, the v-scroll step, per
 *      owner the anchor step; big steps snap; a frame pair that is not consecutive gameplay in
 *      the same room (or whose owners are unknown) is not interpolated;
 *   3. picture j of n (hifps_frame): R(k)'s snapshot with every step s moved back by
 *      round((1 - j/n) x s), rendered in frame mode (4:3, or through the wide view at the moved
 *      camera); with no motion the picture is R(k-1) itself (what 60 Hz shows until k x T).
 * Before the first interpolated picture of a frame, the snapshot is rendered with no step and
 * compared with the raster frame R(k): if the game changed the VDP during the display (beyond
 * the verb-bar band), frame mode cannot reproduce R(k) and the frame is held instead. */
#include "hifps.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "emu.h"
#include "gx.h"
#include "render_emu.h"
#include "widescreen.h"

/* game RAM */
#define VBLANK_HOOK   0xFF0000   /* l: VBlank work routine */
#define VBLANK_MAIN   0x00A65C   /* the in-game VBlank handler */
#define MYSTERY_IDX   0xFF06AA
#define ROOM_INDEX    0xFF06AC
#define SAT_BUF       0xFF0008   /* sprite_build's table, DMA'd to the VDP's sprite table */

#define SNAP_SCROLL   64         /* px: a bigger scroll step is a cut, not motion */
#define SNAP_SPRITE   48         /* px: a bigger anchor step is a teleport */
#define MAX_SPRITES   80
#define MAX_OWNERS    6

/* ------------------------------------------------------------------ settings */
static int g_rate;               /* Hz, 0 = off */
static bool g_eager;
static bool g_stats_env;

/* ------------------------------------------------------------------ announcements */
typedef struct { int slot, first, count; int32_t wx, wy; } owner_t;    /* world position 16.16 */
typedef struct { owner_t own[MAX_OWNERS]; int n, total, cam_x, cam_y; bool done; } ann_t;
static ann_t g_ann_build, g_ann_ready;

void hifps_spr_begin(int cam_x, int cam_y)
{
	g_ann_build.n = 0;
	g_ann_build.done = false;
	g_ann_build.cam_x = cam_x;
	g_ann_build.cam_y = cam_y;
}

void hifps_spr_actor(int slot, int first, int count, int32_t wx, int32_t wy)
{
	if (g_ann_build.n < MAX_OWNERS)
		g_ann_build.own[g_ann_build.n++] = (owner_t){ slot, first, count, wx, wy };
}

void hifps_spr_end(int total)
{
	g_ann_build.total = total;
	g_ann_build.done = true;
	g_ann_ready = g_ann_build;
}

/* ------------------------------------------------------------------ frame records */
typedef struct {
	bool valid;
	long frame;
	bool gameplay;               /* VBlank hook = vblank_main at line 0 */
	int room;                    /* mystery << 8 | room */
	int width, height;           /* VDP width / lines */
	uint8_t reg[24];
	uint16_t hs[RENDER_MAX_H][2];/* h-scroll table entries (A, B), by entry index */
	uint16_t vs[2];
	bool room_line[RENDER_MAX_H];
	ann_t ann;
	bool ann_ok;                 /* owners known and the VDP's sprite table is sprite_build's */
	uint64_t sat_hash;           /* the sprite table as the display uses it */
	bool ws_frame, wide;         /* drawn into the wide frame / through the wide view */
	int cam;                     /* wide: the frame's camera x */
	int out_w, out_h;
	render_band band;
} rec_t;
static rec_t g_prev, g_cur;

/* snapshot of the VDP at line 0 of the current frame */
static uint16_t s_vram[0x8000], s_cram[64], s_vsram[40], s_sat[0x200];
static uint8_t s_reg[0x20];
static int s_width;
static bool s_ok;
static ann_t s_ann;
static bool s_gameplay;
static int s_room;

/* images: R(k) and R(k-1) as shown (the whole frames) */
static uint32_t g_img[2][RENDER_MAX_W * RENDER_MAX_H];
static int g_icur;               /* g_img[g_icur] = R(k), the other R(k-1) */

/* the plan R(k-1) -> R(k) */
enum { WHY_MOTION, WHY_STILL, WHY_FIRST, WHY_NOT_GAMEPLAY, WHY_ROOM, WHY_LAYOUT, WHY_OWNERS, WHY_FRAMEMODE, WHY_N };
static const char *why_name[WHY_N] = { "interpolated", "still (nothing moved)", "no previous frame / discontinuity",
	"not gameplay (VBlank hook is not vblank_main)", "room changed", "VDP layout / frame size / wide view changed",
	"sprite owners unknown (68k sprite_build or SAT not sprite_build's)", "frame mode differs from the raster frame (mid-frame VDP writes)" };
/* A moving actor: drawn at screen (w >> 16) - camera; in between, its world position follows
 * the 16.16 line from R(k-1)'s to R(k)'s, so the pixel steps fall evenly in time. */
typedef struct { int first, count; int32_t w0[2], w1[2]; } move_t;
/* The camera of an axis in between: the integer line from R(k-1)'s to R(k)'s, or, when it moved
 * with Shaggy (camera_follow keeps him in a dead zone: camera = his pixel - a constant), his
 * 16.16 line minus that constant, so the background scrolls at his sub-pixel speed. */
typedef struct { int c0, c1; bool locked; int32_t s0, s1; int lock_ofs; } cam_t;
enum { SC_NONE, SC_LERP, SC_CAMERA };     /* how a plane's scroll entry moves */
static struct {
	int why;
	bool hent[RENDER_MAX_H];      /* h-scroll entry used by a room line */
	uint8_t hmode[RENDER_MAX_H][2];
	int16_t dh[RENDER_MAX_H][2];  /* SC_LERP: the step */
	uint8_t vmode[2];
	int16_t dv[2];
	bool have_cam;
	cam_t cam[2];                 /* x, y */
	move_t mv[MAX_OWNERS];
	int nmv;
	bool zero_done;
} g_plan;

/* work buffers of an in-between picture */
static uint16_t w_vram[0x8000], w_vsram[40], w_sat[0x200];
static uint32_t g_out[RENDER_MAX_W * RENDER_MAX_H];
static render_ctx g_rc, g_rcw;

/* counters */
static long c_frames, c_why[WHY_N], c_pieces, c_matched, c_new, c_snapped, c_moving, c_scroll_snaps;
static long c_gp_frames, c_renders, c_zero, c_zero_bad;
static long c_rt_hist[2001];     /* render time per in-between, 10 us buckets (last: >= 20 ms) */
static double c_rt_max;

/* dumps (SCOOBY_HIFPS_DUMP=DIR[,FROM-TO]) and the whole-frame hash log (SCOOBY_FRAME_HASH_LOG) */
static char g_dump_dir[512];
static long g_dump_from, g_dump_to = -1;
static FILE *g_hash_log;
static bool g_env_read;

static void report_atexit(void) { hifps_report(); }

static void read_env(void)
{
	if (g_env_read)
		return;
	g_env_read = true;
	const char *d = getenv("SCOOBY_HIFPS_DUMP");
	if (d && *d) {
		snprintf(g_dump_dir, sizeof g_dump_dir, "%s", d);
		char *c = strchr(g_dump_dir, ',');
		g_dump_from = 0;
		g_dump_to = 1L << 40;
		if (c) {
			*c = 0;
			sscanf(c + 1, "%ld-%ld", &g_dump_from, &g_dump_to);
		}
		mkdir(g_dump_dir, 0777);
	}
	const char *h = getenv("SCOOBY_FRAME_HASH_LOG");
	if (h && *h)
		g_hash_log = fopen(h, "w");
	g_stats_env = getenv("SCOOBY_HIFPS_STATS") && atoi(getenv("SCOOBY_HIFPS_STATS"));
	atexit(report_atexit);
}

void hifps_set_rate(int hz)
{
	read_env();
	g_rate = hz > 60 ? hz : 0;
	hifps_reset();
}

int hifps_rate(void) { return g_rate; }

int hifps_steps(void)
{
	if (!g_rate)
		return 1;
	int n = (int)lround(g_rate / emu_fps());
	return n < 1 ? 1 : n;
}

bool hifps_active(void) { return g_rate > 0 && hifps_steps() > 1 && render_emu_kind() == RENDERER_NATIVE; }
void hifps_set_eager(bool on) { g_eager = on; }

void hifps_reset(void)
{
	g_prev.valid = g_cur.valid = false;
	s_ok = false;
	g_ann_ready.done = false;        /* a loaded state's sprite table is not the last build's */
}

/* ------------------------------------------------------------------ helpers */
static inline uint16_t sw16(const uint16_t *vram, uint32_t a) { return vram[(a & 0xFFFF) >> 1]; }
static inline int wrap(int d, int m) { d %= m; if (d >= m / 2) d -= m; if (d < -m / 2) d += m; return d; }
static uint32_t hscroll_base(const uint8_t *reg) { return ((uint32_t)reg[13] << 10) & 0xFC00; }
static uint32_t sat_base(const uint8_t *reg)
{
	return ((uint32_t)reg[5] << 9) & ((reg[12] & 1) ? 0xFC00 : 0xFE00);
}
/* h-scroll entry of a line (an index: byte offset / 4) */
static int hs_entry(const uint8_t *reg, int line)
{
	static const uint8_t mask[4] = { 0x00, 0x07, 0xF8, 0xFF };
	return line & mask[reg[11] & 3];
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------ line 0: snapshot */
void hifps_line0(const render_vdp *v)
{
	if (!hifps_active())
		return;
	memcpy(s_vram, v->vram, sizeof s_vram);
	memcpy(s_cram, v->cram, sizeof s_cram);
	memcpy(s_vsram, v->vsram, sizeof s_vsram);
	memcpy(s_sat, v->sat, sizeof s_sat);
	memcpy(s_reg, v->reg, 24);
	s_width = v->width;
	s_gameplay = emu_ram32(VBLANK_HOOK) == VBLANK_MAIN;
	s_room = emu_ram16(MYSTERY_IDX) << 8 | (emu_ram16(ROOM_INDEX) & 0xFF);
	/* the owners of the sprite table the display uses: sprite_build's last run (in the VBlank
	 * just before). Valid only if the VDP's table is that run's output, entry for entry. */
	s_ann = g_ann_ready;
	g_ann_ready.done = false;        /* used once: a frame without a C sprite_build run has no owners */
	s_ok = true;
}

static bool sat_matches_build(const ann_t *a)
{
	if (!a->done || a->total <= 0 || a->total > MAX_SPRITES) {
		if (getenv("SCOOBY_HIFPS_DEBUG") && s_gameplay)
			fprintf(stderr, "hifps %ld: no sprite_build run (done %d, total %d)\n", emu_frame_count(), a->done, a->total);
		return false;
	}
	uint32_t satb = sat_base(s_reg);
	for (int i = 0; i < a->total; i++) {
		uint32_t r = SAT_BUF + 8 * (uint32_t)i, s = satb + 8 * (uint32_t)i;
		if (sw16(s_vram, s) != emu_ram16(r) || sw16(s_vram, s + 4) != emu_ram16(r + 4) ||
		    sw16(s_vram, s + 6) != emu_ram16(r + 6) || (s_sat[4 * i] & 0x3FF) != (emu_ram16(r) & 0x3FF)) {
			if (getenv("SCOOBY_HIFPS_DEBUG"))
				fprintf(stderr, "hifps %ld: SAT entry %d of %d is not sprite_build's (vram %04X %04X %04X cache %04X, ram %04X %04X %04X)\n",
				        emu_frame_count(), i, a->total, sw16(s_vram, s), sw16(s_vram, s + 4), sw16(s_vram, s + 6),
				        s_sat[4 * i], emu_ram16(r), emu_ram16(r + 4), emu_ram16(r + 6));
			return false;
		}
	}
	return true;
}

/* ------------------------------------------------------------------ the plan */
static bool same_layout(const rec_t *a, const rec_t *b)
{
	static const int regs[] = { 0, 1, 2, 3, 4, 5, 11, 12, 13, 16, 17 };
	for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
		if (a->reg[regs[i]] != b->reg[regs[i]])
			return false;
	return a->width == b->width && a->height == b->height && a->ws_frame == b->ws_frame &&
	       a->wide == b->wide && a->out_w == b->out_w && a->out_h == b->out_h;
}

static void plan(void)
{
	memset(&g_plan, 0, sizeof g_plan);
	const rec_t *p = &g_prev, *c = &g_cur;
	if (!p->valid || p->frame != c->frame - 1) { g_plan.why = WHY_FIRST; return; }
	if (!p->gameplay || !c->gameplay) { g_plan.why = WHY_NOT_GAMEPLAY; return; }
	if (p->room != c->room) { g_plan.why = WHY_ROOM; return; }
	if (!same_layout(p, c)) { g_plan.why = WHY_LAYOUT; return; }
	/* without owners (no sprite_build: boot, title, a room load) the sprites can only stay
	 * where they are, which is right only if the table did not change: then they are
	 * fixed on the screen at 60 Hz too */
	bool owners = p->ann_ok && c->ann_ok;
	if (!owners && p->sat_hash != c->sat_hash) { g_plan.why = WHY_OWNERS; return; }
	bool motion = false;
	/* the camera (sprite_build's, the same VBlank as camera_update's scroll) */
	g_plan.have_cam = owners && p->ann.done && c->ann.done;
	const owner_t *sh0 = NULL, *sh1 = NULL;          /* Shaggy (slot 0) in both frames */
	for (int k = 0; owners && k < p->ann.n; k++)
		if (p->ann.own[k].slot == 0)
			sh0 = &p->ann.own[k];
	for (int k = 0; owners && k < c->ann.n; k++)
		if (c->ann.own[k].slot == 0)
			sh1 = &c->ann.own[k];
	if (g_plan.have_cam)
		for (int ax = 0; ax < 2; ax++) {
			cam_t *cm = &g_plan.cam[ax];
			cm->c0 = ax ? p->ann.cam_y : p->ann.cam_x;
			cm->c1 = ax ? c->ann.cam_y : c->ann.cam_x;
			if (abs(cm->c1 - cm->c0) > SNAP_SCROLL) {
				cm->c0 = cm->c1;                     /* a cut */
				c_scroll_snaps++;
			}
			if (sh0 && sh1 && cm->c1 != cm->c0) {
				int32_t s0 = ax ? sh0->wy : sh0->wx, s1 = ax ? sh1->wy : sh1->wx;
				if ((s1 >> 16) - (s0 >> 16) == cm->c1 - cm->c0) {
					cm->locked = true;
					cm->s0 = s0;
					cm->s1 = s1;
					cm->lock_ofs = (s1 >> 16) - cm->c1;
				}
			}
		}
	/* the planes: every h-scroll entry a room line uses; an entry that moved by the camera's
	 * step follows the camera, any other step is a straight line */
	for (int l = 0; l < c->height; l++) {
		if (!c->room_line[l])
			continue;
		int e = hs_entry(c->reg, l);
		if (g_plan.hent[e])
			continue;
		g_plan.hent[e] = true;
		for (int pl = 0; pl < 2; pl++) {
			int d = wrap((int)c->hs[e][pl] - (int)p->hs[e][pl], 512);
			if (!d)
				continue;
			if (g_plan.have_cam && d == wrap(-(c->ann.cam_x - p->ann.cam_x), 512)) {
				g_plan.hmode[e][pl] = g_plan.cam[0].c0 != g_plan.cam[0].c1 ? SC_CAMERA : SC_NONE;
			} else if (abs(d) > SNAP_SCROLL) {
				c_scroll_snaps++;
			} else {
				g_plan.hmode[e][pl] = SC_LERP;
				g_plan.dh[e][pl] = (int16_t)d;
			}
			motion |= g_plan.hmode[e][pl] != SC_NONE;
		}
	}
	if (!(c->reg[11] & 4))          /* full-screen v-scroll (= camera y, + the screen shake) */
		for (int pl = 0; pl < 2; pl++) {
			int d = wrap((int)(c->vs[pl] & 0x3FF) - (int)(p->vs[pl] & 0x3FF), 256);
			if (!d)
				continue;
			if (g_plan.have_cam && d == wrap(c->ann.cam_y - p->ann.cam_y, 256)) {
				g_plan.vmode[pl] = g_plan.cam[1].c0 != g_plan.cam[1].c1 ? SC_CAMERA : SC_NONE;
			} else if (abs(d) > SNAP_SCROLL) {
				c_scroll_snaps++;
			} else {
				g_plan.vmode[pl] = SC_LERP;
				g_plan.dv[pl] = (int16_t)d;
			}
			motion |= g_plan.vmode[pl] != SC_NONE;
		}
	/* the actors: owner + anchor */
	if (owners)
		c_gp_frames++;
	for (int i = 0; owners && i < c->ann.n; i++) {
		const owner_t *o = &c->ann.own[i];
		c_pieces += o->count;
		const owner_t *q = NULL;
		for (int k = 0; k < p->ann.n; k++)
			if (p->ann.own[k].slot == o->slot)
				q = &p->ann.own[k];
		if (!q) {
			c_new += o->count;          /* not drawn in R(k-1): appears at its place */
			continue;
		}
		int dx = (o->wx >> 16) - (q->wx >> 16), dy = (o->wy >> 16) - (q->wy >> 16);
		if (abs(dx) > SNAP_SPRITE || abs(dy) > SNAP_SPRITE) {
			c_snapped += o->count;
			continue;
		}
		c_matched += o->count;
		bool cam_moved = g_plan.cam[0].c0 != g_plan.cam[0].c1 || g_plan.cam[1].c0 != g_plan.cam[1].c1;
		if (o->wx != q->wx || o->wy != q->wy || cam_moved) {
			if (dx || dy || cam_moved)
				c_moving += o->count;
			g_plan.mv[g_plan.nmv++] = (move_t){ o->first, o->count, { q->wx, q->wy }, { o->wx, o->wy } };
			motion |= dx || dy;
		}
	}
	g_plan.why = motion ? WHY_MOTION : WHY_STILL;
	static int dbg = -1;
	if (dbg < 0)
		dbg = getenv("SCOOBY_HIFPS_DEBUG") ? atoi(getenv("SCOOBY_HIFPS_DEBUG")) : 0;
	if (dbg >= 2) {                   /* SCOOBY_HIFPS_DEBUG=2: every frame's steps */
		char buf[300];
		int n = snprintf(buf, sizeof buf, "hifps %ld: %s cam %d->%d%s %d->%d%s; scroll %d/%d v %d/%d;", c->frame,
		                 why_name[g_plan.why], g_plan.cam[0].c0, g_plan.cam[0].c1, g_plan.cam[0].locked ? "L" : "",
		                 g_plan.cam[1].c0, g_plan.cam[1].c1, g_plan.cam[1].locked ? "L" : "",
		                 g_plan.hmode[0][0], g_plan.hmode[0][1], g_plan.vmode[0], g_plan.vmode[1]);
		for (int m = 0; m < g_plan.nmv && n < (int)sizeof buf - 40; m++)
			n += snprintf(buf + n, sizeof buf - n, " spr %d+%d (%.2f,%.2f)", g_plan.mv[m].first, g_plan.mv[m].count,
			              (g_plan.mv[m].w1[0] - g_plan.mv[m].w0[0]) / 65536.0, (g_plan.mv[m].w1[1] - g_plan.mv[m].w0[1]) / 65536.0);
		fprintf(stderr, "%s\n", buf);
	}
}

/* ------------------------------------------------------------------ an in-between picture */
static inline uint32_t black(void) { return render_rgba(0, 0, 0); }

/* a 16.16 point of the line from a to b at f, as a pixel (floor) */
static int lerp_px(int32_t a, int32_t b, double f)
{
	return (int)floor((a + f * ((double)b - a)) / 65536.0);
}

/* the camera of an axis at f (0 = R(k-1), 1 = R(k)) */
static int cam_at(const cam_t *cm, double f)
{
	if (cm->locked)
		return lerp_px(cm->s0, cm->s1, f) - cm->lock_ofs;
	return cm->c1 - (int)lround((1.0 - f) * (cm->c1 - cm->c0));
}

/* R(k)'s snapshot moved to the moment f between R(k-1) (0) and R(k) (1), rendered into g_out */
static void render_at(double f)
{
	const rec_t *c = &g_cur;
	memcpy(w_vram, s_vram, sizeof w_vram);
	memcpy(w_vsram, s_vsram, sizeof w_vsram);
	memcpy(w_sat, s_sat, sizeof w_sat);
	int cx = g_plan.have_cam ? cam_at(&g_plan.cam[0], f) : 0;
	int cy = g_plan.have_cam ? cam_at(&g_plan.cam[1], f) : 0;
	int ocx = g_plan.have_cam ? g_plan.cam[0].c1 - cx : 0;    /* camera behind R(k)'s by */
	int ocy = g_plan.have_cam ? g_plan.cam[1].c1 - cy : 0;
	uint32_t hb = hscroll_base(c->reg);
	int ob = 0;                         /* plane B's scroll offset at entry 0 (the wide camera) */
	for (int e = 0; e < RENDER_MAX_H; e++) {
		if (!g_plan.hent[e])
			continue;
		for (int pl = 0; pl < 2; pl++) {
			int o = 0;                  /* subtracted from R(k)'s h-scroll (= -camera) */
			if (g_plan.hmode[e][pl] == SC_LERP)
				o = (int)lround((1.0 - f) * g_plan.dh[e][pl]);
			else if (g_plan.hmode[e][pl] == SC_CAMERA)
				o = -ocx;
			uint32_t a = (hb + 4 * (uint32_t)e + 2 * (uint32_t)pl) & 0xFFFF;
			w_vram[a >> 1] = (uint16_t)(w_vram[a >> 1] - o);
			if (pl == 1 && e == 0)
				ob = o;
		}
	}
	for (int pl = 0; pl < 2; pl++) {
		int o = 0;
		if (g_plan.vmode[pl] == SC_LERP)
			o = (int)lround((1.0 - f) * g_plan.dv[pl]);
		else if (g_plan.vmode[pl] == SC_CAMERA)
			o = ocy;
		w_vsram[pl] = (uint16_t)(w_vsram[pl] - o);
	}
	uint32_t satb = sat_base(c->reg);
	for (int m = 0; m < g_plan.nmv; m++) {
		const move_t *mv = &g_plan.mv[m];
		/* screen position at f minus the one R(k) shows: (world - camera) at f vs at 1 */
		int ox = ((mv->w1[0] >> 16) - g_plan.cam[0].c1) - (lerp_px(mv->w0[0], mv->w1[0], f) - cx);
		int oy = ((mv->w1[1] >> 16) - g_plan.cam[1].c1) - (lerp_px(mv->w0[1], mv->w1[1], f) - cy);
		if (!ox && !oy)
			continue;
		for (int i = mv->first; i < mv->first + mv->count && i < MAX_SPRITES; i++) {
			uint32_t a = satb + 8 * (uint32_t)i;
			uint16_t x0 = w_vram[((a + 6) & 0xFFFF) >> 1], x = (uint16_t)(x0 - ox);
			if (!(x & 0x1FF) && (x0 & 0x1FF))
				x++;                        /* x = 0 is the VDP's sprite mask: keep it the game's */
			w_vram[((a + 6) & 0xFFFF) >> 1] = x;
			uint16_t y = (uint16_t)(w_vram[(a & 0xFFFF) >> 1] - oy);
			w_vram[(a & 0xFFFF) >> 1] = y;
			w_sat[4 * i] = (uint16_t)((w_sat[4 * i] & ~0x3FF) | (y & 0x3FF));
		}
	}
	render_vdp v = { w_vram, s_cram, w_vsram, s_reg, w_sat, s_width };
	const render_band *band = &c->band;
	render_view view;
	int bl = 0, br = 0;
	bool wide = c->wide && ws_interp_view(&v, c->cam + ob, &view, &bl, &br);
	int W = c->out_w, sw = s_width;
	render_ctx_reset(&g_rc);
	render_ctx_reset(&g_rcw);
	bool on = s_reg[1] & 0x40;
	if (on) {
		render_parse_sprites(&g_rc, &v, -1);
		if (c->ws_frame)
			render_parse_sprites(&g_rcw, &v, -1);
	}
	uint32_t line43[RENDER_MAX_W];
	for (int l = 0; l < c->height && l < RENDER_MAX_H; l++) {
		uint32_t *dst = g_out + (size_t)l * RENDER_MAX_W;
		render_line_pixels(&g_rc, &v, band, l);
		/* the wide view's room lines, as ws_line_wide decides them for the raster frame */
		bool wl = wide && l >= 16 && l < (band->line >= 0 ? band->line : 168) &&
		          !render_line_is_window(&v, band, l);
		if (wl)
			render_line_pixels_view(&g_rcw, &v, band, l, &view);
		if (on && l < c->height - 1) {
			render_parse_sprites(&g_rc, &v, l);
			if (c->ws_frame)
				render_parse_sprites(&g_rcw, &v, l);
		}
		if (!c->ws_frame) {
			render_line_colors(&g_rc, &v, dst);
			continue;
		}
		if (wl) {
			render_line_colors(&g_rcw, &v, dst);
			for (int x = 0; x < bl; x++)
				dst[x] = black();
			for (int x = br; x < W; x++)
				dst[x] = black();
		} else {
			render_line_colors(&g_rc, &v, line43);
			int cx = (W - sw) / 2;
			for (int x = 0; x < cx; x++)
				dst[x] = black();
			memcpy(dst + cx, line43, (size_t)sw * 4);
			for (int x = cx + sw; x < W; x++)
				dst[x] = black();
		}
	}
}

static bool same_image(const uint32_t *a, const uint32_t *b, int w, int h)
{
	for (int y = 0; y < h; y++)
		if (memcmp(a + (size_t)y * RENDER_MAX_W, b + (size_t)y * RENDER_MAX_W, (size_t)w * 4))
			return false;
	return true;
}

/* the first time a frame is interpolated: frame mode with no step must give the raster frame */
static void zero_check(void)
{
	if (g_plan.zero_done)
		return;
	g_plan.zero_done = true;
	render_at(1.0);
	c_zero++;
	if (!same_image(g_out, g_img[g_icur], g_cur.out_w, g_cur.out_h)) {
		c_zero_bad++;
		c_why[WHY_MOTION]--;
		c_why[WHY_FRAMEMODE]++;
		g_plan.why = WHY_FRAMEMODE;
		if (getenv("SCOOBY_HIFPS_DEBUG"))
			fprintf(stderr, "hifps %ld: frame mode differs from the raster frame: held\n", g_cur.frame);
	}
}

const uint32_t *hifps_frame(int j, int n, int *w, int *h, bool *moved)
{
	if (moved)
		*moved = false;
	if (!g_cur.valid || n < 2 || j <= 0 || j >= n)
		return NULL;
	*w = g_cur.out_w;
	*h = g_cur.out_h;
	if (g_plan.why == WHY_MOTION)
		zero_check();
	if (g_plan.why != WHY_MOTION)
		return g_prev.valid ? g_img[g_icur ^ 1] : g_img[g_icur];   /* R(k-1), as 60 Hz shows it */
	double t0 = now_ms();
	render_at((double)j / n);
	double dt = now_ms() - t0;
	int b = (int)(dt * 100);
	c_rt_hist[b < 2000 ? b : 2000]++;
	if (dt > c_rt_max)
		c_rt_max = dt;
	c_renders++;
	if (moved)
		*moved = true;
	return g_out;
}

/* ------------------------------------------------------------------ frame end */
static void write_ppm(const char *path, const uint32_t *px, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t c = px[(size_t)y * RENDER_MAX_W + x];
			uint8_t rgb[3] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16) };
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
}

static uint64_t image_hash(const uint32_t *img, int w, int h)
{
	uint64_t x = 0xCBF29CE484222325ull;
	for (int y = 0; y < h; y++)
		for (int i = 0; i < w; i++) {
			uint32_t c = img[(size_t)y * RENDER_MAX_W + i] & 0xFFFFFF;
			x = (x ^ c) * 0x100000001B3ull;
		}
	return x ^ (uint64_t)w << 48 ^ (uint64_t)h << 32;
}

void hifps_frame_end(long frame, const render_band *band, const uint32_t *img, int w, int h)
{
	read_env();
	if (g_hash_log && img) {
		fprintf(g_hash_log, "%ld %016llx\n", frame, (unsigned long long)image_hash(img, w, h));
		if (!(frame & 255))
			fflush(g_hash_log);
	}
	if (!hifps_active() || !img || !s_ok || w > RENDER_MAX_W || h > RENDER_MAX_H) {
		g_prev.valid = g_cur.valid = false;
		s_ok = false;
		return;
	}
	s_ok = false;
	/* R(k) becomes R(k-1) */
	g_prev = g_cur;
	g_icur ^= 1;
	rec_t *c = &g_cur;
	c->valid = true;
	c->frame = frame;
	c->gameplay = s_gameplay;
	c->room = s_room;
	c->width = s_width;
	c->height = h;
	memcpy(c->reg, s_reg, 24);
	c->band = *band;
	uint32_t hb = hscroll_base(s_reg);
	render_vdp v = { s_vram, s_cram, s_vsram, s_reg, s_sat, s_width };
	memset(c->room_line, 0, sizeof c->room_line);
	for (int l = 0; l < h; l++) {
		int e = hs_entry(s_reg, l);
		c->hs[e][0] = sw16(s_vram, hb + 4 * (uint32_t)e);
		c->hs[e][1] = sw16(s_vram, hb + 4 * (uint32_t)e + 2);
		/* a room line: planes shown with the frame's own scroll (not the verb-bar band, not
		 * covered or split by the window: name band, text boxes) */
		c->room_line[l] = (band->line < 0 || l < band->line) && !render_line_is_window(&v, band, l);
	}
	c->vs[0] = s_vsram[0];
	c->vs[1] = s_vsram[1];
	c->ann = s_ann;
	c->ann_ok = sat_matches_build(&s_ann);
	uint64_t x = 0xCBF29CE484222325ull;
	uint32_t satb = sat_base(s_reg);
	for (int i = 0; i < MAX_SPRITES * 4; i++)
		x = (x ^ sw16(s_vram, satb + 2 * (uint32_t)i) ^ (uint64_t)s_sat[i] << 16) * 0x100000001B3ull;
	c->sat_hash = x;
	int ww = 0, cam = 0;
	c->wide = ws_last_wide(&ww, &cam);
	c->ws_frame = w > s_width;
	c->cam = cam;
	c->out_w = w;
	c->out_h = h;
	for (int y = 0; y < h; y++)
		memcpy(g_img[g_icur] + (size_t)y * RENDER_MAX_W, img + (size_t)y * RENDER_MAX_W, (size_t)w * 4);
	plan();
	c_frames++;
	c_why[g_plan.why]++;

	bool dump = g_dump_dir[0] && frame >= g_dump_from && frame <= g_dump_to;
	if (g_eager || dump) {
		int n = hifps_steps();
		char p[700];
		for (int j = 1; j < n; j++) {
			int fw, fh;
			const uint32_t *px = hifps_frame(j, n, &fw, &fh, NULL);
			if (px && dump) {
				/* named by presentation order: picture j of the span (k-1) -> k */
				snprintf(p, sizeof p, "%s/s%07ld_%02d.ppm", g_dump_dir, frame - 1, j);
				write_ppm(p, px, fw, fh);
			}
		}
		if (dump) {
			snprintf(p, sizeof p, "%s/s%07ld_00.ppm", g_dump_dir, frame);
			write_ppm(p, img, w, h);
		}
	}
}

/* ------------------------------------------------------------------ report */
static double pct(long a, long b) { return b ? 100.0 * a / b : 0.0; }

static double rt_quantile(double q)
{
	long tot = 0;
	for (int i = 0; i <= 2000; i++)
		tot += c_rt_hist[i];
	if (!tot)
		return 0;
	long want = (long)ceil(q * tot), acc = 0;
	for (int i = 0; i <= 2000; i++) {
		acc += c_rt_hist[i];
		if (acc >= want)
			return (i + 1) / 100.0;
	}
	return 20.0;
}

void hifps_report(void)
{
	if (!c_frames || !(g_eager || g_stats_env))
		return;
	fflush(stdout);
	fprintf(stderr, "hifps: rate %d Hz, %d pictures per logic frame, %ld frames\n", g_rate, hifps_steps(), c_frames);
	for (int i = 0; i < WHY_N; i++)
		if (c_why[i])
			fprintf(stderr, "hifps: frames %-12s %ld (%s)\n",
			        i == WHY_MOTION ? "interp" : i == WHY_STILL ? "still" : "held", c_why[i], why_name[i]);
	fprintf(stderr, "hifps: sprites: %ld actor pieces on %ld gameplay frames, %ld matched (%.2f%%), %ld moving, "
	        "%ld new owner, %ld snapped (> %d px)\n", c_pieces, c_gp_frames, c_matched, pct(c_matched, c_pieces),
	        c_moving, c_new, c_snapped, SNAP_SPRITE);
	fprintf(stderr, "hifps: scroll snaps (> %d px): %ld\n", SNAP_SCROLL, c_scroll_snaps);
	fprintf(stderr, "hifps: frame-mode check: %ld frames, %ld differ from the raster frame\n", c_zero, c_zero_bad);
	fprintf(stderr, "hifps: render per in-between: %ld renders, p50 %.2f ms, p99 %.2f ms, max %.2f ms\n",
	        c_renders, rt_quantile(0.5), rt_quantile(0.99), c_rt_max);
}
