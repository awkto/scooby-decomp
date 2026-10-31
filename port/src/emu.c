/* Emulator wrapper: statically linked Genesis Plus GX via its libretro entry points. */
#include "emu.h"
#include "hle.h"
#include "sha1.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libretro.h"
#include "gx.h"
#include "render/render_emu.h"
#include "render/hifps.h"

/* core/debug/cpuhook.c (type is hook_type_t, an enum) */
extern void set_cpu_hook(void (*hook)(int type, int width, unsigned int address, unsigned int value));

static char g_workdir[1024] = ".";
static const uint16_t *g_fb;
static unsigned g_fb_w, g_fb_h;
static size_t g_fb_pitch;
static uint16_t g_fb_copy[512 * 512];      /* frame kept across state loads */
static unsigned g_pad[2];
static long g_frames;
static int16_t g_audio[2 * 4096];
static size_t g_audio_frames;
static struct retro_system_av_info g_av;
static bool g_up;
static void *g_boot;          /* power-on state for emu_reset */

#define MAX_HOOKS 8
static emu_hook_fn g_hooks[MAX_HOOKS];
static int g_nhooks;

/* ------------------------------------------------------------------ libretro callbacks */
static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
	if (level < RETRO_LOG_WARN)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = log_cb;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		/* we only handle RGB565 (FRONTEND_SUPPORTS_RGB565 build) */
		return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = g_workdir;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	default:
		return false;
	}
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (!data)
		return;
	g_fb = data;
	g_fb_w = w;
	g_fb_h = h;
	g_fb_pitch = pitch;
}

static void audio_cb(int16_t l, int16_t r)
{
	if (g_audio_frames < sizeof g_audio / 4) {
		g_audio[2 * g_audio_frames] = l;
		g_audio[2 * g_audio_frames + 1] = r;
		g_audio_frames++;
	}
}

static size_t audio_batch_cb(const int16_t *d, size_t n)
{
	size_t room = sizeof g_audio / 4 - g_audio_frames;
	size_t k = n < room ? n : room;
	memcpy(g_audio + 2 * g_audio_frames, d, k * 4);
	g_audio_frames += k;
	return n;
}

static void poll_cb(void) {}

static const int retro_id[8] = {
	RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT,
	RETRO_DEVICE_ID_JOYPAD_RIGHT,
	RETRO_DEVICE_ID_JOYPAD_Y,      /* Genesis A (GPGX default mapping, same as tools/harness) */
	RETRO_DEVICE_ID_JOYPAD_B,      /* Genesis B */
	RETRO_DEVICE_ID_JOYPAD_A,      /* Genesis C */
	RETRO_DEVICE_ID_JOYPAD_START,
};

static int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)index;
	if (port > 1 || device != RETRO_DEVICE_JOYPAD)
		return 0;
	unsigned mask = 0;
	for (int i = 0; i < 8; i++)
		if (g_pad[port] & (1u << i))
			mask |= 1u << retro_id[i];
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		return (int16_t)mask;
	return (mask >> id) & 1;
}

/* ------------------------------------------------------------------ hooks */
static void hook_tramp(int type, int width, unsigned int addr, unsigned int value)
{
	for (int i = 0; i < g_nhooks; i++)
		g_hooks[i](type, width, addr, value);
}

void emu_add_hook(emu_hook_fn fn)
{
	for (int i = 0; i < g_nhooks; i++)
		if (g_hooks[i] == fn)
			return;
	if (g_nhooks < MAX_HOOKS)
		g_hooks[g_nhooks++] = fn;
	set_cpu_hook(hook_tramp);
}

void emu_remove_hook(emu_hook_fn fn)
{
	for (int i = 0; i < g_nhooks; i++)
		if (g_hooks[i] == fn) {
			memmove(g_hooks + i, g_hooks + i + 1, (g_nhooks - i - 1) * sizeof *g_hooks);
			g_nhooks--;
			break;
		}
	set_cpu_hook(g_nhooks ? hook_tramp : NULL);
}

