/* HLE registry and CPU glue. The patched core (tools/patches/gpgx-hle.patch) calls
 * m68k_hle_handler(pc) before fetching an instruction whose address has its bit set in
 * m68k_hle_bitmap. */
#include "hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx.h"

/* from gpgx-hle.patch */
extern unsigned char *m68k_hle_bitmap;
extern int (*m68k_hle_handler)(unsigned int pc);
extern unsigned int m68k_hle_skip_pc;
extern unsigned int m68k_bus_read8(unsigned int a);
extern unsigned int m68k_bus_read16(unsigned int a);
extern unsigned int m68k_bus_read32(unsigned int a);
extern void m68k_bus_write8(unsigned int a, unsigned int v);
extern void m68k_bus_write16(unsigned int a, unsigned int v);
extern void m68k_bus_write32(unsigned int a, unsigned int v);
extern unsigned int m68k_hle_get_sr(void);
extern void m68k_hle_set_ccr(unsigned int v);
extern void m68k_hle_set_sr(unsigned int v);
extern int m68k_step_one(void);

#define MAX_DEFS 4096
#define SENTINEL_RET 0x0000F0u   /* return address used for hle_call68k (never executed) */

typedef struct {
	hle_def def;
	bool enabled;
	unsigned long calls;
} entry;

static entry g_ent[MAX_DEFS];
static int g_n;
static uint8_t g_bitmap[0x400000 >> 4];
static bool g_installed;
static unsigned long g_frame_calls;

uint8_t *hle_rom;
void (*hle_post_access)(void);

/* ------------------------------------------------------------------ registry */
static entry *find_addr(uint32_t addr)
{
	for (int i = 0; i < g_n; i++)
		if (g_ent[i].def.addr == addr)
			return &g_ent[i];
	return NULL;
}

static void bitmap_set(uint32_t addr, bool on)
{
	if (addr >= 0x400000 || (addr & 1))
		return;
	uint8_t bit = (uint8_t)(1u << ((addr >> 1) & 7));
	if (on)
		g_bitmap[addr >> 4] |= bit;
	else
		g_bitmap[addr >> 4] &= (uint8_t)~bit;
}

void hle_register_def(const hle_def *def)
{
	if (def->addr >= 0x400000 || (def->addr & 1)) {
		fprintf(stderr, "hle: %s: bad address $%06X\n", def->name, def->addr);
		abort();
	}
	entry *e = find_addr(def->addr);
	if (e) {
		fprintf(stderr, "hle: $%06X registered twice (%s, %s)\n", def->addr, e->def.name, def->name);
		abort();
	}
	if (g_n == MAX_DEFS)
		abort();
	e = &g_ent[g_n++];
	e->def = *def;
	e->enabled = true;
	bitmap_set(def->addr, true);
}

void hle_register_named(uint32_t addr, const char *name, hle_fn fn, int cycles)
{
	hle_def d = { addr, name, fn, cycles, "(runtime)" };
	if (!name) {
		char *buf = malloc(16);
		snprintf(buf, 16, "sub_%06X", addr);
		d.name = buf;
	}
	hle_register_def(&d);
}

void hle_register(uint32_t addr, hle_fn fn) { hle_register_named(addr, NULL, fn, 0); }

int hle_count(void) { return g_n; }
const hle_def *hle_get(int i) { return i >= 0 && i < g_n ? &g_ent[i].def : NULL; }
bool hle_is_enabled(int i) { return i >= 0 && i < g_n && g_ent[i].enabled; }
unsigned long hle_calls(int i) { return i >= 0 && i < g_n ? g_ent[i].calls : 0; }
unsigned long hle_calls_this_frame(void) { return g_frame_calls; }
void hle_frame_begin(void) { g_frame_calls = 0; }

