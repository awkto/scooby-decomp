#include "script.h"
#include "capture.h"
#include "emu.h"
#include "gx.h"
#include "hle.h"

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
		} else if (!strcmp(cmd, "echo")) {
			fputs(line + 5 < line + strlen(line) ? line + 5 : "\n", stderr);
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
	fclose(s->f);
	free(s);
}
