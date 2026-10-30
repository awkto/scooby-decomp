/* Finding the ROM: see romfind.h. */
#include "romfind.h"

#include <SDL.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "emu.h"
#include "overlay.h"
#include "sha1.h"

bool rom_verify(const char *path, char *err, size_t errlen)
{
	struct stat st;
	if (stat(path, &st) || !S_ISREG(st.st_mode)) {
		snprintf(err, errlen, "%s: not a readable file", path);
		return false;
	}
	if (st.st_size < 0x1000 || st.st_size > 8 * 1024 * 1024) {
		snprintf(err, errlen, "%s: not a Genesis ROM (size %ld)", path, (long)st.st_size);
		return false;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(err, errlen, "%s: cannot open", path);
		return false;
	}
	uint8_t *buf = malloc((size_t)st.st_size);
	bool ok = buf && fread(buf, 1, (size_t)st.st_size, f) == (size_t)st.st_size;
	fclose(f);
	if (!ok) {
		free(buf);
		snprintf(err, errlen, "%s: read error", path);
		return false;
	}
	char hex[41];
	sha1_hex(buf, (size_t)st.st_size, hex);
	free(buf);
	if (strcmp(hex, SCOOBY_ROM_SHA1)) {
		snprintf(err, errlen, "%s is not the right ROM (sha1 %s); this needs Scooby-Doo Mystery (USA) %s",
		         path, hex, SCOOBY_ROM_SHA1);
		return false;
	}
	return true;
}

static bool exe_dir(const char *argv0, char *dir, size_t n)
{
	char exe[1024];
	ssize_t r = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (r <= 0 && argv0)
		r = (ssize_t)snprintf(exe, sizeof exe, "%s", argv0);
	if (r <= 0)
		return false;
	exe[r] = 0;
	char *s = strrchr(exe, '/');
	if (!s)
		return false;
	*s = 0;
	snprintf(dir, n, "%s", exe);
	return true;
}

static bool scan_dir(const char *dir, char *out, size_t n)
{
	DIR *d = opendir(dir);
	if (!d)
		return false;
	char err[256];
	int tries = 0;
	bool found = false;
	for (struct dirent *e; !found && tries < 40 && (e = readdir(d));) {
		const char *x = strrchr(e->d_name, '.');
		if (!x || (strcasecmp(x, ".md") && strcasecmp(x, ".bin") && strcasecmp(x, ".gen")))
			continue;
		char p[1400];
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		tries++;
		if (rom_verify(p, err, sizeof err)) {
			snprintf(out, n, "%s", p);
			found = true;
		}
	}
	closedir(d);
	return found;
}

bool rom_locate(const char *explicit_path, const char *ini_rom, const char *argv0, char *out, size_t n)
{
	char err[256], dir[1024];
	if (explicit_path) {
		snprintf(out, n, "%s", explicit_path);
		return true;
	}
	if (ini_rom && *ini_rom && rom_verify(ini_rom, err, sizeof err)) {
		snprintf(out, n, "%s", ini_rom);
		return true;
	}
	if (exe_dir(argv0, dir, sizeof dir) && scan_dir(dir, out, n))
		return true;
	if (rom_verify("rom/baserom.md", err, sizeof err)) {
		snprintf(out, n, "rom/baserom.md");
		return true;
	}
#ifndef SCOOBY_DIST
	{   /* development tree: the repo's rom/ (not in release builds) */
		const char *p = emu_find_rom(NULL, argv0);
		if (p && rom_verify(p, err, sizeof err)) {
			snprintf(out, n, "%s", p);
			return true;
		}
	}
#endif
	return false;
}

/* run a picker command, read one line from it */
static bool picker(const char *cmd, char *out, size_t n)
{
	FILE *p = popen(cmd, "r");
	if (!p)
		return false;
	bool ok = fgets(out, (int)n, p) != NULL;
	int rc = pclose(p);
	out[strcspn(out, "\r\n")] = 0;
	return ok && rc == 0 && *out;
}

static bool have(const char *prog)
{
	char cmd[128];
	snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", prog);
	return system(cmd) == 0;
}

static bool file_picker(char *out, size_t n)
{
	if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY"))
		return false;
	if (have("zenity"))
		return picker("zenity --file-selection --title='Select your Scooby-Doo Mystery ROM (.md/.bin)' 2>/dev/null", out, n);
	if (have("kdialog"))
		return picker("kdialog --title 'Select your Scooby-Doo Mystery ROM' --getopenfilename \"$HOME\" 2>/dev/null", out, n);
	return false;
}

/* a small window that waits for the ROM file to be dropped on it */
static bool drop_window(char *out, size_t n, char *err, size_t errlen)
{
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
		snprintf(err, errlen, "no ROM found and no display for a file picker");
		return false;
	}
	SDL_Window *w = SDL_CreateWindow("Scooby-Doo Mystery - ROM needed", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                                 768, 448, 0);
	SDL_Renderer *r = w ? SDL_CreateRenderer(w, -1, 0) : NULL;
	if (!r) {
		if (w)
			SDL_DestroyWindow(w);
		snprintf(err, errlen, "no ROM found and no window could be opened (%s)", SDL_GetError());
		return false;
	}
	SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
	SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, 192, 112);
	static uint32_t px[192 * 112];
	char note[400] = "";
	bool got = false;
	for (bool quit = false; !quit && !got;) {
		for (SDL_Event e; SDL_PollEvent(&e);) {
			if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE))
				quit = true;
			else if (e.type == SDL_DROPFILE) {
				char verr[300];
				if (rom_verify(e.drop.file, verr, sizeof verr)) {
					snprintf(out, n, "%s", e.drop.file);
					got = true;
				} else {
					snprintf(note, sizeof note, "THAT IS NOT THE RIGHT ROM. TRY AGAIN.");
				}
				SDL_free(e.drop.file);
			}
		}
		for (int i = 0; i < 192 * 112; i++)
			px[i] = 0xFF301810u;
		overlay_text_rgba(px, 192, 112, 8, 20, 0xFFFFFFFFu, "SCOOBY-DOO MYSTERY");
		overlay_text_rgba(px, 192, 112, 8, 40, 0xFFD0D0D0u, "DROP YOUR ROM FILE ON THIS WINDOW");
		overlay_text_rgba(px, 192, 112, 8, 52, 0xFFD0D0D0u, "SCOOBY-DOO MYSTERY (USA) .MD OR .BIN");
		overlay_text_rgba(px, 192, 112, 8, 64, 0xFFD0D0D0u, "ESC TO QUIT");
		if (*note)
			overlay_text_rgba(px, 192, 112, 8, 84, 0xFF6060FFu, note);
		SDL_UpdateTexture(t, NULL, px, 192 * 4);
		SDL_RenderCopy(r, t, NULL, NULL);
		SDL_RenderPresent(r);
		SDL_Delay(30);
	}
	SDL_DestroyTexture(t);
	SDL_DestroyRenderer(r);
	SDL_DestroyWindow(w);
	if (!got)
		snprintf(err, errlen, "no ROM selected");
	return got;
}

bool rom_ask_user(char *out, size_t n, char *err, size_t errlen)
{
	char p[1400];
	if (file_picker(p, sizeof p)) {
		if (rom_verify(p, err, errlen)) {
			snprintf(out, n, "%s", p);
			return true;
		}
		fprintf(stderr, "%s\n", err);      /* wrong file picked: fall through to the drop window */
	}
	return drop_window(out, n, err, errlen);
}
