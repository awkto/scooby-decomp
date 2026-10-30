/* Headless libretro harness for reverse engineering.
 *
 *   harness <core.so> <rom> <script> <outdir>
 *
 * Script lines (blank / '#' ignored):
 *   run N                 advance N frames with current input
 *   press BTN... N        hold buttons for N frames, then release   (BTN: U D L R A B C S)
 *   shot NAME             write outdir/NAME.ppm of the current frame
 *   ram NAME              write outdir/NAME.ram (64 KiB 68k work RAM)
 *   save NAME / load NAME savestates
 *   vdp NAME              write outdir/NAME.{vram,cram,vsram,reg}: VRAM (64 KiB, big-endian
 *                         words as on hardware), CRAM (64 words, hardware 0000BBB0GGG0RRR0),
 *                         VSRAM (40 words), VDP registers (32 bytes)   [needs vdptrace core]
 *   trace NAME / untrace  start/stop writing outdir/NAME.trace, one text line per event:
 *                           W <frame> <V|C|S> <vdpaddr> <data> D <src>          (DMA from 68k bus)
 *                           W <frame> <V|C|S> <vdpaddr> <data> P <pc> <lastrom> (CPU port write)
 *                           R <frame> <pc> <romaddr> <width>                    (68k ROM data read)
 *                         <lastrom> = most recent ROM data read address (or -1).
 *                         VDP events need a core built with tools/patches/gpgx-vdptrace.patch.
 *
 * At exit writes outdir/cov.bin: one byte per ROM byte,
 *   bit0 = instruction start executed, bit1 = read as data by the 68k,
 *   bit2 = byte belongs to an executed instruction's opcode word
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"

#define ROM_MAX (4 * 1024 * 1024)

static uint8_t cov[ROM_MAX];
static uint8_t ramexec[0x10000];
static long rom_size;
static const char *outdir;

static const void *fb;
static unsigned fb_w, fb_h;
static size_t fb_pitch;
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint16_t pad;   /* bitmask of RETRO_DEVICE_ID_JOYPAD_* */

enum { HOOK_M68K_E = 1, HOOK_M68K_R = 2, HOOK_M68K_W = 4,
       HOOK_VRAM_W = 16, HOOK_CRAM_W = 64, HOOK_VSRAM_W = 256 }; /* from gpgx core/debug/cpuhook.h */

/* optional symbols exported by the vdptrace core patch */
static unsigned int *p_hook_dma_src;
static uint8_t *p_vram, *p_cram, *p_vsram, *p_reg;
static FILE *tracef;
static long frame;
static unsigned cur_pc;
static long last_rom = -1;

static void hook(int type, int width, unsigned int addr, unsigned int value)
{
	if (type == HOOK_VRAM_W || type == HOOK_CRAM_W || type == HOOK_VSRAM_W) {
		if (!tracef)
			return;
		char k = type == HOOK_VRAM_W ? 'V' : type == HOOK_CRAM_W ? 'C' : 'S';
		unsigned src = p_hook_dma_src ? *p_hook_dma_src : 0xFFFFFFFFu;
		if (src != 0xFFFFFFFFu)
			fprintf(tracef, "W %ld %c %04x %04x D %06x\n", frame, k, addr & 0xFFFF, value & 0xFFFF, src & 0xFFFFFF);
		else
			fprintf(tracef, "W %ld %c %04x %04x P %06x %lx\n", frame, k, addr & 0xFFFF, value & 0xFFFF, cur_pc, last_rom);
		return;
	}
	addr &= 0xFFFFFF;
	if (type == HOOK_M68K_E)
		cur_pc = addr;
	if (type == HOOK_M68K_E && addr >= 0xFF0000) {
		ramexec[addr & 0xFFFF] = 1;
		return;
	}
	if (addr >= (unsigned)rom_size)
		return;
	if (type == HOOK_M68K_E) {
		cov[addr] |= 1;
		cov[addr] |= 4;
		if (addr + 1 < (unsigned)rom_size)
			cov[addr + 1] |= 4;
	} else if (type == HOOK_M68K_R) {
		last_rom = addr;
		if (tracef)
			fprintf(tracef, "R %ld %06x %06x %d\n", frame, cur_pc, addr, width);
		for (int i = 0; i < width && addr + i < (unsigned)rom_size; i++)
			cov[addr + i] |= 2;
	}
}

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
		pixfmt = *(enum retro_pixel_format *)data;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = outdir;
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
	if (data) {
		fb = data;
		fb_w = w;
		fb_h = h;
		fb_pitch = pitch;
	}
}
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t n) { (void)d; return n; }
static void poll_cb(void) {}
static int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)index;
	if (port != 0 || device != RETRO_DEVICE_JOYPAD)
		return 0;
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		return pad;
	return (pad >> id) & 1;
}