static entry *lookup(const char *s)
{
	if (!s || !*s)
		return NULL;
	if (*s == '$' || (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
		uint32_t a = (uint32_t)strtoul(s + (*s == '$' ? 1 : 2), NULL, 16);
		return find_addr(a);
	}
	for (int i = 0; i < g_n; i++)
		if (!strcmp(g_ent[i].def.name, s))
			return &g_ent[i];
	return NULL;
}

const hle_def *hle_find(const char *s)
{
	entry *e = lookup(s);
	return e ? &e->def : NULL;
}

bool hle_set_enabled(const char *s, bool on)
{
	entry *e = lookup(s);
	if (!e)
		return false;
	e->enabled = on;
	bitmap_set(e->def.addr, on);
	return true;
}

void hle_set_all(bool on)
{
	for (int i = 0; i < g_n; i++) {
		g_ent[i].enabled = on;
		bitmap_set(g_ent[i].def.addr, on);
	}
}

bool hle_configure(const char *spec)
{
	if (!spec)
		return true;
	char buf[4096];
	snprintf(buf, sizeof buf, "%s", spec);
	bool ok = true;
	for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
		if (!strcmp(t, "all") || !strcmp(t, "on"))
			hle_set_all(true);
		else if (!strcmp(t, "none") || !strcmp(t, "off"))
			hle_set_all(false);
		else {
			bool on = *t != '-';
			const char *n = (*t == '-' || *t == '+') ? t + 1 : t;
			if (!hle_set_enabled(n, on)) {
				fprintf(stderr, "hle: unknown function '%s'\n", n);
				ok = false;
			}
		}
	}
	return ok;
}

void hle_list(void)
{
	for (int i = 0; i < g_n; i++)
		printf("$%06X  %-24s %-3s cyc=%-5d %s\n", g_ent[i].def.addr, g_ent[i].def.name,
		       g_ent[i].enabled ? "on" : "off", g_ent[i].def.cycles, g_ent[i].def.file);
}

/* ------------------------------------------------------------------ CPU state <-> ctx */
static void ctx_load(hle_ctx *c)
{
	uint32_t *dar = gx_m68k_dar();
	memcpy(c->d, dar, sizeof c->d);
	memcpy(c->a, dar + 8, sizeof c->a);
	c->sr = (uint16_t)m68k_hle_get_sr();
	c->pc = gx_m68k_pc();
	c->jumped = 0;
	c->def = NULL;
}

/* Registers and PC are stored before SR: lowering the interrupt mask can take a pending
 * interrupt right away, which must stack the final PC/SP (as after the original rts...).
 * A C function must not change the S bit (the game always runs in supervisor mode). */
static void ctx_store(const hle_ctx *c)
{
	uint16_t old = (uint16_t)m68k_hle_get_sr();
	uint32_t *dar = gx_m68k_dar();
	memcpy(dar, c->d, sizeof c->d);
	memcpy(dar + 8, c->a, sizeof c->a);
	gx_m68k_set_pc(c->pc);
	if ((old & 0xFF00) == (c->sr & 0xFF00))
		m68k_hle_set_ccr(c->sr & 0xFF);
	else
		m68k_hle_set_sr(c->sr);
}

static int run_def(const hle_def *def, entry *e)
{
	hle_ctx c;
	ctx_load(&c);
	c.def = def;
	c.cycles = def->cycles;
	if (e)
		e->calls++;
	g_frame_calls++;
	def->fn(&c);
	if (!c.jumped) {
		c.pc = bus_r32(c.a[7]);   /* rts */
		c.a[7] += 4;
	}
	ctx_store(&c);
	return c.cycles > 0 ? c.cycles : 0;
}

static int g_trace = -1;   /* SCOOBY_HLE_TRACE=1: log every dispatched call to stderr */

