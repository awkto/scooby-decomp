/* frameprof: per-frame profile of the frame structure (main loop vs interrupts).
 *
 *   frameprof [--script FILE]... [--watch ADDR[=name]]... [--frames] [--hist FROM TO]
 *
 * Plays the scripts (default port/scripts/capture.txt) with pure 68k emulation and, for
 * every frame, records the active VBlank/HBlank hooks ($FF0000/$FF0004), the number of
 * VBlank and HBlank interrupts taken, main-loop iterations (PC hits at $000D38),
 * wait_vblank calls and the 68k cycles spent outside interrupt handlers.
 * Prints run-length segments of frames with the same hooks, then first/last frame and
 * hit count of every watched address (scene-flow checkpoints).
 * It also counts frames in which vblank_main did not run although it is the hook ("missing";
 * "gameplay" = with VBF_FRAME_WORK set): must be 0 in gameplay (port/tools/irqtest.sh, #30).
 * --hle SPEC enables C replacements (default none). Script mouse/click commands work.
 * --frames prints one line per frame instead of segments; --hist FROM TO prints the most
 * executed instruction addresses in that frame range (where the CPU time goes).
 */
#include "emu.h"
#include "ipprof.h"
#include "hle.h"
#include "script.h"
#include "gx.h"
#include "mouse.h"

extern unsigned int m68k_hle_get_sr(void);   /* gpgx-hle.patch */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_WATCH 128

typedef struct {
	uint32_t addr;
	char name[48];
	long first, last;
	unsigned long hits;
} watch;

static watch g_w[MAX_WATCH];
static int g_nw;

/* per-frame counters */
static unsigned g_vbl, g_hbl, g_loop, g_wait, g_vmain;

static void add_watch(uint32_t a, const char *name)
{
	if (g_nw >= MAX_WATCH)
		return;
	watch *w = &g_w[g_nw++];
	w->addr = a;
	snprintf(w->name, sizeof w->name, "%s", name ? name : "");
	w->first = w->last = -1;
}

static long g_frame;
static unsigned g_ww = 0xFFFFFFFF;   /* --wwatch ADDR: 68k writes to a RAM byte, and changes per frame */
static int g_ww_last = -1;
static unsigned g_vw_lo = 1, g_vw_hi = 0, g_vw_n, g_vw_zero;   /* --vramw LO HI: VRAM writes */
static long g_hist_from = -1, g_hist_to = -1;
static unsigned *g_hist;    /* executed-instruction count per address (--hist FROM TO) */

static void hook(int type, int width, unsigned addr, unsigned value)
{
	(void)width; (void)value;
	if (type == EMU_HOOK_WRITE && g_ww != 0xFFFFFFFF && (addr & 0xFFFFFF) <= g_ww &&
	    (addr & 0xFFFFFF) + (unsigned)width > g_ww) {
		fprintf(stderr, "frame %ld: 68k write.%d $%06X = $%X (pc $%06X)\n", g_frame, width, addr & 0xFFFFFF, value,
		        gx_m68k_pc());
		return;
	}
	if (type == EMU_HOOK_VRAM_W) {
		if (addr >= g_vw_lo && addr < g_vw_hi) {
			g_vw_n++;
			g_vw_zero += !value;
		}
		return;
	}
	if (type != EMU_HOOK_EXEC)
		return;
	addr &= 0xFFFFFF;
	if (g_hist && g_frame >= g_hist_from && g_frame <= g_hist_to && addr < 0x400000)
		g_hist[addr]++;
	switch (addr) {
	case 0x0008BC: g_vbl++; break;
	case 0x00A65C: g_vmain++; break;
	case 0x0008AA: g_hbl++; break;
	case 0x000D38: g_loop++; break;
	case 0x00974C: g_wait++; break;
	}
	for (int i = 0; i < g_nw; i++)
		if (g_w[i].addr == addr) {
			if (g_w[i].first < 0)
				g_w[i].first = g_frame;
			g_w[i].last = g_frame;
			g_w[i].hits++;
		}
}