static void write_shot(const char *name)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s.ppm", outdir, name);
	FILE *f = fopen(path, "wb");
	if (!f || !fb)
		return;
	fprintf(f, "P6\n%u %u\n255\n", fb_w, fb_h);
	for (unsigned y = 0; y < fb_h; y++) {
		for (unsigned x = 0; x < fb_w; x++) {
			uint8_t rgb[3];
			if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
				uint32_t p = ((const uint32_t *)((const uint8_t *)fb + y * fb_pitch))[x];
				rgb[0] = p >> 16; rgb[1] = p >> 8; rgb[2] = p;
			} else {
				uint16_t p = ((const uint16_t *)((const uint8_t *)fb + y * fb_pitch))[x];
				if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
					rgb[0] = (p >> 11) << 3; rgb[1] = ((p >> 5) & 63) << 2; rgb[2] = (p & 31) << 3;
				} else {
					rgb[0] = ((p >> 10) & 31) << 3; rgb[1] = ((p >> 5) & 31) << 3; rgb[2] = (p & 31) << 3;
				}
			}
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
}

static int btn_id(char c)
{
	/* GPGX default mapping: retro B=Gen A, retro A=Gen B, retro Y=Gen C? keep explicit */
	switch (c) {
	case 'U': return RETRO_DEVICE_ID_JOYPAD_UP;
	case 'D': return RETRO_DEVICE_ID_JOYPAD_DOWN;
	case 'L': return RETRO_DEVICE_ID_JOYPAD_LEFT;
	case 'R': return RETRO_DEVICE_ID_JOYPAD_RIGHT;
	case 'A': return RETRO_DEVICE_ID_JOYPAD_Y;
	case 'B': return RETRO_DEVICE_ID_JOYPAD_B;
	case 'C': return RETRO_DEVICE_ID_JOYPAD_A;
	case 'S': return RETRO_DEVICE_ID_JOYPAD_START;
	}
	return -1;
}

