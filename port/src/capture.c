#include "capture.h"
#include "emu.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gx.h"

#define CAP_MAGIC 0x50414353u   /* "SCAP" */
#define CAP_VERSION 2   /* v2: state blob zero-run encoded (states are ~95% zeros) */

static uint32_t g_addr;
static int g_max, g_every, g_skip, g_n;
static unsigned long g_calls;
static char g_dir[1024];
static uint8_t *g_buf;
static size_t g_size;
static bool g_on;

static void mkdirs(const char *dir)
{
	char tmp[1024];
	snprintf(tmp, sizeof tmp, "%s", dir);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0777);
			*p = '/';
		}
	mkdir(tmp, 0777);
}

/* zero-run encoding: repeated { u32 zeros; u32 nlit; nlit bytes } covering the blob */
static void zrle_write(FILE *f, const uint8_t *p, size_t n)
{
	size_t i = 0;
	while (i < n) {
		uint32_t z = 0;
		while (i < n && !p[i]) {
			z++;
			i++;
		}
		size_t s = i, e = i;
		while (e < n) {
			if (!p[e]) {        /* end the literal run at >= 16 zeros */
				size_t k = e;
				while (k < n && !p[k] && k - e < 16)
					k++;
				if (k - e >= 16 || k == n)
					break;
				e = k;
			} else {
				e++;
			}
		}
		uint32_t lit = (uint32_t)(e - s);
		fwrite(&z, 4, 1, f);
		fwrite(&lit, 4, 1, f);
		fwrite(p + s, 1, lit, f);
		i = e;
	}
}

static bool zrle_read(FILE *f, uint8_t *p, size_t n)
{
	size_t i = 0;
	while (i < n) {
		uint32_t z, lit;
		if (fread(&z, 4, 1, f) != 1 || fread(&lit, 4, 1, f) != 1 || i + z + lit > n)
			return false;
		memset(p + i, 0, z);
		i += z;
		if (fread(p + i, 1, lit, f) != lit)
			return false;
		i += lit;
	}
	return true;
}

static void cap_hook(int type, int width, unsigned addr, unsigned value)
{
	if (type != EMU_HOOK_EXEC || (addr & 0xFFFFFF) != g_addr)
		return;
	if (!g_dir[0]) {          /* count-only mode */
		g_calls++;
		return;
	}
	if (g_n >= g_max)
		return;
	unsigned long call = g_calls++;
	if ((long)call < g_skip || (call - g_skip) % g_every)
		return;
	if (!emu_state_save(g_buf, g_size))
		return;
	char path[1100];
	snprintf(path, sizeof path, "%s/%05d.cap", g_dir, g_n);
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "capture: %s: %s\n", path, strerror(errno));
		g_max = 0;
		return;
	}
	uint32_t hdr[8] = { CAP_MAGIC, CAP_VERSION, g_addr, (uint32_t)call, (uint32_t)emu_frame_count(),
	                    (uint32_t)gx_input_pad()[0] | ((uint32_t)gx_input_pad()[1] << 16), (uint32_t)g_size, 0 };
	fwrite(hdr, sizeof hdr, 1, f);
	zrle_write(f, g_buf, g_size);
	fclose(f);
	g_n++;
}

bool capture_start(uint32_t addr, int max_states, const char *dir, int every, int skip)
{
	g_addr = addr & 0xFFFFFF;
	g_max = max_states;
	g_every = every > 0 ? every : 1;
	g_skip = skip > 0 ? skip : 0;
	g_n = 0;
	g_calls = 0;
	snprintf(g_dir, sizeof g_dir, "%s", dir ? dir : "");
	if (dir)
		mkdirs(g_dir);
	g_size = emu_state_size();
	g_buf = realloc(g_buf, g_size);
	g_on = true;
	emu_add_hook(cap_hook);
	return true;
}

void capture_stop(void)
{
	if (g_on)
		emu_remove_hook(cap_hook);
	g_on = false;
}

int capture_count(void) { return g_n; }
unsigned long capture_calls(void) { return g_calls; }
bool capture_done(void) { return g_on && g_dir[0] && g_n >= g_max; }

bool cap_read(const char *path, cap_state *c)
{
	memset(c, 0, sizeof *c);
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	uint32_t hdr[8];
	bool ok = fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == CAP_MAGIC && hdr[1] == CAP_VERSION;
	if (ok) {
		c->addr = hdr[2];
		c->index = hdr[3];
		c->frame = hdr[4];
		c->pad[0] = (uint16_t)hdr[5];
		c->pad[1] = (uint16_t)(hdr[5] >> 16);
		c->state_size = hdr[6];
		c->state = malloc(c->state_size);
		ok = zrle_read(f, c->state, c->state_size);
	}
	fclose(f);
	if (!ok)
		cap_free(c);
	return ok;
}

void cap_free(cap_state *c)
{
	free(c->state);
	c->state = NULL;
}

bool cap_apply(const cap_state *c)
{
	if (c->state_size != emu_state_size() || !emu_state_load(c->state, c->state_size))
		return false;
	gx_input_pad()[0] = c->pad[0];
	gx_input_pad()[1] = c->pad[1];
	return true;
}