static const char *hook_name(uint32_t a)
{
	switch (a) {
	case 0x00A65C: return "vblank_main";
	case 0x00A38C: return "vblank_title_fade";
	case 0x00A4A4: return "vblank_light";
	case 0x01006A: return "hblank_nop";
	case 0x01006C: return "hblank_verb_bar";
	case 0: return "-";
	}
	static char b[4][16];
	static int k;
	k = (k + 1) & 3;
	snprintf(b[k], sizeof b[k], "$%06X", a);
	return b[k];
}

int main(int argc, char **argv)
{
	const char *scripts[16];
	int ns = 0;
	bool per_frame = false;
	const char *hle_spec = "none";
	unsigned long no_vbl_main = 0, frames_main_hook = 0, no_vbl_play = 0, frames_play = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--script") && i + 1 < argc)
			scripts[ns++] = argv[++i];
		else if (!strcmp(argv[i], "--watch") && i + 1 < argc) {
			char buf[128];
			snprintf(buf, sizeof buf, "%s", argv[++i]);
			char *eq = strchr(buf, '=');
			if (eq)
				*eq++ = 0;
			add_watch((uint32_t)strtoul(buf + (buf[0] == '$'), NULL, 16), eq);
		} else if (!strcmp(argv[i], "--hist") && i + 2 < argc) {
			g_hist_from = atol(argv[++i]);
			g_hist_to = atol(argv[++i]);
			g_hist = calloc(0x400000, sizeof *g_hist);
		} else if (!strcmp(argv[i], "--hle") && i + 1 < argc) {
			hle_spec = argv[++i];
		} else if (!strcmp(argv[i], "--wwatch") && i + 1 < argc) {
			g_ww = (unsigned)strtoul(argv[++i], NULL, 16) & 0xFFFFFF;
		} else if (!strcmp(argv[i], "--vramw") && i + 2 < argc) {
			g_vw_lo = (unsigned)strtoul(argv[++i], NULL, 16);
			g_vw_hi = (unsigned)strtoul(argv[++i], NULL, 16);
		} else if (!strcmp(argv[i], "--frames"))
			per_frame = true;
		else {
			fprintf(stderr, "usage: frameprof [--script FILE]... [--watch ADDR[=name]]... [--frames]\n");
			return 2;
		}
	}
	if (!ns)
		scripts[ns++] = SCOOBY_ROOT "/port/scripts/capture.txt";

	char err[512];
	const char *rom = emu_find_rom(getenv("SCOOBY_ROM"), argv[0]);
	if (!rom || !emu_init(rom, "build", err, sizeof err)) {
		fprintf(stderr, "%s\n", rom ? err : "rom not found");
		return 1;
	}
	mkdir("runs", 0777);
	mkdir("runs/frameprof", 0777);
	hle_set_all(false);
	if (!hle_configure(hle_spec))
		return 2;
	emu_add_hook(hook);
	ipprof_init(hle_spec);   /* SCOOBY_IPPROF=file: interpreted-instruction profile (src/ipprof.c) */

	uint32_t seg_vh = 0xFFFFFFFF, seg_hh = 0xFFFFFFFF;
	long seg_start = 0;
	unsigned long seg_loop = 0, seg_vbl = 0, seg_wait = 0, seg_hbl = 0;
	unsigned seg_loop_min = ~0u, seg_loop_max = 0;
	if (!per_frame)
		printf("%-7s %-7s %-18s %-16s %9s %13s %8s %9s\n", "from", "to", "vblank hook", "hblank hook",
		       "VBlanks", "loops min/max", "waits", "HBlanks");
