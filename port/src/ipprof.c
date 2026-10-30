/* Interpreted-instruction profile: see ipprof.h.
 *
 * Every 68k instruction the CPU executes (EMU_HOOK_EXEC) is counted per address, separately for
 * top level (68k code running on its own: main loop, interrupt handlers) and nested (68k code called
 * from a C function).  The exec hook also fires at the entry of a replaced function, where the CPU
 * hands over to C instead of executing an instruction: those events are dropped
 * (hle_pc_replaced).  Nested counts are additionally keyed by the calling C function, so the
 * report can say who still needs each 68k function.
 *
 * Output, text, one record per line:
 *   # ipprof v1 / hle SPEC / frames N
 *   T ADDR COUNT           top-level instruction at ADDR executed COUNT times
 *   N ADDR COUNT           nested
 *   C CALLER ADDR COUNT    nested, by calling C function (innermost running C function)
 */
#include "ipprof.h"
#include "emu.h"
#include "hle.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ADDR_SPACE 0x400000u
static uint32_t *g_top, *g_nest;
static char g_path[1024], g_spec[512];

/* nested count per (caller, address): open addressing */
typedef struct { uint64_t key; uint64_t n; } slot;
static slot *g_tab;
static size_t g_tab_cap = 1 << 16, g_tab_used;
static const char *g_callers[512];
static int g_ncallers;

static int caller_id(const hle_def *d)
{
	if (!d)
		return 0;
	for (int i = 1; i <= g_ncallers; i++)
		if (g_callers[i - 1] == d->name)
			return i;
	if (g_ncallers == 511)
		return 0;
	g_callers[g_ncallers++] = d->name;
	return g_ncallers;
}

static size_t hash(uint64_t key, size_t cap) { return (size_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1); }

static void tab_add(uint64_t key)
{
	for (;;) {
		size_t i = hash(key, g_tab_cap);
		for (;; i = (i + 1) & (g_tab_cap - 1)) {
			if (g_tab[i].key == key) { g_tab[i].n++; return; }
			if (!g_tab[i].key) break;
		}
		if (g_tab_used * 2 < g_tab_cap) {
			g_tab[i].key = key; g_tab[i].n = 1; g_tab_used++;
			return;
		}
		slot *old = g_tab; size_t oc = g_tab_cap;
		g_tab_cap *= 2;
		g_tab = calloc(g_tab_cap, sizeof *g_tab);
		g_tab_used = 0;
		for (size_t k = 0; k < oc; k++)
			if (old[k].key) {
				size_t j = hash(old[k].key, g_tab_cap);
				while (g_tab[j].key) j = (j + 1) & (g_tab_cap - 1);
				g_tab[j] = old[k]; g_tab_used++;
			}
		free(old);
	}
}

static void hook(int type, int width, unsigned addr, unsigned value)
{
	(void)width; (void)value;
	if (type != EMU_HOOK_EXEC)
		return;
	addr &= 0xFFFFFF;
	if (addr >= ADDR_SPACE || hle_pc_replaced(addr))
		return;
	if (hle_nest_depth() > 0) {
		g_nest[addr]++;
		int id = caller_id(hle_current_def());
		tab_add(((uint64_t)(id + 1) << 32) | addr);
	} else
		g_top[addr]++;
}

static void write_out(void)
{
	char path[1100];
	const char *pc = strstr(g_path, "%d");
	if (pc)
		snprintf(path, sizeof path, "%.*s%d%s", (int)(pc - g_path), g_path, (int)getpid(), pc + 2);
	else
		snprintf(path, sizeof path, "%s", g_path);
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "# ipprof v1\nhle %s\nframes %ld\n", g_spec, emu_frame_count());
	for (uint32_t a = 0; a < ADDR_SPACE; a++) {
		if (g_top[a]) fprintf(f, "T %06X %u\n", a, g_top[a]);
		if (g_nest[a]) fprintf(f, "N %06X %u\n", a, g_nest[a]);
	}
	for (size_t i = 0; i < g_tab_cap; i++)
		if (g_tab[i].key) {
			int id = (int)(g_tab[i].key >> 32) - 1;
			fprintf(f, "C %s %06X %llu\n", id ? g_callers[id - 1] : "-", (unsigned)(g_tab[i].key & 0xFFFFFF),
			        (unsigned long long)g_tab[i].n);
		}
	fclose(f);
}

void ipprof_init(const char *hle_spec)
{
	const char *p = getenv("SCOOBY_IPPROF");
	if (!p || !*p)
		return;
	snprintf(g_path, sizeof g_path, "%s", p);
	snprintf(g_spec, sizeof g_spec, "%s", hle_spec ? hle_spec : "none");
	g_top = calloc(ADDR_SPACE, sizeof *g_top);
	g_nest = calloc(ADDR_SPACE, sizeof *g_nest);
	g_tab = calloc(g_tab_cap, sizeof *g_tab);
	emu_add_hook(hook);
	atexit(write_out);
}