#define SYM(name) \
	do { \
		*(void **)&p_##name = dlsym(core, #name); \
		if (!p_##name) { fprintf(stderr, "missing %s\n", #name); return 1; } \
	} while (0)

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr, "usage: %s core.so rom script outdir\n", argv[0]);
		return 2;
	}
	outdir = argv[4];
	void *core = dlopen(argv[1], RTLD_NOW);
	if (!core) {
		fprintf(stderr, "%s\n", dlerror());
		return 1;
	}
	void (*p_retro_set_environment)(retro_environment_t);
	void (*p_retro_set_video_refresh)(retro_video_refresh_t);
	void (*p_retro_set_audio_sample)(retro_audio_sample_t);
	void (*p_retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*p_retro_set_input_poll)(retro_input_poll_t);
	void (*p_retro_set_input_state)(retro_input_state_t);
	void (*p_retro_init)(void);
	bool (*p_retro_load_game)(const struct retro_game_info *);
	void (*p_retro_run)(void);
	void *(*p_retro_get_memory_data)(unsigned);
	size_t (*p_retro_serialize_size)(void);
	bool (*p_retro_serialize)(void *, size_t);
	bool (*p_retro_unserialize)(const void *, size_t);
	void (*p_set_cpu_hook)(void (*)(int, int, unsigned, unsigned));
	SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample);
	SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll); SYM(retro_set_input_state);
	SYM(retro_init); SYM(retro_load_game); SYM(retro_run); SYM(retro_get_memory_data);
	SYM(retro_serialize_size); SYM(retro_serialize); SYM(retro_unserialize); SYM(set_cpu_hook);
	p_hook_dma_src = dlsym(core, "hook_dma_src");
	p_vram = dlsym(core, "vram"); p_cram = dlsym(core, "cram");
	p_vsram = dlsym(core, "vsram"); p_reg = dlsym(core, "reg");

	FILE *rf = fopen(argv[2], "rb");
	if (!rf) { perror(argv[2]); return 1; }
	static uint8_t rom[ROM_MAX];
	rom_size = fread(rom, 1, ROM_MAX, rf);
	fclose(rf);

	p_retro_set_environment(env_cb);
	p_retro_set_video_refresh(video_cb);
	p_retro_set_audio_sample(audio_cb);
	p_retro_set_audio_sample_batch(audio_batch_cb);
	p_retro_set_input_poll(poll_cb);
	p_retro_set_input_state(input_cb);
	p_retro_init();
	struct retro_game_info gi = { argv[2], rom, (size_t)rom_size, NULL };
	if (!p_retro_load_game(&gi)) { fprintf(stderr, "load failed\n"); return 1; }
	p_set_cpu_hook(hook);

	FILE *sf = fopen(argv[3], "r");
	if (!sf) { perror(argv[3]); return 1; }
	char line[512];
	while (fgets(line, sizeof line, sf)) {
		char cmd[32], arg[256];
		int n;
		if (sscanf(line, "%31s", cmd) != 1 || cmd[0] == '#')
			continue;
		if (!strcmp(cmd, "run") && sscanf(line, "%*s %d", &n) == 1) {
			while (n--) { p_retro_run(); frame++; }
		} else if (!strcmp(cmd, "press") && sscanf(line, "%*s %255s %d", arg, &n) == 2) {
			pad = 0;
			for (char *c = arg; *c; c++)
				if (btn_id(*c) >= 0) pad |= 1u << btn_id(*c);
			while (n--) { p_retro_run(); frame++; }
			pad = 0;
		} else if (!strcmp(cmd, "shot") && sscanf(line, "%*s %255s", arg) == 1) {
			write_shot(arg);
		} else if (!strcmp(cmd, "ram") && sscanf(line, "%*s %255s", arg) == 1) {
			char path[1024];
			snprintf(path, sizeof path, "%s/%s.ram", outdir, arg);
			FILE *f = fopen(path, "wb");
			fwrite(p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM), 1, 0x10000, f);
			fclose(f);
		} else if ((!strcmp(cmd, "save") || !strcmp(cmd, "load")) && sscanf(line, "%*s %255s", arg) == 1) {
			char path[1024];
			snprintf(path, sizeof path, "%s/%s.state", outdir, arg);
			size_t sz = p_retro_serialize_size();
			void *buf = malloc(sz);
			if (cmd[0] == 's') {
				p_retro_serialize(buf, sz);
				FILE *f = fopen(path, "wb"); fwrite(buf, 1, sz, f); fclose(f);
			} else {
				FILE *f = fopen(path, "rb");
				if (f) { if (fread(buf, 1, sz, f) == sz) p_retro_unserialize(buf, sz); fclose(f); }
			}
			free(buf);
		} else if (!strcmp(cmd, "trace") && sscanf(line, "%*s %255s", arg) == 1) {
			char path[1024];
			snprintf(path, sizeof path, "%s/%s.trace", outdir, arg);
			if (tracef) fclose(tracef);
			tracef = fopen(path, "w");
			if (!p_hook_dma_src)
				fprintf(stderr, "warning: core lacks hook_dma_src (vdptrace patch)\n");
		} else if (!strcmp(cmd, "untrace")) {
			if (tracef) fclose(tracef);
			tracef = NULL;
		} else if (!strcmp(cmd, "vdp") && sscanf(line, "%*s %255s", arg) == 1) {
			if (!p_vram || !p_cram || !p_vsram || !p_reg) {
				fprintf(stderr, "vdp: core lacks vram/cram/vsram/reg exports\n");
				continue;
			}
			char path[1024];
			static uint8_t buf[0x10000];
			/* GPGX keeps VRAM as native-endian 16-bit words */
			for (int i = 0; i < 0x10000; i += 2) {
				uint16_t w = *(uint16_t *)(p_vram + i);
				buf[i] = w >> 8; buf[i + 1] = w;
			}
			snprintf(path, sizeof path, "%s/%s.vram", outdir, arg);
			FILE *f = fopen(path, "wb"); fwrite(buf, 1, 0x10000, f); fclose(f);
			/* CRAM is stored packed as 9-bit BBBGGGRRR */
			for (int i = 0; i < 64; i++) {
				uint16_t c = *(uint16_t *)(p_cram + 2 * i);
				uint16_t w = ((c & 0x1C0) << 3) | ((c & 0x038) << 2) | ((c & 0x007) << 1);
				buf[2 * i] = w >> 8; buf[2 * i + 1] = w;
			}
			snprintf(path, sizeof path, "%s/%s.cram", outdir, arg);
			f = fopen(path, "wb"); fwrite(buf, 1, 128, f); fclose(f);
			for (int i = 0; i < 0x80; i += 2) {
				uint16_t w = *(uint16_t *)(p_vsram + i);
				buf[i] = w >> 8; buf[i + 1] = w;
			}
			snprintf(path, sizeof path, "%s/%s.vsram", outdir, arg);
			f = fopen(path, "wb"); fwrite(buf, 1, 0x50, f); fclose(f);
			snprintf(path, sizeof path, "%s/%s.reg", outdir, arg);
			f = fopen(path, "wb"); fwrite(p_reg, 1, 0x20, f); fclose(f);
		} else {
			fprintf(stderr, "bad script line: %s", line);
		}
	}
	char path[1024];
	snprintf(path, sizeof path, "%s/cov.bin", outdir);
	FILE *cf = fopen(path, "wb");
	fwrite(cov, 1, rom_size, cf);
	fclose(cf);
	snprintf(path, sizeof path, "%s/ramexec.bin", outdir);
	cf = fopen(path, "wb");
	fwrite(ramexec, 1, sizeof ramexec, cf);
	fclose(cf);
	fprintf(stderr, "ran %ld frames\n", frame);
	return 0;
}
