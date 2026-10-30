/* Native renderer <-> emulator bridge (GitLab #25), see render_emu.h and docs/subsystems/render.md. */
#include "render_emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "emu.h"
#include "gx.h"
#include "hle.h"

/* game/frameloop.c: record the verb-bar band at the 68k hblank_verb_bar (SCOOBY_RENDER_SPLIT=model) */
void frameloop_split_model_check(bool on);

static renderer_kind g_kind = RENDERER_GPGX;
static bool g_running;            /* hooks installed, native frames produced */
static render_ctx g_rc;
static uint32_t g_fb[RENDER_MAX_W * RENDER_MAX_H];
static render_band g_band = { .line = -1 };
static int g_out_w = 256, g_out_h = 224;

/* ------------------------------------------------------------------ the live view */
static render_vdp live_view(void)
{
	render_vdp v;
	v.vram = (const uint16_t *)gx_vram();
	v.cram = (const uint16_t *)gx_cram();
	v.vsram = (const uint16_t *)gx_vsram();
	v.reg = gx_vdp_reg();
	v.sat = (const uint16_t *)gx_sat();
	v.width = gx_viewport_w();
	return v;
}

/* frame mode statistics (SCOOBY_RENDERCMP_FRAME=1): the frame drawn in one go from a snapshot
 * taken before line 0, as a CPU-less build would draw it, compared with GPGX's frame; counts how
 * often mid-frame VDP writes matter (docs/subsystems/render.md) */
static bool g_frame_stats;
static render_ctx g_rcf;
static uint16_t g_snap_vram[0x8000], g_snap_cram[64], g_snap_vsram[40], g_snap_sat[0x200];
static uint8_t g_snap_reg[0x20];
static render_scene g_snap;
static bool g_snap_ok;
static uint32_t g_ffb[RENDER_MAX_W * RENDER_MAX_H];
static long g_fm_frames, g_fm_bad, g_fm_pixels;

static void snapshot(void)
{
	memcpy(g_snap_vram, gx_vram(), sizeof g_snap_vram);
	memcpy(g_snap_cram, gx_cram(), sizeof g_snap_cram);
	memcpy(g_snap_vsram, gx_vsram(), sizeof g_snap_vsram);
	memcpy(g_snap_sat, gx_sat(), sizeof g_snap_sat);
	memcpy(g_snap_reg, gx_vdp_reg(), sizeof g_snap_reg);
	g_snap.vdp = (render_vdp){ g_snap_vram, g_snap_cram, g_snap_vsram, g_snap_reg, g_snap_sat, gx_viewport_w() };
	g_snap.height = gx_viewport_h();
	g_snap_ok = true;
}

static void on_line(int line)
{
	if (line < 0 || line >= RENDER_MAX_H)
		return;
	if (line == 0 && g_frame_stats)
		snapshot();
	render_vdp v = live_view();
	render_line_pixels(&g_rc, &v, &g_band, line);
}

static void on_parse(int line)
{
	render_vdp v = live_view();
	render_parse_sprites(&g_rc, &v, line);
}

static void on_remap(int line)
{
	if (line < 0 || line >= RENDER_MAX_H || line >= gx_viewport_h())
		return;
	render_vdp v = live_view();
	render_line_colors(&g_rc, &v, g_fb + (size_t)line * RENDER_MAX_W);
}

static void start(void)
{
	if (g_running)
		return;
	render_tables_init();
	render_ctx_reset(&g_rc);
	gx_set_render_hooks(on_line, on_parse, on_remap);
	g_running = true;
}

/* ------------------------------------------------------------------ comparison */
static int g_cmp_every;           /* 0 = off */
static char g_cmp_dir[512];
static long g_cmp_frames, g_cmp_bad, g_cmp_all, g_cmp_all_bad, g_cmp_dumps, g_cmp_split_race;
static long g_last_change = -1000;
static uint16_t g_prev[RENDER_MAX_W * RENDER_MAX_H];   /* GPGX's previous frame */
static uint32_t g_prev_hooks;
#define SCENE_WINDOW 8            /* frames after a scene change that are always checked */

