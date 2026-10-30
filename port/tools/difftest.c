/* difftest: prove a C replacement (HLE_FUNC) equivalent to the original 68k function.
 *
 *   difftest list
 *   difftest capture ADDR N [--out DIR] [--script FILE]... [--every K] [--skip K] [--hle SPEC]
 *   difftest capture-multi N TAG ADDR... [--script FILE]...   every call (up to N) of each
 *       ADDR in ONE pass over the route, into runs/states/<ADDR>_<TAG>/ (or --out DIR/<ADDR>_<TAG>)
 *   difftest run FUNC... [--states DIR] [--max N] [options]
 *   difftest check FUNC...|all [--n N] [capture/run options]   capture if needed, then run
 *
 * FUNC is a registered name (pad_read) or $ADDR / 0xADDR. States live in
 * runs/states/<6-digit hex addr>/NNNNN.cap by default. Capture plays port/scripts/capture.txt
 * (or --script files) and, unless --every is given, spreads the N states evenly over all
 * calls made on that route.
 *
 * For every captured entry state: load it, run the original 68k code until it returns
 * (an HLE_RANGE: until it reaches one of its declared exits at the exit's SP; other HLE
 * functions disabled unless --with-hle), snapshot; reload, run the C function,
 * snapshot; compare registers, SR, work RAM (minus dead stack), VRAM/CRAM/VSRAM, VDP
 * registers + port latches, Z80 RAM / bus state, I/O registers and the ordered list of
 * writes to the VDP ports, Z80 area and I/O area. The first failing state is reported in
 * detail.
 *
 * run options:
 *   --ignore-regs d1,a0   don't compare these registers (document why in the C source!)
 *   --ignore-flags        don't compare X N Z V C
 *   --poke "W:FF06AC=5,L:FF068E=0xFCA0C,D0=7"   run each state once per --poke variant, after patching
 *                         work RAM (B/W/L:HEXADDR=VALUE) and registers (D0..D7/A0..A7=VALUE), comma list
 *   --strict-stack        compare RAM below the entry SP as well
 *   --stack-window B      bytes below entry SP treated as scratch (default 256)
 *   --with-hle            keep other enabled HLE functions active on both sides
 *   --passthrough         C side = hle_call68k_orig (self-test of the harness)
 *   --keep-going          report every failing state, not just the first
 *   --entry               "synthetic dispatch": use states captured at ANOTHER address with
 *                         the same calling context (e.g. another script op handler, entered by
 *                         the same `jsr (a0)`), with the PC set to the target. Combine with
 *                         --poke to put the VM where that dispatch would have put it
 *                         (A5 = an op of the target's kind, A0 = handler; tools/py/script_pokes.py)
 *   -v                    print one line per state
 */
#include "capture.h"
#include "frameloop.h"
#include "emu.h"
#include "hle.h"
#include "sandbox.h"
#include "script.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx.h"

typedef struct {
	const char *states;
	int max;
	int n_capture;
	const char *scripts[16];
	int nscripts;
	int every, skip;
	const char *hle_spec;
	sb_cmp_opts cmp;
	uint32_t stack_window;
	bool with_hle, passthrough, keep_going, verbose;
	bool entry;                 /* --entry: states captured elsewhere, PC set to the target */
	const char *out;
	const char *pokes[1024];    /* --poke variants, each "W:ADDR=VAL,B:ADDR=VAL,..." */
	int npokes;
} opts;

/* Apply one --poke variant to work RAM: comma list of [BWL:]ADDR=VALUE (default size W). */
static void apply_poke(const char *spec)
{
	char buf[512];
	snprintf(buf, sizeof buf, "%s", spec);
	for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
		char sz = 'W';
		if (tok[1] == ':') { sz = tok[0] & ~0x20; tok += 2; }
		char *eq = strchr(tok, '=');
		if (!eq) continue;
		if ((tok[0] == 'D' || tok[0] == 'A') && tok[1] >= '0' && tok[1] <= '7' && tok[2] == '=') {
			/* register poke: D0=value / A3=value (long) */
			gx_m68k_dar()[(tok[0] == 'A' ? 8 : 0) + (tok[1] - '0')] = (uint32_t)strtoul(eq + 1, NULL, 0);
			continue;
		}
		uint32_t addr = (uint32_t)strtoul(tok, NULL, 16), v = (uint32_t)strtoul(eq + 1, NULL, 0);
		if (sz == 'B') RAM8(addr) = (uint8_t)v;
		else if (sz == 'L') RAM32_W(addr, v);
		else RAM16(addr) = (uint16_t)v;
	}
}

