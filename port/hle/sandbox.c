#include "sandbox.h"
#include "emu.h"
#include "gx.h"
#include "hle.h"

#include <stdlib.h>
#include <string.h>

/* gpgx-hle.patch */
extern int m68k_step_one(void);
extern unsigned int m68k_hle_get_sr(void);

static sb_event *g_ev;
static int g_nev, g_cap;
static bool g_rec;

/* ------------------------------------------------------------------ sandbox mode */
static void post(void)
{
	gx_m68k_clear_irq();             /* no interrupts inside the function under test */
	gx_z80_freeze();                 /* Z80 never catches up (bus requests don't run it) */
	gx_vdp_follow_cpu();             /* VDP line base follows the CPU (FIFO timing model) */
	if (gx_dma_length())
		gx_flush_dma();              /* DMA completes instantly: timing-independent */
}

void sb_begin(void)
{
	hle_post_access = post;
	post();
}

void sb_end(void)
{
	hle_post_access = NULL;
}

static bool at_exit(const uint32_t *exits, uint32_t pc)
{
	for (; *exits; exits++)
		if (*exits == pc)
			return true;
	return false;
}

bool sb_run_68k(unsigned long max_insns, unsigned long *insns, unsigned long *cycles, uint32_t *min_sp)
{
	return sb_run_68k_until(NULL, max_insns, insns, cycles, min_sp);
}

bool sb_run_68k_until(const uint32_t *exits, unsigned long max_insns, unsigned long *insns, unsigned long *cycles,
                      uint32_t *min_sp)
{
	uint32_t *dar = gx_m68k_dar();
	uint32_t sp0 = dar[15];
	uint32_t ret = bus_r32(sp0) & 0xFFFFFF;
	unsigned long n = 0;
	long used = 0;
	uint32_t lo = sp0;
	bool ok = true;
	while (exits ? !(n && dar[15] == sp0 && at_exit(exits, gx_m68k_pc()))
	             : !(gx_m68k_pc() == ret && dar[15] == sp0 + 4)) {
		int before = gx_m68k_cycles();
		if (n++ >= max_insns || m68k_step_one() < 0) {
			ok = false;
			break;
		}
		/* excludes DMA flushed by post(); a single instruction never takes more than ~160
		 * cycles, so larger jumps are bus stalls (VDP FIFO / DMA) in the frozen sandbox
		 * timeline and are not counted */
		int d = gx_m68k_cycles() - before;
		if (d > 0 && d <= 7 * 200)
			used += d;
		post();
		if (dar[15] < lo)
			lo = dar[15];
	}
	if (insns)
		*insns = n;
	if (cycles)
		*cycles = (unsigned long)(used / 7);
	if (min_sp)
		*min_sp = lo;
	return ok;
}

/* ------------------------------------------------------------------ recording */
static bool is_port(unsigned a)
{
	a &= 0xFFFFFF;
	return (a >= 0xA00000 && a < 0xA20000) || (a >= 0xC00000 && a < 0xE00000);
}

static void rec_hook(int type, int width, unsigned addr, unsigned value)
{
	if (!g_rec || type != EMU_HOOK_WRITE || !is_port(addr))
		return;
	if (g_nev == g_cap) {
		g_cap = g_cap ? 2 * g_cap : 256;
		g_ev = realloc(g_ev, g_cap * sizeof *g_ev);
	}
	g_ev[g_nev++] = (sb_event){ (uint8_t)width, addr & 0xFFFFFF, value };
}

void sb_record_begin(void)
{
	g_ev = NULL;
	g_nev = g_cap = 0;
	g_rec = true;
	emu_add_hook(rec_hook);
}

void sb_snapshot(sb_snap *s)
{
	g_rec = false;
	emu_remove_hook(rec_hook);
	uint32_t *dar = gx_m68k_dar();
	memcpy(s->d, dar, sizeof s->d);
	memcpy(s->a, dar + 8, sizeof s->a);
	s->other_sp = gx_m68k_other_sp();
	s->sr = (uint16_t)m68k_hle_get_sr();
	s->pc = gx_m68k_pc();
	const uint8_t *wr = gx_work_ram();
	for (int i = 0; i < 0x10000; i++)
		s->ram[i] = wr[i ^ 1];
	memcpy(s->vram, gx_vram(), sizeof s->vram);
	memcpy(s->cram, gx_cram(), sizeof s->cram);
	memcpy(s->vsram, gx_vsram(), sizeof s->vsram);
	memcpy(s->vreg, gx_vdp_reg(), sizeof s->vreg);
	gx_vdp_latch(s->vdp_latch);
	s->dma_length = gx_dma_length();
	memcpy(s->zram, gx_zram(), sizeof s->zram);
	s->zstate = gx_zstate();
	s->zbank = gx_zbank();
	memcpy(s->io, gx_io_reg(), sizeof s->io);
	s->ev = g_ev;
	s->nev = g_nev;
	g_ev = NULL;
	g_nev = g_cap = 0;
}

void sb_snap_free(sb_snap *s)
{
	free(s->ev);
	s->ev = NULL;
	s->nev = 0;
}

/* ------------------------------------------------------------------ compare */
static const char *wname(int w) { return w == 1 ? "b" : w == 2 ? "w" : "l"; }

static int cmp_bytes(FILE *out, const char *what, const uint8_t *a, const uint8_t *b, size_t n,
                     uint32_t base, uint32_t skip_lo, uint32_t skip_hi, int maxrep, bool quiet)
{
	int diffs = 0;
	uint32_t first = 0;
	for (size_t i = 0; i < n; i++) {
		uint32_t ad = base + (uint32_t)i;
		if (ad >= skip_lo && ad < skip_hi)
			continue;
		if (a[i] != b[i]) {
			if (!diffs)
				first = ad;
			if (!quiet && diffs < maxrep)
				fprintf(out, "    %s $%06X: 68k=%02X C=%02X\n", what, ad, a[i], b[i]);
			diffs++;
		}
	}
	if (diffs && !quiet)
		fprintf(out, "  %s: %d byte(s) differ, first at $%06X\n", what, diffs, first);
	return diffs;
}