static int dispatch(unsigned int pc)
{
	entry *e = find_addr(pc);
	if (!e || !e->enabled)
		return -1;
	if (g_trace < 0)
		g_trace = getenv("SCOOBY_HLE_TRACE") ? atoi(getenv("SCOOBY_HLE_TRACE")) : 0;
	if (!g_trace)
		return run_def(&e->def, e);
	int c0 = gx_m68k_cycles();
	uint32_t d0 = gx_m68k_dar()[0], d1 = gx_m68k_dar()[1], sp = gx_m68k_dar()[15];
	int r = run_def(&e->def, e);
	fprintf(stderr, "hle %-18s D0=%08X D1=%08X SP=%06X -> ret %06X  mclk %d -> %d (+%d charged)\n",
	        e->def.name, d0, d1, sp, gx_m68k_pc(), c0, gx_m68k_cycles(), r * 7);
	return r;
}

int hle_invoke(const hle_def *def)
{
	return run_def(def, find_addr(def->addr));
}

void hle_install(void)
{
	hle_rom = gx_rom();
	m68k_hle_handler = dispatch;
	m68k_hle_bitmap = g_bitmap;
	g_installed = true;
}

/* ------------------------------------------------------------------ nested 68k calls */
static void call68k(hle_ctx *c, uint32_t addr, bool orig)
{
	/* push the sentinel return address, run until it is popped */
	hle_ctx save = *c;
	c->a[7] -= 4;
	bus_w32(c->a[7], SENTINEL_RET);
	uint32_t sp_done = c->a[7] + 4;
	c->pc = addr;
	ctx_store(c);
	if (orig)
		m68k_hle_skip_pc = addr;
	unsigned long n = 0;
	while (!(gx_m68k_pc() == SENTINEL_RET && gx_m68k_dar()[15] == sp_done)) {
		if (m68k_step_one() < 0) {
			fprintf(stderr, "hle_call68k($%06X): address error at pc $%06X\n", addr, gx_m68k_pc());
			abort();
		}
		if (hle_post_access)
			hle_post_access();
		if (++n > 200000000ul) {
			fprintf(stderr, "hle_call68k($%06X): no return after %lu instructions\n", addr, n);
			abort();
		}
	}
	m68k_hle_skip_pc = 0xFFFFFFFF;
	ctx_load(c);
	c->def = save.def;
	c->pc = save.pc;
	c->jumped = save.jumped;
}

void hle_call68k(hle_ctx *c, uint32_t addr) { call68k(c, addr, false); }
void hle_call68k_orig(hle_ctx *c, uint32_t addr) { call68k(c, addr, true); }

/* ------------------------------------------------------------------ bus */
#define CHECK_ALIGN(a) do { if ((a) & 1) { fprintf(stderr, "hle: odd word access $%06X\n", (unsigned)(a)); abort(); } } while (0)

uint8_t bus_r8(uint32_t a) { return (uint8_t)m68k_bus_read8(a); }
uint16_t bus_r16(uint32_t a) { CHECK_ALIGN(a); return (uint16_t)m68k_bus_read16(a); }
uint32_t bus_r32(uint32_t a) { CHECK_ALIGN(a); return m68k_bus_read32(a); }

/* VDP data port ($C00000-$C00003): see gx_vdp_untimed_begin() */
static bool is_vdp_data(uint32_t a)
{
	return (a & 0xFFFFFC) == 0xC00000;
}

void bus_w8(uint32_t a, uint8_t v)
{
	bool vd = is_vdp_data(a);
	if (vd)
		gx_vdp_untimed_begin();
	m68k_bus_write8(a, v);
	if (vd)
		gx_vdp_untimed_end();
	if (hle_post_access)
		hle_post_access();
}

void bus_w16(uint32_t a, uint16_t v)
{
	CHECK_ALIGN(a);
	bool vd = is_vdp_data(a);
	if (vd)
		gx_vdp_untimed_begin();
	m68k_bus_write16(a, v);
	if (vd)
		gx_vdp_untimed_end();
	if (hle_post_access)
		hle_post_access();
}

void bus_w32(uint32_t a, uint32_t v)
{
	CHECK_ALIGN(a);
	bool vd = is_vdp_data(a);
	if (vd)
		gx_vdp_untimed_begin();
	m68k_bus_write32(a, v);
	if (vd)
		gx_vdp_untimed_end();
	if (hle_post_access)
		hle_post_access();
}
