/* HLE registry and CPU glue. The patched core (tools/patches/gpgx-hle.patch) calls
 * m68k_hle_handler(pc) before fetching an instruction whose address has its bit set in
 * m68k_hle_bitmap. */
#include "hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx.h"
#include "native.h"

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
#define STALL_PC     0x0000F4u   /* "burn the rest of a long C function's cost" trap (#31) */
#define STALL_SLICE  1000        /* 68k cycles burnt per visit: interrupts get in between */

typedef struct {
	hle_def def;
	bool enabled;
	unsigned long calls;
} entry;

static entry g_ent[MAX_DEFS];
static int g_n;
/* address -> entry index + 1, open addressing (dispatch runs for every C block) */
#define HASH_SIZE 16384
static uint16_t g_hash[HASH_SIZE];
static uint8_t g_bitmap[0x400000 >> 4];
static bool g_installed;
static unsigned long g_frame_calls;

uint8_t *hle_rom;
void (*hle_post_access)(void);

/* ------------------------------------------------------------------ registry */
static unsigned hash_slot(uint32_t addr) { return (unsigned)((addr * 2654435761u) >> 18) & (HASH_SIZE - 1); }

static entry *find_addr(uint32_t addr)
{
	for (unsigned h = hash_slot(addr); g_hash[h]; h = (h + 1) & (HASH_SIZE - 1))
		if (g_ent[g_hash[h] - 1].def.addr == addr)
			return &g_ent[g_hash[h] - 1];
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
	unsigned h = hash_slot(def->addr);
	while (g_hash[h])
		h = (h + 1) & (HASH_SIZE - 1);
	g_hash[h] = (uint16_t)g_n;
	e->def = *def;
	e->enabled = !def->group;
	bitmap_set(def->addr, e->enabled);
}