int sb_compare(const sb_snap *a, const sb_snap *b, uint32_t dead_lo, uint32_t dead_hi,
               const sb_cmp_opts *o, FILE *out)
{
	int bad = 0, maxrep = o->max_report ? o->max_report : 8;
	bool q = o->quiet;

	/* 1. port writes, in order */
	int n = a->nev < b->nev ? a->nev : b->nev;
	for (int i = 0; i < n; i++) {
		const sb_event *x = &a->ev[i], *y = &b->ev[i];
		if (x->width != y->width || x->addr != y->addr || x->value != y->value) {
			if (!q)
				fprintf(out, "  port write #%d: 68k %s $%06X=%0*X  C %s $%06X=%0*X\n", i,
				        wname(x->width), x->addr, 2 * x->width, x->value,
				        wname(y->width), y->addr, 2 * y->width, y->value);
			bad++;
			break;
		}
	}
	if (a->nev != b->nev && !bad) {
		if (!q) {
			fprintf(out, "  port writes: 68k made %d, C made %d", a->nev, b->nev);
			const sb_snap *l = a->nev > b->nev ? a : b;
			fprintf(out, " (first extra: %s $%06X=%X)\n", wname(l->ev[n].width), l->ev[n].addr, l->ev[n].value);
		}
		bad++;
	}

	/* 2. registers */
	int rbad = 0;
	for (int i = 0; i < 16; i++) {
		if (o->ignore_regs & (1u << i))
			continue;
		uint32_t x = i < 8 ? a->d[i] : a->a[i - 8], y = i < 8 ? b->d[i] : b->a[i - 8];
		if (x != y) {
			if (!q)
				fprintf(out, "  %c%d: 68k=%08X C=%08X\n", i < 8 ? 'D' : 'A', i & 7, x, y);
			rbad++;
		}
	}
	if (a->other_sp != b->other_sp) {
		if (!q)
			fprintf(out, "  inactive SP: 68k=%08X C=%08X\n", a->other_sp, b->other_sp);
		rbad++;
	}
	if ((a->sr & ~o->ignore_sr) != (b->sr & ~o->ignore_sr)) {
		if (!q)
			fprintf(out, "  SR: 68k=%04X C=%04X (XNZVC 68k=%d%d%d%d%d C=%d%d%d%d%d)\n", a->sr, b->sr,
			        a->sr >> 4 & 1, a->sr >> 3 & 1, a->sr >> 2 & 1, a->sr >> 1 & 1, a->sr & 1,
			        b->sr >> 4 & 1, b->sr >> 3 & 1, b->sr >> 2 & 1, b->sr >> 1 & 1, b->sr & 1);
		rbad++;
	}
	if (a->pc != b->pc) {
		if (!q)
			fprintf(out, "  PC: 68k=%06X C=%06X\n", a->pc, b->pc);
		rbad++;
	}
	bad += rbad != 0;

	/* 3. memories */
	uint32_t lo = o->strict_stack ? 1 : (dead_lo & 0xFFFF) | 0xFF0000;
	uint32_t hi = o->strict_stack ? 0 : (dead_hi == dead_lo ? lo : ((dead_hi - 1) & 0xFFFF) + 0xFF0001);
	bad += cmp_bytes(out, "RAM", a->ram, b->ram, 0x10000, 0xFF0000, lo, hi, maxrep, q) != 0;
	bad += cmp_bytes(out, "VRAM", a->vram, b->vram, 0x10000, 0, 1, 0, maxrep, q) != 0;
	bad += cmp_bytes(out, "CRAM", a->cram, b->cram, 0x80, 0, 1, 0, maxrep, q) != 0;
	bad += cmp_bytes(out, "VSRAM", a->vsram, b->vsram, 0x80, 0, 1, 0, maxrep, q) != 0;
	bad += cmp_bytes(out, "VDPREG", a->vreg, b->vreg, 0x20, 0, 1, 0, maxrep, q) != 0;
	if (memcmp(a->vdp_latch, b->vdp_latch, sizeof a->vdp_latch) || a->dma_length != b->dma_length) {
		if (!q)
			fprintf(out, "  VDP latch (addr,code,pending,addr_latch,dmafill,cached,dmalen): 68k=%X,%X,%d,%X,%d,%d,%u C=%X,%X,%d,%X,%d,%d,%u\n",
			        a->vdp_latch[0], a->vdp_latch[1], a->vdp_latch[2], a->vdp_latch[3], a->vdp_latch[4], a->vdp_latch[5], a->dma_length,
			        b->vdp_latch[0], b->vdp_latch[1], b->vdp_latch[2], b->vdp_latch[3], b->vdp_latch[4], b->vdp_latch[5], b->dma_length);
		bad++;
	}
	bad += cmp_bytes(out, "Z80RAM", a->zram, b->zram, 0x2000, 0xA00000, 1, 0, maxrep, q) != 0;
	if (a->zstate != b->zstate || a->zbank != b->zbank) {
		if (!q)
			fprintf(out, "  Z80 bus: zstate 68k=%d C=%d, zbank 68k=%X C=%X\n", a->zstate, b->zstate, a->zbank, b->zbank);
		bad++;
	}
	bad += cmp_bytes(out, "IOREG", a->io, b->io, 0x10, 0, 1, 0, maxrep, q) != 0;
	return bad;
}