static void usage(void)
{
	fprintf(stderr,
	        "usage: difftest list\n"
	        "       difftest capture ADDR N [--out DIR] [--script FILE]... [--every K] [--skip K] [--hle SPEC]\n"
	        "       difftest capture-multi N TAG ADDR... [--out BASE] [--script FILE]...\n"
	        "       difftest run FUNC... [--states DIR] [--max N] [--ignore-regs d1,a0] [--ignore-flags]\n"
	        "                [--strict-stack] [--stack-window B] [--with-hle] [--passthrough] [--keep-going] [-v]\n"
	        "       difftest check FUNC...|all [--n N] [capture and run options]\n");
	exit(2);
}

static uint32_t parse_regs(const char *s)
{
	uint32_t m = 0;
	char buf[256];
	snprintf(buf, sizeof buf, "%s", s);
	for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
		if ((t[0] == 'd' || t[0] == 'D') && t[1] >= '0' && t[1] <= '7')
			m |= 1u << (t[1] - '0');
		else if ((t[0] == 'a' || t[0] == 'A') && t[1] >= '0' && t[1] <= '7')
			m |= 1u << (8 + t[1] - '0');
		else {
			fprintf(stderr, "bad register %s\n", t);
			exit(2);
		}
	}
	return m;
}

static void default_dir(char *buf, size_t n, uint32_t addr)
{
	snprintf(buf, n, "runs/states/%06X", addr);
}

/* ------------------------------------------------------------------ capture */
/* play the capture route (--script files, default port/scripts/capture.txt) */
static int play_scripts(const opts *o, const char *outdir)
{
	const char *def_script = SCOOBY_ROOT "/port/scripts/capture.txt";
	int ns = o->nscripts ? o->nscripts : 1;
	for (int i = 0; i < ns && !capture_done(); i++) {
		const char *path = o->nscripts ? o->scripts[i] : def_script;
		script *s = script_open(path, outdir);
		if (!s)
			return 1;
		unsigned pad;
		while (!capture_done() && script_step(s, &pad)) {
			emu_set_pad(0, pad);
			emu_run_frame();
		}
		script_close(s);
	}
	return 0;
}

static int do_capture(uint32_t addr, int n, const char *dir, const opts *o)
{
	hle_set_all(false);                 /* capture pure 68k behaviour */
	if (o->hle_spec)
		hle_configure(o->hle_spec);
	int every = o->every;
	if (every <= 0) {
		/* auto: count the calls over the whole route first, then spread the N captures
		 * evenly over it (the first calls are often all alike, e.g. boot with no input) */
		emu_reset();
		capture_start(addr, 0, NULL, 1, 0);
		if (play_scripts(o, "runs"))
			return 1;
		unsigned long calls = capture_calls();
		capture_stop();
		every = calls > (unsigned long)(o->skip + n) ? (int)((calls - o->skip) / n) : 1;
		printf("$%06X is called %lu times on the capture route: keeping every %d%s\n", addr, calls, every,
		       every == 1 ? "" : "th");
		emu_reset();
	}
	capture_start(addr, n, dir, every, o->skip);
	if (play_scripts(o, dir))
		return 1;
	capture_stop();
	printf("captured %d entry states of $%06X into %s\n", capture_count(), addr, dir);
	return capture_count() >= n ? 0 : 1;
}

/* ------------------------------------------------------------------ run */
static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static char **list_states(const char *dir, int *count)
{
	DIR *d = opendir(dir);
	*count = 0;
	if (!d)
		return NULL;
	char **v = NULL;
	int cap = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t l = strlen(e->d_name);
		if (l < 5 || strcmp(e->d_name + l - 4, ".cap"))
			continue;
		if (*count == cap) {
			cap = cap ? 2 * cap : 256;
			v = realloc(v, cap * sizeof *v);
		}
		size_t pl = strlen(dir) + l + 2;
		v[*count] = malloc(pl);
		snprintf(v[*count], pl, "%s/%s", dir, e->d_name);
		(*count)++;
	}
	closedir(d);
	qsort(v, *count, sizeof *v, cmp_names);
	return v;
}

