#include "script.h"
#include "capture.h"
#include "emu.h"
#include "gx.h"
#include "hle.h"
#include "mouse.h"
#include "widescreen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct script {
	FILE *f;
	char outdir[1024];
	long frames_left;      /* of the current run/press */
	unsigned cur_pad;      /* pad for the current run/press */
	unsigned held;         /* `hold` buttons */
	bool release_after;    /* press: clear pad when done */
	int line;
	int errors;
	FILE *peek;            /* OUTDIR/peek.log (harness format) */
};

script *script_open(const char *path, const char *outdir)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		perror(path);
		return NULL;
	}
	script *s = calloc(1, sizeof *s);
	s->f = f;
	snprintf(s->outdir, sizeof s->outdir, "%s", outdir ? outdir : ".");
	return s;
}

static void out_path(script *s, char *buf, size_t n, const char *name, const char *ext)
{
	if (strchr(name, '/'))
		snprintf(buf, n, "%s", name);
	else
		snprintf(buf, n, "%s/%s.%s", s->outdir, name, ext);
}

static void err(script *s, const char *msg, const char *line)
{
	fprintf(stderr, "script line %d: %s: %s", s->line, msg, line);
	s->errors++;
}

bool script_step(script *s, unsigned *pad)
{
	for (;;) {
		if (s->frames_left > 0) {
			s->frames_left--;
			*pad = s->cur_pad;
			return true;
		}
		char line[1024], cmd[64], a1[512], a2[512], a3[512];
		if (!fgets(line, sizeof line, s->f))
			return false;
		s->line++;
		if (sscanf(line, "%63s", cmd) != 1 || cmd[0] == '#')
			continue;
		char path[1600];
		long n;
		if (!strcmp(cmd, "run") && sscanf(line, "%*s %ld", &n) == 1) {
			s->frames_left = n;
			s->cur_pad = s->held;
		} else if (!strcmp(cmd, "press") && sscanf(line, "%*s %511s %ld", a1, &n) == 2) {
			s->frames_left = n;
			s->cur_pad = emu_pad_from_letters(a1);
		} else if (!strcmp(cmd, "hold") && sscanf(line, "%*s %511s", a1) == 1) {
			s->held = strcmp(a1, "-") ? emu_pad_from_letters(a1) : 0;
		} else if (!strcmp(cmd, "shot") && sscanf(line, "%*s %511s", a1) == 1) {
			out_path(s, path, sizeof path, a1, "ppm");
			if (!emu_write_ppm(path))
				err(s, "cannot write", path);
		} else if (!strcmp(cmd, "ram") && sscanf(line, "%*s %511s", a1) == 1) {
			out_path(s, path, sizeof path, a1, "ram");
			if (!emu_write_ram(path))
				err(s, "cannot write", path);
		} else if (!strcmp(cmd, "vdp") && sscanf(line, "%*s %511s", a1) == 1) {
			out_path(s, path, sizeof path, a1, "x");
			path[strlen(path) - 2] = 0;
			if (!emu_write_vdp(path))
				err(s, "cannot write", path);
		} else if (!strcmp(cmd, "save") && sscanf(line, "%*s %511s", a1) == 1) {
			out_path(s, path, sizeof path, a1, "state");
			if (!emu_state_save_file(path))
				err(s, "cannot save", path);
		} else if (!strcmp(cmd, "load") && sscanf(line, "%*s %511s", a1) == 1) {
			out_path(s, path, sizeof path, a1, "state");
			if (!emu_state_load_file(path))
				err(s, "cannot load", path);
		} else if (!strcmp(cmd, "hle") && sscanf(line, "%*s %511s", a1) == 1) {
			if (!hle_configure(a1))
				err(s, "bad hle spec", line);
		} else if (!strcmp(cmd, "capture") && sscanf(line, "%*s %511s %ld %511s", a1, &n, a2) == 3) {
			int every = 1, skip = 0;
			if (sscanf(line, "%*s %*s %*s %*s %511s", a3) == 1)
				every = atoi(a3);
			if (sscanf(line, "%*s %*s %*s %*s %*s %511s", a3) == 1)
				skip = atoi(a3);
			capture_start((uint32_t)strtoul(a1 + (a1[0] == '$'), NULL, 16), (int)n, a2, every, skip);
		} else if (!strcmp(cmd, "regs")) {
			const uint32_t *r = gx_m68k_dar();
			fprintf(stderr, "frame %ld pc %06X  D:", emu_frame_count(), gx_m68k_pc());
			for (int i = 0; i < 8; i++)
				fprintf(stderr, " %08X", r[i]);
			fprintf(stderr, "\n  A:");
			for (int i = 8; i < 16; i++)
				fprintf(stderr, " %08X", r[i]);
			fprintf(stderr, "\n");
		} else if (!strcmp(cmd, "poke") && sscanf(line, "%*s %511s %ld %511s", a1, &n, a2) == 3) {
			/* poke ADDR W VAL: write W (1/2/4) bytes of work RAM, big-endian (harness syntax) */
			uint32_t addr = (uint32_t)strtoul(a1 + (a1[0] == '$'), NULL, 16);
			uint32_t val = (uint32_t)strtoul(a2 + (a2[0] == '$'), NULL, 16);
			for (long i = 0; i < n; i++)
				RAM8(addr + i) = (uint8_t)(val >> (8 * (n - 1 - i)));
		} else if (!strcmp(cmd, "peek") && sscanf(line, "%*s %511s %511s", a1, a2) == 2) {
			int w = 2;
			sscanf(line, "%*s %*s %*s %d", &w);
			uint32_t a = (uint32_t)strtoul(a2 + (a2[0] == '$'), NULL, 16), v = 0;
			for (int i = 0; i < w && i < 4; i++)
				v = (v << 8) | emu_ram8(a + (uint32_t)i);
			if (!s->peek) {
				snprintf(path, sizeof path, "%s/peek.log", s->outdir);
				s->peek = fopen(path, "a");
			}
			if (s->peek) {
				fprintf(s->peek, "%ld %s %0*x\n", emu_frame_count(), a1, w * 2, v);
				fflush(s->peek);
			}
		} else if (!strcmp(cmd, "mouse") && sscanf(line, "%*s %511s %511s", a1, a2) == 2) {
			if (!strcmp(a1, "leave"))
				mouse_leave();
			else
				mouse_move(atoi(a1), atoi(a2));
		} else if (!strcmp(cmd, "mouse") && sscanf(line, "%*s %511s", a1) == 1 && !strcmp(a1, "leave")) {
			mouse_leave();
		} else if (!strcmp(cmd, "click") && sscanf(line, "%*s %511s", a1) == 1) {
			int x, y;
			if (sscanf(line, "%*s %*s %d %d", &x, &y) == 2)
				mouse_move(x, y);
			int b = a1[0] == 'R' || a1[0] == 'r' ? MOUSE_RIGHT : a1[0] == 'M' || a1[0] == 'm' ? MOUSE_MIDDLE : MOUSE_LEFT;
			mouse_click(b);
		} else if (!strcmp(cmd, "fmouse") && sscanf(line, "%*s %511s %511s", a1, a2) == 2) {
			/* fmouse X Y / fclick L|R|M X Y: like mouse / click, in pixels of the shown frame
			 * (the wide frame with widescreen, mapped like the window's mouse, widescreen.c) */
			int gx, gy;
			ws_frame_to_game(atoi(a1), atoi(a2), &gx, &gy);
			mouse_move(gx, gy);
		} else if (!strcmp(cmd, "fclick") && sscanf(line, "%*s %511s", a1) == 1) {
			int x, y, gx, gy;
			if (sscanf(line, "%*s %*s %d %d", &x, &y) == 2) {
				ws_frame_to_game(x, y, &gx, &gy);
				mouse_move(gx, gy);
			}
			int b = a1[0] == 'R' || a1[0] == 'r' ? MOUSE_RIGHT : a1[0] == 'M' || a1[0] == 'm' ? MOUSE_MIDDLE : MOUSE_LEFT;
			mouse_click(b);
		} else if (!strcmp(cmd, "wheel") && sscanf(line, "%*s %ld", &n) == 1) {
			mouse_wheel((int)n);
		} else if (!strcmp(cmd, "echo")) {
			/* harness semantics: "FRAME TEXT" line in OUTDIR/peek.log (live drivers sync on it), also on stderr */
			const char *t = line + 5 < line + strlen(line) ? line + 5 : "\n";
			if (!s->peek) {
				snprintf(path, sizeof path, "%s/peek.log", s->outdir);
				s->peek = fopen(path, "a");
			}
			if (s->peek) {
				fprintf(s->peek, "%ld %s", emu_frame_count(), t);
				fflush(s->peek);
			}
			fputs(t, stderr);
		} else if (!strcmp(cmd, "trace") || !strcmp(cmd, "untrace")) {
			/* harness-only bus trace: accepted and ignored so driver scripts run unchanged */
		} else {
			err(s, "bad command", line);
		}
		/* after press, the pad is released for following `run`s (harness semantics) */
	}
}

int script_errors(const script *s) { return s->errors; }

void script_close(script *s)
{
	if (!s)
		return;
	if (s->peek)
		fclose(s->peek);
	fclose(s->f);
	free(s);
}