#define FLUSH()                                                                                       \
	do {                                                                                              \
		if (!per_frame && seg_vh != 0xFFFFFFFF)                                                       \
			printf("%-7ld %-7ld %-18s %-16s %9lu %6u/%-6u %8lu %9lu\n", seg_start, g_frame - 1,       \
			       hook_name(seg_vh), hook_name(seg_hh), seg_vbl, seg_loop_min, seg_loop_max, seg_wait, \
			       seg_hbl);                                                                              \
	} while (0)

	for (int i = 0; i < ns; i++) {
		script *s = script_open(scripts[i], "runs/frameprof");
		if (!s)
			return 1;
		unsigned pad;
		while (script_step(s, &pad)) {
			g_vbl = g_hbl = g_loop = g_wait = g_vmain = 0;
			g_vw_n = g_vw_zero = 0;
			emu_set_pad(0, mouse_frame(pad));   /* script mouse/click commands (#23) */
			emu_run_frame();
			uint32_t vh = emu_ram32(0xFF0000), hh = emu_ram32(0xFF0004);
			if (g_ww != 0xFFFFFFFF && emu_ram8(g_ww) != g_ww_last) {
				fprintf(stderr, "frame %ld: $%06X now $%02X\n", g_frame, g_ww, emu_ram8(g_ww));
				g_ww_last = emu_ram8(g_ww);
			}
			if (g_vw_n)
				fprintf(stderr, "frame %ld: %u VRAM writes in $%04X-$%04X (%u zero)\n", g_frame, g_vw_n, g_vw_lo, g_vw_hi,
				        g_vw_zero);
			if (vh == 0x00A65C) {
				/* gameplay: vblank_main must run every frame (regression check for #30) */
				frames_main_hook++;
				/* lost = gameplay frame without vblank_main although the CPU ends the frame with
				 * the VBlank interrupt unmasked (frames inside masked code - room loads, which
				 * run with SR=$2700 - are legitimately without VBlank) */
				bool unmasked = ((m68k_hle_get_sr() >> 8) & 7) < 6;
				if (emu_ram8(0xFF09DC) & 0x40) {      /* gameplay: VBF_FRAME_WORK set */
					frames_play++;
					no_vbl_play += !g_vmain && unmasked;
				}
				if (!g_vmain) {
					no_vbl_main++;
					if ((emu_ram8(0xFF09DC) & 0x40) && unmasked && no_vbl_play <= 40)
						fprintf(stderr, "frame %ld: no vblank_main (VBlank vector taken %u times), pc $%06X sr %04X"
						        " flags %02X cycles %d\n", g_frame, g_vbl, gx_m68k_pc(), m68k_hle_get_sr(),
						        emu_ram8(0xFF09DC), gx_m68k_cycles());
				}
			}
			if (per_frame) {
				printf("%6ld pad=%02X %-18s %-16s vbl=%u hbl=%u loops=%u waits=%u flags=%02X\n", g_frame, pad,
				       hook_name(vh), hook_name(hh), g_vbl, g_hbl, g_loop, g_wait, emu_ram8(0xFF09DC));
			} else if (vh != seg_vh || hh != seg_hh) {
				FLUSH();
				seg_vh = vh;
				seg_hh = hh;
				seg_start = g_frame;
				seg_loop = seg_vbl = seg_wait = seg_hbl = 0;
				seg_loop_min = ~0u;
				seg_loop_max = 0;
			}
			seg_loop += g_loop;
			seg_vbl += g_vbl;
			seg_wait += g_wait;
			seg_hbl += g_hbl;
			if (g_loop < seg_loop_min) seg_loop_min = g_loop;
			if (g_loop > seg_loop_max) seg_loop_max = g_loop;
			g_frame++;
		}
		script_close(s);
	}
	FLUSH();
	printf("\nvblank_main: missing in %lu of %lu frames with the vblank_main hook\n", no_vbl_main, frames_main_hook);
	printf("vblank_main gameplay: lost in %lu of %lu gameplay frames (VBlank unmasked, no vblank_main)\n", no_vbl_play, frames_play);
	printf("\n%-8s %-28s %8s %8s %10s\n", "addr", "watch", "first", "last", "hits");
	for (int i = 0; i < g_nw; i++)
		printf("$%06X  %-28s %8ld %8ld %10lu\n", g_w[i].addr, g_w[i].name, g_w[i].first, g_w[i].last, g_w[i].hits);
	if (g_hist) {
		/* top executed instruction addresses in the --hist frame range */
		printf("\ninstruction histogram, frames %ld-%ld (top 60 addresses)\n", g_hist_from, g_hist_to);
		for (int k = 0; k < 60; k++) {
			uint32_t best = 0;
			for (uint32_t a = 1; a < 0x400000; a++)
				if (g_hist[a] > g_hist[best])
					best = a;
			if (!g_hist[best])
				break;
			printf("$%06X %10u\n", best, g_hist[best]);
			g_hist[best] = 0;
		}
	}
	return 0;
}