static void passthrough_fn(hle_ctx *ctx)
{
	if (ctx->def && ctx->def->exits)
		hle_run68k_until(ctx, ctx->def->exits);   /* HLE_RANGE */
	else
		hle_call68k_orig(ctx, ctx->pc);
}

static bool *g_saved_enable;

static void set_hle_for_side(const hle_def *target, bool c_side, const opts *o)
{
	for (int i = 0; i < hle_count(); i++) {
		const hle_def *d = hle_get(i);
		char nm[16];
		snprintf(nm, sizeof nm, "$%06X", d->addr);
		bool on = o->with_hle ? g_saved_enable[i] : false;
		if (d->addr == target->addr)
			on = false;                 /* entry is invoked explicitly on the C side */
		hle_set_enabled(nm, on);
	}
	(void)c_side;
}

/* Per-function comparison exemptions, applied automatically (also by `check all`).
 * Every entry needs a proof in the C source that no caller reads what is ignored. */
static const struct {
	uint32_t addr;
	uint32_t regs;      /* bit n = Dn, bit 8+n = An */
	uint16_t sr;        /* CCR bits */
} dt_ignores[] = {
	/* walk.c: $005E4A and its tails end at one rts back into vblank_main, whose caller
	 * (VBlank $0008BC) restores D0-D7/A0-A6 and SR; vblank_main reads no register. */
	{ 0x005E4A, 0x7FFF, 0x1F },
	{ 0x006AE8, 0x7FFF, 0x1F },
	{ 0x006B28, 0x7FFF, 0x1F },
	{ 0x006C28, 0x7FFF, 0x1F },
	{ 0x006D24, 0x7FFF, 0x1F },
	/* room.c: registers/flags are dead for the callers (see the comments in room.c) */
	{ 0x00758A, 0x7FFF, 0x1F },   /* room_load */
	{ 0x007950, 0x7FFF, 0x1F },   /* room_draw_view */
	{ 0x007A8C, 0x7FFF, 0x1F },   /* room_setup_actors */
	{ 0x007C04, 0x0000, 0x1F },   /* objects_update: flags only */
	{ 0x007F22, 0x7FFF, 0x1F },   /* game_state_init */
	{ 0x007D54, 0x0000, 0x1F },   /* overlay_apply: flags only */
	/* sprite.c (#15): see the proofs in sprite.c */
	{ 0x00D3F0, 0x3EFF, 0x1F },   /* frame_pack_48x80: only caller actor_animate discards regs */
	{ 0x00D42C, 0x3EFF, 0x1F },   /* frame_pack_80x48 */
	{ 0x00D468, 0x3EFF, 0x1F },   /* frame_scale_48x80 */
	{ 0x00DA6A, 0x3EFF, 0x1F },   /* frame_scale_80x48 */
	{ 0x00B0B8, 0x0000, 0x1F },   /* actor_animate: flags only (registers restored by movem) */
	{ 0x00AF24, 0x0040, 0x00 },   /* actor_dma ("native" group): D6 = HV counter read; dead */
	/* hud_dma ("native" group, frameloop.c): X = bit 7 of the HV word (the H counter, which the
	 * sandbox does not freeze); its only caller is vblank_main, which reads no flags */
	{ 0x005C8E, 0x0000, 0x10 },
	/* ---- #21 menus (menus.c, menu_password.c, menu_debug.c, soundtest.c): the proofs are in the
	 * comment of each function ("live out").  Flags are dead everywhere in the menu code: the only
	 * instructions that read X are the roxl in the password routines ($008710, $009392, $009382),
	 * which are compared with X; every conditional branch after a range exit is preceded by a
	 * flag-setting instruction (tst / btst / cmp / move). */
	{ 0x00819E, 0x7EFF, 0x1F },   /* title_setup: live a0 */
	{ 0x0083E8, 0x0000, 0x1F },   /* title_bats_start */
	{ 0x00840E, 0x0000, 0x1F },   /* title_bats_first */
	{ 0x008416, 0x3FBF, 0x1F },   /* title_bats_frame: live d6, a6 */
	{ 0x0084AE, 0x3FBF, 0x1F },   /* title_bats_wait: live d6, a6 */
	{ 0x00851E, 0x7CFE, 0x1F },   /* menu_open: live d0, a0, a1 */
	{ 0x008590, 0x7FFE, 0x1F },   /* menu_show: live d0 (the item count popped for menu_select) */
	{ 0x0085D2, 0x7FFF, 0x1F },   /* menu_dispatch: nothing live in the targets */
	{ 0x008D36, 0x7FFF, 0x1F },   /* menu_choose */
	{ 0x008D4A, 0x7FFE, 0x1F },   /* menu_choose_show: live d0 (the count) */
	{ 0x008D6A, 0x7F0F, 0x1F },   /* menu_select: live d4-d7 (the cursor state) */
	{ 0x008D8E, 0x7F0F, 0x1F },
	{ 0x008D96, 0x7F0F, 0x1F },
	{ 0x008DA6, 0x7F0F, 0x1F },
	{ 0x008E78, 0x7F0F, 0x1F },
	{ 0x008EE6, 0x7FFE, 0x1F },   /* menu_select_done: live d0 (the choice) */
	{ 0x008F94, 0x0000, 0x1F },   /* quit_confirm_done */
	{ 0x008F9A, 0x0000, 0x1F },   /* soft_reset_begin */
	{ 0x008FB8, 0x0000, 0x1F },   /* menu_play_blake / _carnival */
	{ 0x008FC2, 0x0000, 0x1F },
	{ 0x008FCA, 0x0000, 0x1F },   /* mystery_menu */
	{ 0x008FEA, 0x7FFF, 0x1F },   /* mystery_menu_done: nothing live */
	{ 0x00873E, 0x0107, 0x1F },   /* password_scramble: d0-d2, a0 scratch (only caller loads them afresh) */
	{ 0x00876C, 0x010F, 0x1F },   /* password_unscramble: d0-d3, a0 */
	{ 0x008660, 0x7FFF, 0x1F },   /* password_show: every register scratch for all callers */
	{ 0x009392, 0x0000, 0x0F },   /* password_read_bit: X is the result */
	{ 0x009382, 0x0000, 0x0F },   /* password_read_byte */
	{ 0x009298, 0x7F0F, 0x1F },   /* password_check: live d4-d7 */
	{ 0x00933E, 0x7FFF, 0x1F },   /* password_accept */
	{ 0x0093A2, 0x0000, 0x1F },   /* password key handlers */
	{ 0x0093B6, 0x0000, 0x1F },
	{ 0x0093CC, 0x0000, 0x1F },
	{ 0x0093DC, 0x0000, 0x1F },
	{ 0x00927E, 0x0000, 0x1F },   /* password_cancel */
	{ 0x009010, 0x7F0F, 0x1F },   /* password_screen_*: live d4-d7 (+ a0 where the icon handler is called) */
	{ 0x00903A, 0x7E0F, 0x1F },
	{ 0x009052, 0x7E0F, 0x1F },
	{ 0x009160, 0x7F0F, 0x1F },
	{ 0x0091D8, 0x7F0F, 0x1F },
	{ 0x00940C, 0x7F1F, 0x1F },   /* sound_test: live d5-d7 */
	{ 0x00944E, 0x7F1F, 0x1F },
	{ 0x00946C, 0x7F1F, 0x1F },
	{ 0x00947C, 0x7D1F, 0x1F },
	{ 0x0096AC, 0x7F1F, 0x1F },
	{ 0x009648, 0x7FFF, 0x1F },
	{ 0x0088D8, 0x0000, 0x1F },   /* rooms browser: live d3-d5, d7, a2 (+d0 at the rts) */
	{ 0x0088FC, 0x0001, 0x1F },
	{ 0x00892C, 0x7B47, 0x1F },
	{ 0x008976, 0x7B46, 0x1F },
	{ 0x008AB4, 0x0000, 0x1F },
	{ 0x008AD4, 0x0001, 0x1F },
	{ 0x008B00, 0x7B47, 0x1F },
	{ 0x008B1A, 0x7B46, 0x1F },
	{ 0x0096F4, 0x0000, 0x1F },   /* menu_load_screen */
	{ 0x009710, 0x0000, 0x1F },
	{ 0x0088AC, 0x0000, 0x1F },   /* debug_rooms_pick */
	{ 0x0088B6, 0x0000, 0x1F },
	{ 0x008C40, 0x0000, 0x1F },   /* debug_objects */
	{ 0x008C60, 0x0000, 0x1F },   /* debug_objects_pick: flags only (d0 = location, d1/d2/a0 untouched or pushed) */
	{ 0x008C78, 0x7FFF, 0x1F },   /* debug_objects_move: nothing live at $008CC0 */
	{ 0x008CC0, 0x0000, 0x1F },
	/* scaled blits (blit.c): d3-d7 are the loop counters; every caller restores the registers or is a
	 * range that leaves them undefined.  Flags are compared. */
	{ 0x00E40C, 0x00F8, 0x00 },
	{ 0x00F272, 0x00F8, 0x00 },
	/* camera_update: d0-d3, a0 scratch and flags dead for its only caller vblank_main (camera.c) */
	{ 0x00FA78, 0x010F, 0x1F },
	/* palette_cycle: only caller vblank_main (palcycle.c); everything it touches is scratch */
	{ 0x00A7CE, 0x033F, 0x1F },
};