static void write_ppm_rgba(const char *path, const uint32_t *px, int w, int h, int pitch)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t c = px[y * pitch + x];
			uint8_t rgb[3] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16) };
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
}

static void compare(long frame)
{
	unsigned w, h;
	size_t pitch;
	const uint16_t *g = emu_video(&w, &h, &pitch);
	if (!g)
		return;
	/* scene change: the interrupt hooks changed, or more than a third of the picture did */
	uint32_t hooks = emu_ram32(0xFF0000) ^ (emu_ram32(0xFF0004) << 1);
	long changed = 0;
	for (unsigned i = 0; i < w * h; i++)
		changed += g[i] != g_prev[i];
	if (hooks != g_prev_hooks || changed * 3 > (long)(w * h))
		g_last_change = frame;
	g_prev_hooks = hooks;
	memcpy(g_prev, g, (size_t)w * h * 2);

	long diff = 0;
	int fx = -1, fy = -1;
	bool other_rows = false;     /* a difference outside the first line of the band */
	bool dims = (int)w == g_out_w && (int)h == g_out_h;
	if (dims) {
		for (unsigned y = 0; y < h; y++)
			for (unsigned x = 0; x < w; x++)
				if (render_rgba_to_565(g_fb[y * RENDER_MAX_W + x]) != g[y * w + x]) {
					if (!diff) {
						fx = (int)x;
						fy = (int)y;
					}
					diff++;
					if (g_band.line < 0 || (int)y != g_band.line)
						other_rows = true;
				}
	}
	if (g_frame_stats && g_snap_ok && dims) {
		g_snap.band = g_band;
		render_frame(&g_rcf, &g_snap, g_ffb, RENDER_MAX_W);
		long fd = 0;
		for (unsigned y = 0; y < h; y++)
			for (unsigned x = 0; x < w; x++)
				fd += render_rgba_to_565(g_ffb[y * RENDER_MAX_W + x]) != g[y * w + x];
		g_fm_frames++;
		g_fm_bad += fd != 0;
		g_fm_pixels += fd;
		g_snap_ok = false;
	}
	bool sampled = (frame % g_cmp_every) == 0 || frame - g_last_change <= SCENE_WINDOW;
	/* band model only: the first band line differs alone when the 68k's last register write
	 * landed after the VDP drew that line (a beam race of the original, see render.md) */
	if (diff && dims && !other_rows)
		g_cmp_split_race++;
	g_cmp_all++;
	if (diff || !dims)
		g_cmp_all_bad++;
	if (sampled) {
		g_cmp_frames++;
		if (diff || !dims)
			g_cmp_bad++;
	}
	if (diff || !dims) {
		if (g_cmp_all_bad <= 50)
			fprintf(stderr, "rendercmp: frame %ld%s: %s %ld pixels differ, first at (%d,%d) native=%04X gpgx=%04X\n",
			        frame, sampled ? "" : " (not sampled)", dims ? "" : "SIZE", diff, fx, fy,
			        fx >= 0 ? render_rgba_to_565(g_fb[fy * RENDER_MAX_W + fx]) : 0,
			        fx >= 0 ? g[fy * w + fx] : 0);
		if (g_cmp_dir[0] && g_cmp_dumps < 10) {
			char p[700];
			if (!g_cmp_dumps)
				mkdir(g_cmp_dir, 0777);
			uint32_t *gr = malloc((size_t)w * h * 4), *d = malloc((size_t)w * h * 4);
			for (unsigned i = 0; i < w * h; i++) {
				gr[i] = render_565_to_rgba(g[i]);
				unsigned x = i % w, y = i / w;
				uint32_t n = g_fb[y * RENDER_MAX_W + x];
				d[i] = render_rgba_to_565(n) == g[i] ? (gr[i] >> 2 & 0x3F3F3F) | 0xFF000000u : 0xFF0000FFu;
			}
			snprintf(p, sizeof p, "%s/f%07ld-native.ppm", g_cmp_dir, frame);
			write_ppm_rgba(p, g_fb, g_out_w, g_out_h, RENDER_MAX_W);
			snprintf(p, sizeof p, "%s/f%07ld-gpgx.ppm", g_cmp_dir, frame);
			write_ppm_rgba(p, gr, (int)w, (int)h, (int)w);
			snprintf(p, sizeof p, "%s/f%07ld-diff.ppm", g_cmp_dir, frame);
			write_ppm_rgba(p, d, (int)w, (int)h, (int)w);
			free(gr);
			free(d);
			g_cmp_dumps++;
		}
	}
}