/* ------------------------------------------------------------------ setup */
static bool file_exists(const char *p)
{
	return access(p, R_OK) == 0;
}

const char *emu_find_rom(const char *explicit_path, const char *argv0)
{
	static char buf[2048];
	if (explicit_path)
		return explicit_path;
	if (file_exists("rom/baserom.md"))
		return "rom/baserom.md";
#ifndef SCOOBY_DIST          /* the development tree the binary was built from (not in release builds) */
	snprintf(buf, sizeof buf, "%s/rom/baserom.md", SCOOBY_ROOT);
	if (file_exists(buf))
		return buf;
#endif
	/* <exe dir>/../rom, ../../rom, ../../../rom */
	char exe[1024];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (n <= 0 && argv0)
		n = (ssize_t)snprintf(exe, sizeof exe, "%s", argv0);
	if (n > 0) {
		exe[n] = 0;
		char *s = strrchr(exe, '/');
		if (s)
			*s = 0;
		const char *ups[] = { "", "/..", "/../..", "/../../.." };
		for (int i = 0; i < 4; i++) {
			snprintf(buf, sizeof buf, "%s%s/rom/baserom.md", exe, ups[i]);
			if (file_exists(buf))
				return buf;
		}
	}
	return NULL;
}

bool emu_init(const char *rom_path, const char *workdir, char *err, size_t errlen)
{
	if (workdir)
		snprintf(g_workdir, sizeof g_workdir, "%s", workdir);
	FILE *f = fopen(rom_path, "rb");
	if (!f) {
		snprintf(err, errlen, "cannot open ROM %s", rom_path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0 || sz > 8 * 1024 * 1024) {
		fclose(f);
		snprintf(err, errlen, "%s: bad size %ld", rom_path, sz);
		return false;
	}
	uint8_t *rom = malloc(sz);
	if (fread(rom, 1, sz, f) != (size_t)sz) {
		fclose(f);
		free(rom);
		snprintf(err, errlen, "%s: read error", rom_path);
		return false;
	}
	fclose(f);
	char hex[41];
	sha1_hex(rom, sz, hex);
	free(rom);
	if (strcmp(hex, SCOOBY_ROM_SHA1)) {
		snprintf(err, errlen, "%s: sha1 %s does not match Scooby-Doo Mystery (USA) %s - refusing to run",
		         rom_path, hex, SCOOBY_ROM_SHA1);
		return false;
	}

	retro_set_environment(env_cb);
	retro_set_video_refresh(video_cb);
	retro_set_audio_sample(audio_cb);
	retro_set_audio_sample_batch(audio_batch_cb);
	retro_set_input_poll(poll_cb);
	retro_set_input_state(input_cb);
	retro_init();
	struct retro_game_info gi = { rom_path, NULL, (size_t)sz, NULL };
	if (!retro_load_game(&gi)) {
		snprintf(err, errlen, "core failed to load %s", rom_path);
		return false;
	}
	retro_get_system_av_info(&g_av);
	hle_install();
	g_boot = malloc(emu_state_size());
	if (!emu_state_save(g_boot, emu_state_size())) {
		free(g_boot);
		g_boot = NULL;
	}
	g_up = true;
	render_emu_init();     /* SCOOBY_RENDERER / SCOOBY_RENDERCMP (#25) */
	return true;
}

void emu_shutdown(void)
{
	if (!g_up)
		return;
	retro_unload_game();
	retro_deinit();
	g_up = false;
}

/* ------------------------------------------------------------------ running */
void emu_set_pad(int port, unsigned buttons)
{
	if (port >= 0 && port < 2)
		g_pad[port] = buttons & 0xFF;
}

unsigned emu_get_pad(int port)
{
	return port >= 0 && port < 2 ? g_pad[port] : 0;
}

unsigned emu_pad_from_letters(const char *s)
{
	unsigned m = 0;
	for (; *s; s++) {
		switch (*s) {
		case 'U': m |= PAD_UP; break;
		case 'D': m |= PAD_DOWN; break;
		case 'L': m |= PAD_LEFT; break;
		case 'R': m |= PAD_RIGHT; break;
		case 'A': m |= PAD_A; break;
		case 'B': m |= PAD_B; break;
		case 'C': m |= PAD_C; break;
		case 'S': m |= PAD_START; break;
		}
	}
	return m;
}

/* SCOOBY_STATE_LOG=FILE: one line per frame, "frame hash" of the emulated machine's memories
 * (work RAM, VRAM, CRAM, VSRAM, VDP registers, Z80 RAM). Two runs with the same input must
 * write the same file whatever the host does with the picture (widescreen: port/tools/wstest.sh). */
static FILE *g_state_log;
static int g_state_log_init;

static uint64_t fnv1a(uint64_t h, const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 0x100000001B3ull;
	return h;
}

static void state_log_frame(void)
{
	if (!g_state_log_init) {
		g_state_log_init = 1;
		const char *p = getenv("SCOOBY_STATE_LOG");
		if (p && *p)
			g_state_log = fopen(p, "w");
	}
	if (!g_state_log)
		return;
	uint64_t h = 0xCBF29CE484222325ull;
	h = fnv1a(h, gx_work_ram(), 0x10000);
	h = fnv1a(h, gx_vram(), 0x10000);
	h = fnv1a(h, gx_cram(), 0x80);
	h = fnv1a(h, gx_vsram(), 0x80);
	h = fnv1a(h, gx_vdp_reg(), 0x20);
	h = fnv1a(h, gx_zram(), 0x2000);
	fprintf(g_state_log, "%ld %016llx\n", g_frames, (unsigned long long)h);
	if (!(g_frames & 255))
		fflush(g_state_log);
}

void emu_run_frame(void)
{
	g_audio_frames = 0;
	hle_frame_begin();
	retro_run();
	g_frames++;
	if (g_fb && g_fb_w <= 512 && g_fb_h <= 512) {
		for (unsigned y = 0; y < g_fb_h; y++)
			memcpy(g_fb_copy + y * g_fb_w, (const uint8_t *)g_fb + y * g_fb_pitch, g_fb_w * 2);
	}
	render_emu_frame_end(g_frames);
	state_log_frame();
}

long emu_frame_count(void) { return g_frames; }

void emu_reset(void)
{
	hle_state_reset();
	/* restore the power-on snapshot: retro_reset() is a soft reset that keeps RAM, so runs
	 * after it are not reproducible */
	if (g_boot)
		emu_state_load(g_boot, emu_state_size());
	else
		retro_reset();
	g_frames = 0;
}

const uint16_t *emu_video(unsigned *w, unsigned *h, size_t *pitch)
{
	*w = g_fb_w;
	*h = g_fb_h;
	*pitch = g_fb_w * 2;
	return g_fb_w ? g_fb_copy : NULL;
}

const int16_t *emu_audio(size_t *frames)
{
	*frames = g_audio_frames;
	return g_audio;
}

double emu_fps(void) { return g_av.timing.fps; }
double emu_sample_rate(void) { return g_av.timing.sample_rate; }

/* ------------------------------------------------------------------ state */
size_t emu_state_size(void) { return retro_serialize_size(); }
bool emu_state_save(void *buf, size_t size) { return retro_serialize(buf, size); }
bool emu_state_load(const void *buf, size_t size) { hle_state_reset(); hifps_reset(); return retro_unserialize(buf, size); }

bool emu_state_save_file(const char *path)
{
	size_t sz = emu_state_size();
	void *buf = malloc(sz);
	bool ok = emu_state_save(buf, sz);
	FILE *f = ok ? fopen(path, "wb") : NULL;
	if (f) {
		ok = fwrite(buf, 1, sz, f) == sz;
		fclose(f);
	} else {
		ok = false;
	}
	free(buf);
	return ok;
}

bool emu_state_load_file(const char *path)
{
	size_t sz = emu_state_size();
	void *buf = malloc(sz);
	FILE *f = fopen(path, "rb");
	bool ok = false;
	if (f) {
		ok = fread(buf, 1, sz, f) == sz && emu_state_load(buf, sz);
		fclose(f);
	}
	free(buf);
	return ok;
}

/* ------------------------------------------------------------------ dumps */
bool emu_write_ppm(const char *path)
{
	int nw, nh;
	const uint32_t *nat = render_emu_kind() == RENDERER_NATIVE ? render_emu_frame(&nw, &nh) : NULL;
	if (nat) {          /* the native renderer's frame is what is shown (#25) */
		FILE *f = fopen(path, "wb");
		if (!f)
			return false;
		fprintf(f, "P6\n%d %d\n255\n", nw, nh);
		for (int y = 0; y < nh; y++)
			for (int x = 0; x < nw; x++) {
				/* through RGB565 like GPGX's frames, so shots of both renderers (and the
				 * harness) compare byte for byte */
				uint16_t p = render_rgba_to_565(nat[y * RENDER_MAX_W + x]);
				uint8_t rgb[3] = { (uint8_t)((p >> 11) << 3), (uint8_t)(((p >> 5) & 63) << 2), (uint8_t)((p & 31) << 3) };
				fwrite(rgb, 1, 3, f);
			}
		fclose(f);
		return true;
	}
	unsigned w, h;
	size_t pitch;
	const uint16_t *px = emu_video(&w, &h, &pitch);
	FILE *f = px ? fopen(path, "wb") : NULL;
	if (!f)
		return false;
	fprintf(f, "P6\n%u %u\n255\n", w, h);
	for (unsigned y = 0; y < h; y++)
		for (unsigned x = 0; x < w; x++) {
			uint16_t p = px[y * w + x];
			uint8_t rgb[3] = { (uint8_t)((p >> 11) << 3), (uint8_t)(((p >> 5) & 63) << 2), (uint8_t)((p & 31) << 3) };
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
	return true;
}

bool emu_write_ram(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	for (unsigned a = 0; a < 0x10000; a++)
		fputc(work_ram[a ^ 1], f);   /* GPGX keeps words host-endian: natural 68k byte order */
	fclose(f);
	return true;
}

static bool write_file(const char *prefix, const char *ext, const void *d, size_t n)
{
	char path[1100];
	snprintf(path, sizeof path, "%s.%s", prefix, ext);
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	fwrite(d, 1, n, f);
	fclose(f);
	return true;
}

bool emu_write_vdp(const char *prefix)
{
	static uint8_t buf[0x10000];
	for (int i = 0; i < 0x10000; i += 2) {
		uint16_t w = *(uint16_t *)(gx_vram() + i);
		buf[i] = w >> 8;
		buf[i + 1] = (uint8_t)w;
	}
	bool ok = write_file(prefix, "vram", buf, 0x10000);
	for (int i = 0; i < 64; i++) {
		uint16_t c = *(uint16_t *)(gx_cram() + 2 * i);
		uint16_t w = ((c & 0x1C0) << 3) | ((c & 0x038) << 2) | ((c & 0x007) << 1);
		buf[2 * i] = w >> 8;
		buf[2 * i + 1] = (uint8_t)w;
	}
	ok &= write_file(prefix, "cram", buf, 128);
	for (int i = 0; i < 0x80; i += 2) {
		uint16_t w = *(uint16_t *)(gx_vsram() + i);
		buf[i] = w >> 8;
		buf[i + 1] = (uint8_t)w;
	}
	ok &= write_file(prefix, "vsram", buf, 0x50);
	ok &= write_file(prefix, "reg", gx_vdp_reg(), 0x20);
	return ok;
}

uint8_t emu_ram8(uint32_t a) { return work_ram[(a & 0xFFFF) ^ 1]; }
uint16_t emu_ram16(uint32_t a) { return *(uint16_t *)(work_ram + (a & 0xFFFE)); }
uint32_t emu_ram32(uint32_t a) { return ((uint32_t)emu_ram16(a) << 16) | emu_ram16(a + 2); }