static int do_run(const hle_def *target, const opts *o)
{
	char dirbuf[512];
	const char *dir = o->states;
	if (!dir) {
		default_dir(dirbuf, sizeof dirbuf, target->addr);
		dir = dirbuf;
	}
	int n;
	char **files = list_states(dir, &n);
	if (!n) {
		printf("%s $%06X: no states in %s (run: difftest capture %06X 200)\n", target->name, target->addr, dir, target->addr);
		return 1;
	}
	if (o->max && n > o->max)
		n = o->max;

	hle_def pass = *target;
	pass.fn = passthrough_fn;
	const hle_def *cdef = o->passthrough ? &pass : target;

	sb_snap *ref = calloc(1, sizeof *ref), *got = calloc(1, sizeof *got);
	int pass_n = 0, fail_n = 0, err_n = 0;
	unsigned long cyc_min = ~0ul, cyc_max = 0, cyc_sum = 0, ins_sum = 0, c_sum = 0;
	int nvar = o->npokes ? o->npokes : 1;
	for (int k = 0; k < n * nvar; k++) {
		int i = k / nvar;
		const char *poke = o->npokes ? o->pokes[k % nvar] : NULL;
		cap_state cs;
		if (!cap_read(files[i], &cs)) {
			printf("%s: unreadable capture\n", files[i]);
			err_n++;
			continue;
		}
		if (cs.addr != target->addr && !o->entry) {
			printf("%s: captured at $%06X, not $%06X\n", files[i], cs.addr, target->addr);
			cap_free(&cs);
			err_n++;
			continue;
		}
		/* reference: original 68k */
		set_hle_for_side(target, false, o);
		cap_apply(&cs);
		if (o->entry)
			gx_m68k_set_pc(target->addr);
		if (poke) apply_poke(poke);
		uint32_t sp0 = gx_m68k_dar()[15];
		uint32_t entry[16];
		memcpy(entry, gx_m68k_dar(), sizeof entry);
		sb_begin();
		sb_record_begin();
		unsigned long insns, cycles;
		uint32_t min_sp;
		bool ok = sb_run_68k_until(target->exits, 50000000ul, &insns, &cycles, &min_sp);
		sb_snapshot(ref);
		sb_end();
		if (!ok) {
			printf("%s: original did not return (pc $%06X after %lu insns)\n", files[i], gx_m68k_pc(), insns);
			err_n++;
			cap_free(&cs);
			sb_snap_free(ref);
			continue;
		}
		cyc_sum += cycles;
		ins_sum += insns;
		if (cycles < cyc_min) cyc_min = cycles;
		if (cycles > cyc_max) cyc_max = cycles;

		/* C replacement */
		set_hle_for_side(target, true, o);
		cap_apply(&cs);
		if (o->entry)
			gx_m68k_set_pc(target->addr);
		if (poke) apply_poke(poke);
		sb_begin();
		sb_record_begin();
		uint32_t ret0 = bus_r32(gx_m68k_dar()[15]) & 0xFFFFFF;
		int charged = hle_invoke(cdef);
		c_sum += charged > 0 ? (unsigned long)charged : 0;
		/* a C function that ends in a tail jump into 68k code (hle_jump) is followed
		 * until it returns like the original (ranges stop at their exit at once) */
		if (!sb_run_68k_to(sp0, ret0, target->exits, 50000000ul)) {
			printf("%s: C side did not return (pc $%06X)\n", files[i], gx_m68k_pc());
			err_n++;
			sb_snapshot(got);
			sb_end();
			sb_snap_free(got);
			cap_free(&cs);
			continue;
		}
		sb_snapshot(got);
		sb_end();

		uint32_t lo = sp0 - o->stack_window;
		if (min_sp < lo)
			lo = min_sp;
		/* a range ends with everything below its exit SP dead: with a negative stack
		 * delta (HLE_EXIT_SP) what it pushed, [exit SP, entry SP), is live data and
		 * compared; with a positive delta what it popped, [entry SP, exit SP), is dead
		 * (and often overwritten by later calls inside the range) */
		uint32_t hi = sp0;
		if (target->exits)
			hi = ref->a[7];
		if (lo > hi)
			lo = hi;
		sb_cmp_opts co = o->cmp;
		for (size_t k = 0; k < sizeof dt_ignores / sizeof dt_ignores[0]; k++)
			if (dt_ignores[k].addr == target->addr) {
				co.ignore_regs |= dt_ignores[k].regs;
				co.ignore_sr |= dt_ignores[k].sr;
			}
		co.quiet = true;
		int bad = sb_compare(ref, got, lo, hi, &co, stdout);
		if (bad) {
			fail_n++;
			if (fail_n == 1 || o->keep_going) {
				printf("FAIL %s (frame %u, call #%u): %d categor%s differ\n", files[i], cs.frame, cs.index,
				       bad, bad == 1 ? "y" : "ies");
				if (poke) printf("  poke: %s\n", poke);
				printf("  entry:");
				for (int r = 0; r < 16; r++)
					printf("%s%c%d=%08X", r == 8 ? "\n        " : " ", r < 8 ? 'D' : 'A', r & 7, entry[r]);
				printf("\n");
				co.quiet = false;
				sb_compare(ref, got, lo, hi, &co, stdout);
			}
		} else {
			pass_n++;
			if (o->verbose)
				printf("ok   %s (frame %u, %lu insns, %lu cycles, %d port writes, exit $%06X SP%+d)\n", files[i], cs.frame, insns, cycles, ref->nev, ref->pc, (int)(ref->a[7] - sp0));
		}
		sb_snap_free(ref);
		sb_snap_free(got);
		cap_free(&cs);
		if (fail_n && !o->keep_going)
			break;
	}
	int tested = pass_n + fail_n;
	printf("%s $%06X: %d/%d states equal%s", target->name, target->addr, pass_n, tested,
	       o->passthrough ? " (passthrough)" : "");
	if (err_n)
		printf(", %d errors", err_n);
	if (pass_n + fail_n)
		printf("; original: %lu insns avg, 68k cycles min/avg/max %lu/%lu/%lu; C charged avg %lu", ins_sum / tested,
		       cyc_min, cyc_sum / tested, cyc_max, c_sum / tested);
	printf("\n");
	for (int i = 0; i < n; i++)
		free(files[i]);
	free(files);
	free(ref);
	free(got);
	return fail_n || err_n || !tested;
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv)
{
	if (argc < 2)
		usage();
	const char *mode = argv[1];
	opts o = { 0 };
	o.every = 0;                        /* auto-spread */
	o.n_capture = 200;
	o.stack_window = 256;
	const char *pos[64];
	int npos = 0;
	for (int i = 2; i < argc; i++) {
		const char *a = argv[i];
#define ARG() (i + 1 < argc ? argv[++i] : (usage(), ""))
		if (!strcmp(a, "--states") || !strcmp(a, "--out")) o.states = o.out = ARG();
		else if (!strcmp(a, "--max")) o.max = atoi(ARG());
		else if (!strcmp(a, "--n")) o.n_capture = atoi(ARG());
		else if (!strcmp(a, "--script")) { if (o.nscripts < 16) o.scripts[o.nscripts++] = ARG(); }
		else if (!strcmp(a, "--every")) o.every = atoi(ARG());
		else if (!strcmp(a, "--skip")) o.skip = atoi(ARG());
		else if (!strcmp(a, "--hle")) o.hle_spec = ARG();
		else if (!strcmp(a, "--ignore-regs")) o.cmp.ignore_regs = parse_regs(ARG());
		else if (!strcmp(a, "--ignore-flags")) o.cmp.ignore_sr = 0x1F;
		else if (!strcmp(a, "--strict-stack")) o.cmp.strict_stack = true;
		else if (!strcmp(a, "--stack-window")) o.stack_window = (uint32_t)strtoul(ARG(), NULL, 0);
		else if (!strcmp(a, "--with-hle")) o.with_hle = true;
		else if (!strcmp(a, "--poke")) { if (o.npokes < 1024) o.pokes[o.npokes++] = ARG(); }
		else if (!strcmp(a, "--passthrough")) o.passthrough = true;
		else if (!strcmp(a, "--keep-going")) o.keep_going = true;
		else if (!strcmp(a, "-v")) o.verbose = true;
		else if (!strcmp(a, "--entry")) o.entry = true;
		else if (a[0] == '-' && a[1]) usage();
		else if (npos < 64) pos[npos++] = a;
	}

	char err[512];
	const char *rom = emu_find_rom(getenv("SCOOBY_ROM"), argv[0]);
	if (!rom) {
		fprintf(stderr, "rom/baserom.md not found (set SCOOBY_ROM)\n");
		return 1;
	}
	if (!emu_init(rom, "build", err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	/* frame-loop model (#25): HV readers in C read the live counter here (the sandbox freezes
	 * the V counter for both sides), so they compare with the 68k */
	frameloop_set_hv_live(true);
	g_saved_enable = calloc(hle_count() + 1, sizeof *g_saved_enable);
	for (int i = 0; i < hle_count(); i++)
		g_saved_enable[i] = hle_is_enabled(i);

	if (!strcmp(mode, "list")) {
		hle_list();
		return 0;
	}
	if (!strcmp(mode, "capture")) {
		if (npos != 2)
			usage();
		uint32_t addr = (uint32_t)strtoul(pos[0] + (pos[0][0] == '$'), NULL, 16);
		char dir[512];
		if (o.out)
			snprintf(dir, sizeof dir, "%s", o.out);
		else
			default_dir(dir, sizeof dir, addr);
		return do_capture(addr, atoi(pos[1]), dir, &o);
	}
	if (!strcmp(mode, "capture-multi")) {
		if (npos < 3)
			usage();
		hle_set_all(false);
		if (o.hle_spec)
			hle_configure(o.hle_spec);
		int n = atoi(pos[0]);
		emu_reset();
		for (int i = 2; i < npos; i++) {
			uint32_t addr = (uint32_t)strtoul(pos[i] + (pos[i][0] == '$'), NULL, 16);
			char dir[600];
			snprintf(dir, sizeof dir, "%s/%06X_%s", o.out ? o.out : "runs/states", addr, pos[1]);
			if (i == 2)
				capture_start(addr, n, dir, 1, 0);
			else
				capture_add(addr, n, dir, 1, 0);
		}
		int rc = play_scripts(&o, "runs");
		capture_stop();
		for (int i = 2; i < npos; i++)
			printf("captured %d entry states of %s (%s)\n", capture_slot_count(i - 2), pos[i], pos[1]);
		return rc;
	}
	if (strcmp(mode, "run") && strcmp(mode, "check"))
		usage();

	/* resolve targets */
	const hle_def *targets[512];
	int nt = 0;
	for (int i = 0; i < npos; i++) {
		if (!strcmp(pos[i], "all")) {
			for (int k = 0; k < hle_count() && nt < 512; k++)
				if (hle_get(k)->addr != 0x01006C)   /* hblank_verb_bar: raster wait, not comparable */
					targets[nt++] = hle_get(k);
			continue;
		}
		const hle_def *d = hle_find(pos[i]);
		if (!d) {
			fprintf(stderr, "unknown function %s (see: difftest list)\n", pos[i]);
			return 2;
		}
		targets[nt++] = d;
	}
	if (!nt)
		usage();

	int rc = 0;
	for (int t = 0; t < nt; t++) {
		const hle_def *d = targets[t];
		if (!strcmp(mode, "check")) {
			char dir[512];
			if (o.states)
				snprintf(dir, sizeof dir, "%s", o.states);
			else
				default_dir(dir, sizeof dir, d->addr);
			int have = 0;
			char **f = list_states(dir, &have);
			for (int k = 0; k < have; k++)
				free(f[k]);
			free(f);
			if (have < o.n_capture) {
				/* fresh emulator run for each capture: reset by reloading the ROM state */
				emu_reset();
				if (do_capture(d->addr, o.n_capture, dir, &o))
					fprintf(stderr, "warning: only %d states captured for %s\n", capture_count(), d->name);
			}
		}
		rc |= do_run(d, &o);
	}
	return rc;
}