void render_emu_report(void)
{
	if (!g_cmp_every)
		return;
	/* stderr: session.py (storytest) keeps only the port's stderr */
	fprintf(stderr, "rendercmp: sampled %ld frames, %ld differ; all %ld frames, %ld differ; split-line-only %ld\n",
	        g_cmp_frames, g_cmp_bad, g_cmp_all, g_cmp_all_bad, g_cmp_split_race);
	if (g_frame_stats)
		fprintf(stderr, "rendercmp-frame-mode: %ld frames, %ld differ from GPGX (%ld pixels): mid-frame VDP changes\n",
		        g_fm_frames, g_fm_bad, g_fm_pixels);
}

static void report_atexit(void) { render_emu_report(); }

/* ------------------------------------------------------------------ API */
bool render_emu_select(const char *name)
{
	if (!name || !strcmp(name, "gpgx")) {
		g_kind = RENDERER_GPGX;
		hle_set_group("native", false);
		if (!g_cmp_every && g_running) {
			gx_set_render_hooks(NULL, NULL, NULL);
			g_running = false;
		}
		return true;
	}
	if (!strcmp(name, "native")) {
		g_kind = RENDERER_NATIVE;
		hle_set_group("native", true);
		start();
		return true;
	}
	return false;
}

renderer_kind render_emu_kind(void) { return g_kind; }

void render_emu_init(void)
{
	const char *c = getenv("SCOOBY_RENDERCMP");
	if (c && *c) {
		g_cmp_every = atoi(c);
		if (g_cmp_every < 1)
			g_cmp_every = 1;
		const char *d = strchr(c, ',');
		if (d && d[1]) {
			if (strstr(d + 1, "%d"))          /* one directory per process */
				snprintf(g_cmp_dir, sizeof g_cmp_dir, d + 1, (int)getpid());
			else
				snprintf(g_cmp_dir, sizeof g_cmp_dir, "%s", d + 1);
		}
		start();
		atexit(report_atexit);
		g_frame_stats = getenv("SCOOBY_RENDERCMP_FRAME") && atoi(getenv("SCOOBY_RENDERCMP_FRAME"));
		render_ctx_reset(&g_rcf);
	}
	const char *r = getenv("SCOOBY_RENDERER");
	if (r && *r && !render_emu_select(r))
		fprintf(stderr, "SCOOBY_RENDERER: unknown renderer '%s' (gpgx | native)\n", r);
	const char *s = getenv("SCOOBY_RENDER_SPLIT");
	if (s && !strcmp(s, "model")) {
		start();
		frameloop_split_model_check(true);
	}
}

void render_emu_set_band(const render_band *b)
{
	g_band = *b;
}

void render_emu_frame_end(long frame)
{
	if (!g_running)
		return;
	g_out_w = gx_viewport_w();
	g_out_h = gx_viewport_h();
	if (g_out_w > RENDER_MAX_W)
		g_out_w = RENDER_MAX_W;
	if (g_out_h > RENDER_MAX_H)
		g_out_h = RENDER_MAX_H;
	if (g_cmp_every)
		compare(frame);
	g_band.line = -1;
}

const uint32_t *render_emu_frame(int *w, int *h)
{
	if (!g_running)
		return NULL;
	*w = g_out_w;
	*h = g_out_h;
	return g_fb;
}