void hle_register_named(uint32_t addr, const char *name, hle_fn fn, int cycles)
{
	hle_def d = { addr, name, fn, cycles, "(runtime)", NULL, NULL, 0 };
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

#define MAX_GROUPS 8
static const char *g_groups[MAX_GROUPS];   /* active opt-in groups */

bool hle_group_active(const char *group)
{
	for (int i = 0; i < MAX_GROUPS; i++)
		if (g_groups[i] && group && !strcmp(g_groups[i], group))
			return true;
	return false;
}

void hle_set_all(bool on)
{
	for (int i = 0; i < g_n; i++) {
		bool e = on && (!g_ent[i].def.group || hle_group_active(g_ent[i].def.group));
		g_ent[i].enabled = e;
		bitmap_set(g_ent[i].def.addr, e);
	}
}

void hle_set_group(const char *group, bool on)
{
	if (on && !hle_group_active(group)) {
		for (int i = 0; i < MAX_GROUPS; i++)
			if (!g_groups[i]) {
				g_groups[i] = group;
				break;
			}
	} else if (!on) {
		for (int i = 0; i < MAX_GROUPS; i++)
			if (g_groups[i] && !strcmp(g_groups[i], group))
				g_groups[i] = NULL;
	}
	/* members follow the ordinary replacements: on if any of those is on */
	bool any = false;
	for (int i = 0; i < g_n; i++)
		if (!g_ent[i].def.group && g_ent[i].enabled)
			any = true;
	for (int i = 0; i < g_n; i++)
		if (g_ent[i].def.group && !strcmp(g_ent[i].def.group, group)) {
			g_ent[i].enabled = on && any;
			bitmap_set(g_ent[i].def.addr, g_ent[i].enabled);
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
		printf("$%06X  %-24s %-3s cyc=%-5d %s%s%s\n", g_ent[i].def.addr, g_ent[i].def.name,
		       g_ent[i].enabled ? "on" : "off", g_ent[i].def.cycles, g_ent[i].def.file,
		       g_ent[i].def.group ? "  group=" : "", g_ent[i].def.group ? g_ent[i].def.group : "");
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

static long g_stall_debt;      /* cycles still to burn at STALL_PC */
static int g_nest;             /* depth of nested 68k execution */
static bool g_top;             /* a top-level C function is running */
/* "session": the nested 68k time of the current top-level C function. The clock, raster
 * line base and V counter keep advancing from one nested call to the next (so code after
 * the 2nd call sees the time the 1st took, as in the original, #33) and are rewound when
 * the C function returns; the time becomes part of its charge (#31). */
static bool g_sess;
static int g_sess_c0, g_sess_vc;
static unsigned g_sess_line_base;

int hle_nested_cycles(void) { return g_sess ? (gx_m68k_cycles() - g_sess_c0) / 7 : 0; }

static int sess_end(void)
{
	if (!g_sess)
		return 0;
	int n = (gx_m68k_cycles() - g_sess_c0) / 7;
	gx_m68k_set_cycles(g_sess_c0);
	gx_vdp_fifo_clamp();
	gx_vdp_set_line_base(g_sess_line_base);
	gx_vdp_set_vcounter(g_sess_vc);
	gx_m68k_resync_irq();
	g_sess = false;
	return n > 0 ? n : 0;
}

static const hle_def *g_cur_def;   /* innermost C function running (NULL at 68k top level) */

int hle_nest_depth(void) { return g_nest; }
const hle_def *hle_current_def(void) { return g_cur_def; }
/* would the CPU hand this pc to a C function instead of executing the instruction? (profilers) */
bool hle_pc_replaced(uint32_t pc)
{
	if (pc == STALL_PC)
		return true;
	if (pc >= 0x400000 || !(g_bitmap[pc >> 4] & (1u << ((pc >> 1) & 7))) || pc == m68k_hle_skip_pc)
		return false;
	entry *e = find_addr(pc);
	return e && e->enabled;
}

static int run_def(const hle_def *def, entry *e)
{
	hle_ctx c;
	const hle_def *outer_def = g_cur_def;
	bool top = g_nest == 0 && !g_top;
	if (top)
		g_top = true;
	ctx_load(&c);
	uint32_t sp0 = c.a[7];
	bool unmasked = ((c.sr >> 8) & 7) < 6;
	c.def = def;
	c.cycles = def->cycles;
	if (e)
		e->calls++;
	g_frame_calls++;
	g_cur_def = def;
	def->fn(&c);
	g_cur_def = outer_def;
	if (def->exits) {
		/* HLE_RANGE: must leave through a declared exit */
		if (!c.jumped || !hle_exit_match(def->exits, c.pc, c.a[7], sp0)) {
			fprintf(stderr, "hle: range %s ($%06X) left via $%06X with SP %+d%s, not a declared exit\n",
			        def->name, def->addr, c.pc, (int)(c.a[7] - sp0), c.jumped ? "" : " (no hle_jump)");
			abort();
		}
	} else if (!c.jumped) {
		c.pc = bus_r32(c.a[7]);   /* rts */
		c.a[7] += 4;
	}
	long nested = 0;
	if (top) {
		nested = sess_end();
		g_top = false;
	}
	long total = (c.cycles > 0 ? c.cycles : 0) + nested;
	if (unmasked && total > STALL_SLICE && !hle_post_access && g_nest == 0 && !g_stall_debt) {
		/* Long function entered with VBlank unmasked: the original would have been
		 * interrupted along the way. Spend the cost in slices at STALL_PC, a trap that
		 * "returns" to the real continuation (pushed like a jsr, so a savestate taken
		 * meanwhile just resumes there) - interrupts are taken between the slices. */
		c.a[7] -= 4;
		bus_w32(c.a[7], c.pc);
		c.pc = STALL_PC;
		g_stall_debt = total - STALL_SLICE;
		total = STALL_SLICE;
	}
	ctx_store(&c);
	return (int)total;
}

void hle_state_reset(void) { g_cur_def = NULL; g_stall_debt = 0; g_sess = false; g_top = false; g_nest = 0; }

/* STALL_PC: burn the next slice of g_stall_debt; then rts to the continuation */
static int stall(void)
{
	long n = g_stall_debt < STALL_SLICE ? g_stall_debt : STALL_SLICE;
	g_stall_debt -= n;
	if (g_stall_debt <= 0) {
		g_stall_debt = 0;
		uint32_t *dar = gx_m68k_dar();
		gx_m68k_set_pc(bus_r32(dar[15]) & 0xFFFFFF);
		dar[15] += 4;
	}
	return (int)(n > 0 ? n : 4);
}

static int g_trace = -1;   /* SCOOBY_HLE_TRACE=1: log every dispatched call to stderr */

static int dispatch(unsigned int pc)
{
	if (pc == STALL_PC)
		return stall();
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
	bitmap_set(STALL_PC, true);
	g_installed = true;
}

/* ------------------------------------------------------------------ nested 68k calls */
/* 68k code run nested inside a C function (hle_call68k, hle_run68k_until) executes
 * inside ONE time slice of the emulator's frame loop: the loop can't advance the raster
 * until the C function returns. Two things must not leak out of that (#30):
 *  - Interrupts. m68k_step_one() doesn't poll them, but any instruction that writes SR
 *    (move to SR, andi/ori to SR, rte) checks them, so a pending HBlank would be taken
 *    in the middle of the nested call. The C function takes no emulated time, so the
 *    interrupt belongs after it: the level is cleared after every nested instruction and
 *    recomputed from the VDP's pending flags when the outermost nested call returns.
 *  - The VDP line base. Nested code can run for many lines' worth of CPU cycles. A 68k
 *    VDP data-port write computes its FIFO slot from (CPU cycles - line base) and reads
 *    past GPGX's fifo_timing[] when that exceeds ~2 lines, which stalls the CPU by
 *    millions of cycles: the next ~15 VBlanks were lost (vblank_main skipped, input lag).
 *    So the line base - and the V counter (#33) - follow the CPU during nested execution
 *    and are restored when the C function returns (frame loop bookkeeping).
 *  - DMA (#31): a VDP DMA started by nested code is advanced by the frame loop, line by
 *    line, which can't run until C returns: the nested code's DMA-busy poll spun or the
 *    transfer stalled the CPU clock for seconds, and room_load's plane uploads never
 *    reached VRAM (Lobby drawn without its background). Nested DMA completes at once, as
 *    in the sandbox; the CPU is halted during a 68k->VDP DMA anyway.
 *  - The CPU clock (#31): time keeps running across the nested calls of one C function
 *    (an HV read after a nested call sees the time it took, as in the original: #33) and
 *    is rewound when the C function returns; the nested time is added to its charge
 *    (run_def), so a later Z80 bus request doesn't catch the Z80/YM2612 up by frames. */
static void nest_begin(void)
{
	if (g_nest++ == 0 && !hle_post_access && !g_sess) {   /* the sandbox manages time itself */
		g_sess = true;
		g_sess_c0 = gx_m68k_cycles();
		g_sess_line_base = gx_vdp_line_base();
		g_sess_vc = gx_vdp_vcounter();
	}
	gx_m68k_clear_irq();
	if (g_sess)
		gx_vdp_follow_cpu_lines();
}

static void nest_step_done(void)
{
	gx_m68k_clear_irq();
	if (g_sess)
		gx_vdp_follow_cpu_lines();
	if (gx_dma_length())
		gx_flush_dma();
}

static void nest_end(void)
{
	if (--g_nest == 0 && !g_top)    /* nested call made outside a C function (tools) */
		sess_end();
}

static void no_original(const char *what, uint32_t addr)
{
	fprintf(stderr, "native: %s($%06X): the original 68k code cannot run in the native build "
	        "(a path no route reaches; docs/native.md)\n", what, addr);
	abort();
}

static void call68k(hle_ctx *c, uint32_t addr, bool orig)
{
	if (orig && native_active)
		no_original("hle_call68k_orig", addr);
	/* push the sentinel return address, run until it is popped */
	hle_ctx save = *c;
	c->a[7] -= 4;
	bus_w32(c->a[7], SENTINEL_RET);
	uint32_t sp_done = c->a[7] + 4;
	c->pc = addr;
	nest_begin();
	ctx_store(c);
	if (orig)
		m68k_hle_skip_pc = addr;
	unsigned long n = 0;
	while (!(gx_m68k_pc() == SENTINEL_RET && gx_m68k_dar()[15] == sp_done)) {
		int r = m68k_step_one();
		nest_step_done();
		if (r < 0) {
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
	nest_end();
	ctx_load(c);
	c->def = save.def;
	c->pc = save.pc;
	c->jumped = save.jumped;
}

void hle_call68k(hle_ctx *c, uint32_t addr) { call68k(c, addr, false); }

/* VDP DMA started by C: behave like a nested 68k instruction (see hle.h) */
void hle_dma_begin(void) { nest_begin(); }
void hle_dma_complete(void) { nest_step_done(); }
void hle_dma_end(void)
{
	nest_step_done();
	nest_end();
}
void hle_set_usp(uint32_t usp) { gx_m68k_set_usp(usp); }

/* The original spins here until an interrupt changes something (wait primitives, docs/native.md):
 * charge the rest of the CPU's current time slice, so the frame loop moves on and the interrupt
 * is taken at the raster line where the original's spin would have taken it. */
void hle_idle(hle_ctx *c)
{
	if (g_nest > 0 || hle_post_access) {
		fprintf(stderr, "hle: %s ($%06X) waits for an interrupt inside a nested call / the sandbox\n",
		        c->def ? c->def->name : "?", c->def ? c->def->addr : 0);
		abort();
	}
	int left = native_active ? native_slice_left() : gx_m68k_slice_left();
	c->cycles = left > 4 ? left : 4;
}

void hle_run68k_until(hle_ctx *c, const uint32_t *exits)
{
	if (native_active)
		no_original("hle_run68k_until", c->pc);
	hle_ctx save = *c;
	uint32_t sp = c->a[7], start = c->pc;
	nest_begin();
	ctx_store(c);
	m68k_hle_skip_pc = start;
	for (unsigned long n = 0;; n++) {
		if (n && hle_exit_match(exits, gx_m68k_pc(), gx_m68k_dar()[15], sp))
			break;
		int r = m68k_step_one();
		nest_step_done();
		if (r < 0) {
			fprintf(stderr, "hle_run68k_until($%06X): address error at pc $%06X\n", start, gx_m68k_pc());
			abort();
		}
		if (hle_post_access)
			hle_post_access();
		if (n > 200000000ul) {
			fprintf(stderr, "hle_run68k_until($%06X): no exit after %lu instructions\n", start, n);
			abort();
		}
	}
	m68k_hle_skip_pc = 0xFFFFFFFF;
	nest_end();
	ctx_load(c);
	c->def = save.def;
	c->cycles = save.cycles;
	c->jumped = 1;
}
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

/* fatal errors of the native runner (native.c cannot use stdio: GPGX's headers remap it) */
void native_fatal(const char *what, unsigned pc, unsigned sr, unsigned sp, unsigned ret)
{
	fprintf(stderr, "native: %s at $%06X (SR $%04X, SP $%06X, long at SP $%06X).\n"
	        "        Every 68k address the game reaches must be a registered C block (HLE function or\n"
	        "        range); nothing is interpreted in the native build (docs/native.md).\n",
	        what, pc & 0xFFFFFF, sr, sp, ret);
	abort();
}
