/* scoobyport: native SDL2 host for Scooby-Doo Mystery (Genesis Plus GX core + HLE layer).
 *
 * Keys: arrows = D-pad, Z/A = A, X/S = B, C/D = C, Enter = Start
 *       F1 debug overlay, F2 toggle all C replacements (HLE), F5 save state, F9 load state,
 *       F6/F7 previous/next state slot, P pause, N or . frame advance (pauses),
 *       Tab (hold) fast forward, F11 or Alt+Enter fullscreen, F12 screenshot, Esc quit,
 *       F3 output filter (nearest / smooth / crt), F8 game saves (8 slots, keyboard + mouse).
 * Gamepad (SDL GameController): D-pad / left stick, X/Y = A, A = B, B/RB = C, Start,
 *       Back = pause.
 * Mouse (docs/mouse.md; --no-mouse disables it): left click = walk there / use the
 *       selected (or default) verb on an object / pick a verb or item on the bar,
 *       right click = Look at an object (or leave pointer mode), middle = Start,
 *       wheel = inventory page.
 * Run with --help for the command line (headless --script / --frames / --screenshot,
 * entry-state --capture, --hle selection). */
#include <SDL.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "capture.h"
#include "emu.h"
#include "hostmenu.h"
#include "ipprof.h"
#include "hle.h"
void sndlog_init(void);
#include "mouse.h"
#include "options.h"
#include "overlay.h"
#include "present.h"
#include "render_emu.h"
#include "romfind.h"
#include "saveslot.h"
#include "script.h"

typedef struct {
	const char *rom, *script, *outdir, *screenshot, *window_shot, *savedir, *hle, *renderer, *config, *dump_ram;
	int load_slot, save_slot;
	bool list_slots;
	present_filter filter;
	const char *cap_dir;
	uint32_t cap_addr;
	int cap_n, cap_every, cap_skip;
	long frames;            /* -1: unlimited */
	int scale;
	bool fullscreen, vsync, mute, no_throttle, overlay, list_hle, realtime_script, no_mouse;
} args_t;

static void usage(const char *argv0)
{
	printf("usage: %s [options]\n"
	       "  --rom PATH              ROM (default: ini, next to the binary, file picker; sha1 must match)\n"
	       "  --options K=V,...       set options for this run (--options --json lists them all); an\n"
	       "                          ini file ~/.config/scooby-decomp/scooby.ini holds the permanent ones\n"
	       "  --config FILE           use this ini instead\n"
	       "  --load-slot N           start by loading game save N (1..8, see F8 in the game)\n"
	       "  --save-slot N           (after --script) store the running game in save slot N\n"
	       "  --list-slots            list the game saves and exit\n"
	       "  --dump-ram FILE         at exit, write the 64 KiB work RAM\n"
	       "  --scale N               initial window scale (default 3)\n"
	       "  --renderer R            native (default) | gpgx: the C renderer draws the picture and\n"
	       "                          the verb-bar split is a band, not a raster effect (#25)\n"
	       "  --filter F              nearest (default, whole-number scale) | smooth | crt\n"
	       "  --fullscreen            start fullscreen (F11 toggles)\n"
	       "  --no-vsync              don't sync presentation to the display\n"
	       "  --mute                  no audio\n"
	       "  --overlay               start with the debug overlay on (F1)\n"
	       "  --hle SPEC              C replacements: all (default) | none | [+-]name,...\n"
	       "  --list-hle              list C replacements and exit\n"
	       "  --script FILE           run a harness-style script first (unthrottled)\n"
	       "  --realtime-script       run the script (and --frames) at normal speed, with audio\n"
	       "  --frames N              exit after N frames (after the script)\n"
	       "  --screenshot FILE.ppm   at exit, write the last emulated frame\n"
	       "  --window-shot FILE.ppm  at exit, write the rendered window (scaling + overlay)\n"
	       "  --outdir DIR            where script shot/ram/save files go (default runs/port)\n"
	       "  --savedir DIR           game saves + quick states (default ~/.local/share/scooby-decomp)\n"
	       "  --capture ADDR N        save the first N entry states of the 68k function at ADDR\n"
	       "  --capture-dir DIR       (default runs/states/<ADDR>)  --capture-every K  --capture-skip K\n"
	       "  --no-throttle           run as fast as possible\n"
	       "  --no-mouse              ignore the mouse (keyboard/gamepad only)\n"
	       "Headless: SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy %s --script s.txt --screenshot out.ppm\n",
	       argv0, argv0);
}

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

/* ------------------------------------------------------------------ input */
static unsigned g_keys, g_padbtn;
static int g_axis_x, g_axis_y;

/* keyboard bindings: the key_* options (SDL names, several per button joined with |) */
#define MAX_KEYS_PER_BUTTON 6
static const struct { const char *opt; unsigned pad; } keybind[] = {
	{ "key_up", PAD_UP }, { "key_down", PAD_DOWN }, { "key_left", PAD_LEFT }, { "key_right", PAD_RIGHT },
	{ "key_a", PAD_A }, { "key_b", PAD_B }, { "key_c", PAD_C }, { "key_start", PAD_START },
};
static SDL_Keycode keymap[8][MAX_KEYS_PER_BUTTON];

static void keys_configure(void)
{
	for (int b = 0; b < 8; b++) {
		char list[256];
		snprintf(list, sizeof list, "%s", options_str(keybind[b].opt));
		int n = 0;
		for (char *save, *t = strtok_r(list, "|", &save); t; t = strtok_r(NULL, "|", &save)) {
			SDL_Keycode k = SDL_GetKeyFromName(t);
			if (k == SDLK_UNKNOWN)
				fprintf(stderr, "%s: unknown key name '%s' ignored\n", keybind[b].opt, t);
			else if (n < MAX_KEYS_PER_BUTTON)
				keymap[b][n++] = k;
		}
	}
}

static unsigned key_to_pad(SDL_Keycode k)
{
	unsigned m = 0;
	for (int b = 0; b < 8; b++)
		for (int i = 0; i < MAX_KEYS_PER_BUTTON; i++)
			if (keymap[b][i] == k)
				m |= keybind[b].pad;
	return m;
}

static unsigned button_to_pad(Uint8 b)
{
	switch (b) {
	case SDL_CONTROLLER_BUTTON_DPAD_UP: return PAD_UP;
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return PAD_DOWN;
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return PAD_LEFT;
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return PAD_RIGHT;
	case SDL_CONTROLLER_BUTTON_X: case SDL_CONTROLLER_BUTTON_Y: return PAD_A;
	case SDL_CONTROLLER_BUTTON_A: return PAD_B;
	case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return PAD_C;
	case SDL_CONTROLLER_BUTTON_START: return PAD_START;
	}
	return 0;
}

static unsigned live_pad(void)
{
	unsigned m = g_keys | g_padbtn;
	const int dz = 16000;
	if (g_axis_x < -dz) m |= PAD_LEFT;
	if (g_axis_x > dz) m |= PAD_RIGHT;
	if (g_axis_y < -dz) m |= PAD_UP;
	if (g_axis_y > dz) m |= PAD_DOWN;
	return m;
}

/* ------------------------------------------------------------------ video */
/* The game frame is RGBA (the native renderer's frame, or GPGX's RGB565 frame converted);
 * the overlay and the mouse pointer are drawn into a copy, then the output filter scales it
 * (render/present.c) and SDL puts it in the window (docs/subsystems/render.md "Output"). */
typedef struct {
	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex;
	int tw, th;                 /* texture size (game frame x prescale) */
	int gw, gh;                 /* game frame size */
	present_filter filter;
	uint32_t buf[RENDER_MAX_W * RENDER_MAX_H];     /* game frame + overlay + pointer */
	uint32_t *scaled;
	size_t scaled_cap;
} video_t;

static bool video_init(video_t *v, int scale, bool fullscreen, bool vsync, present_filter filter)
{
	v->filter = filter;
	v->win = SDL_CreateWindow("Scooby-Doo Mystery", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                          320 * scale, 224 * scale,
	                          SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	if (!v->win)
		return false;
	v->ren = SDL_CreateRenderer(v->win, -1, SDL_RENDERER_ACCELERATED | (vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
	if (!v->ren)
		v->ren = SDL_CreateRenderer(v->win, -1, 0);
	if (!v->ren)
		return false;
	return true;
}

static void video_draw(video_t *v)
{
	int ow, oh, r[4];
	SDL_GetRendererOutputSize(v->ren, &ow, &oh);
	present_rect(v->filter, v->gw, v->gh, ow, oh, r);
	SDL_Rect dst = { r[0], r[1], r[2], r[3] };
	SDL_SetRenderDrawColor(v->ren, 0, 0, 0, 255);
	SDL_RenderClear(v->ren);
	if (v->tex)
		SDL_RenderCopy(v->ren, v->tex, NULL, &dst);
}

static void video_present(video_t *v, const uint32_t *px, int w, int h)
{
	if (!v->ren || !px)
		return;
	int ow, oh;
	SDL_GetRendererOutputSize(v->ren, &ow, &oh);
	/* prescale on the CPU by the whole-number fit (crt: the effect needs the pixels; smooth:
	 * SDL does the fractional rest with linear filtering; nearest: SDL scales by k exactly) */
	int k = v->filter == PRESENT_NEAREST ? 1 : present_fit_scale(w, h, ow, oh);
	if (k > 8)
		k = 8;
	int tw = w * k, th = h * k;
	if (!v->tex || v->tw != tw || v->th != th) {
		if (v->tex)
			SDL_DestroyTexture(v->tex);
		v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, tw, th);
		SDL_SetTextureScaleMode(v->tex, v->filter == PRESENT_SMOOTH ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
		v->tw = tw;
		v->th = th;
	}
	v->gw = w;
	v->gh = h;
	if (k == 1) {
		SDL_UpdateTexture(v->tex, NULL, px, w * 4);
	} else {
		size_t need = (size_t)tw * th;
		if (need > v->scaled_cap) {
			free(v->scaled);
			v->scaled = malloc(need * 4);
			v->scaled_cap = need;
		}
		present_scale(px, w, h, w, k, v->filter, v->scaled);
		SDL_UpdateTexture(v->tex, NULL, v->scaled, tw * 4);
	}
	video_draw(v);
	SDL_RenderPresent(v->ren);
}

/* the frame to show: the native renderer's, or GPGX's (RGB565 -> RGBA) */
static bool game_frame(video_t *v, int *w, int *h)
{
	int nw, nh;
	const uint32_t *nat = render_emu_kind() == RENDERER_NATIVE ? render_emu_frame(&nw, &nh) : NULL;
	if (nat) {
		for (int y = 0; y < nh; y++)
			memcpy(v->buf + (size_t)y * nw, nat + (size_t)y * RENDER_MAX_W, (size_t)nw * 4);
		*w = nw;
		*h = nh;
		return true;
	}
	unsigned gw, gh;
	size_t pitch;
	const uint16_t *px = emu_video(&gw, &gh, &pitch);
	if (!px || gw > RENDER_MAX_W || gh > RENDER_MAX_H)
		return false;
	for (unsigned i = 0; i < gw * gh; i++)
		v->buf[i] = render_565_to_rgba(px[i]);
	*w = (int)gw;
	*h = (int)gh;
	return true;
}

static bool window_shot(video_t *v, const char *path)
{
	int w, h;
	if (!v->ren || SDL_GetRendererOutputSize(v->ren, &w, &h))
		return false;
	uint8_t *rgb = calloc((size_t)w * h, 3);
	/* re-render the last frame (the back buffer is undefined after present) */
	video_draw(v);
	bool ok = SDL_RenderReadPixels(v->ren, NULL, SDL_PIXELFORMAT_RGB24, rgb, w * 3) == 0;
	FILE *f = ok ? fopen(path, "wb") : NULL;
	if (f) {
		fprintf(f, "P6\n%d %d\n255\n", w, h);
		fwrite(rgb, 1, (size_t)w * h * 3, f);
		fclose(f);
	}
	free(rgb);
	return f != NULL;
}

/* mouse position (window coordinates) -> game pixel, through the placement of video_draw.
 * Window coordinates are converted to renderer output pixels first (they differ on HiDPI
 * displays). */
/* returns 1 and the game pixel when the pointer is on the image, 0 when off it, -1 if unknown */
static int window_to_game(video_t *v, int x, int y, int *gx, int *gy)
{
	int ww, wh, ow, oh, r[4];
	if (!v->win || !v->ren)
		return -1;
	SDL_GetWindowSize(v->win, &ww, &wh);
	if (SDL_GetRendererOutputSize(v->ren, &ow, &oh) || ww <= 0 || wh <= 0)
		return -1;
	int gw = v->gw ? v->gw : 256, gh = v->gh ? v->gh : 224;
	present_rect(v->filter, gw, gh, ow, oh, r);
	int px = x * ow / ww - r[0], py = y * oh / wh - r[1];
	if (px < 0 || py < 0 || px >= r[2] || py >= r[3] || r[2] <= 0 || r[3] <= 0)
		return 0;
	*gx = px * gw / r[2];
	*gy = py * gh / r[3];
	return 1;
}

static void mouse_from_window(video_t *v, int x, int y)
{
	int gx, gy, on = window_to_game(v, x, y, &gx, &gy);
	if (on == 0)
		mouse_leave();
	else if (on > 0)
		mouse_move(gx, gy);
}

/* ------------------------------------------------------------------ saves menu */
/* carry out what the GAME SAVES overlay asked for; `status` is the line it shows */
static void menu_do(hm_action act, int sl, long play_frames, char *status, size_t n, SDL_AudioDeviceID adev, bool mouse_on)
{
	char e[200];
	status[0] = 0;
	if (!hostmenu_active()) {            /* it closed itself (Esc / F8) */
		if (mouse_on)
			SDL_ShowCursor(SDL_DISABLE);
		return;
	}
	if (act == HM_SAVE) {
		if (slot_save_current(sl, play_frames, e, sizeof e))
			snprintf(status, n, "SAVED %d", sl);
		else
			snprintf(status, n, "%.50s", e);
	} else if (act == HM_ERASE) {
		snprintf(status, n, slot_erase(sl) ? "ERASED %d" : "NOTHING TO ERASE", sl);
	} else if (act == HM_LOAD) {
		if (slot_load_begin(sl, e, sizeof e)) {
			hostmenu_close();
			if (mouse_on)
				SDL_ShowCursor(SDL_DISABLE);
			if (adev)
				SDL_ClearQueuedAudio(adev);
		} else {
			snprintf(status, n, "%.50s", e);
		}
	}
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv)
{
	args_t a = { 0 };
	a.frames = -1;
	a.scale = 3;
	a.vsync = true;
	a.outdir = "runs/port";
	a.cap_every = 1;
	a.filter = PRESENT_NEAREST;

	/* options: defaults < ini < command line (docs: options.h).  --config is looked for first
	 * so that the ini is read before the flags that override it. */
	char cfgpath[1024], operr[300];
	options_default_path(cfgpath, sizeof cfgpath);
	for (int i = 1; i + 1 < argc; i++)
		if (!strcmp(argv[i], "--config"))
			snprintf(cfgpath, sizeof cfgpath, "%s", argv[i + 1]);
	options_init();
	options_load(cfgpath);
	for (int i = 1; i < argc; i++) {
		const char *s = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (usage(argv[0]), exit(2), ""))
#define SETOPT(k, v) do { if (!options_set(k, v, OPT_CLI, operr, sizeof operr)) { fprintf(stderr, "%s\n", operr); return 2; } } while (0)
		if (!strcmp(s, "--rom")) a.rom = NEXT();
		else if (!strcmp(s, "--config")) a.config = NEXT();
		else if (!strcmp(s, "--options")) {
			const char *v = NEXT();
			if (!strcmp(v, "--json") || !strcmp(v, "list")) {
				options_print(stdout, !strcmp(v, "--json"));
				return 0;
			}
			if (!options_parse_list(v, OPT_CLI, operr, sizeof operr)) {
				fprintf(stderr, "--options: %s\n", operr);
				return 2;
			}
		}
		else if (!strcmp(s, "--load-slot")) a.load_slot = atoi(NEXT());
		else if (!strcmp(s, "--save-slot")) a.save_slot = atoi(NEXT());
		else if (!strcmp(s, "--list-slots")) a.list_slots = true;
		else if (!strcmp(s, "--dump-ram")) a.dump_ram = NEXT();
		else if (!strcmp(s, "--scale")) SETOPT("scale", NEXT());
		else if (!strcmp(s, "--fullscreen")) SETOPT("fullscreen", "on");
		else if (!strcmp(s, "--no-vsync")) SETOPT("vsync", "off");
		else if (!strcmp(s, "--mute")) SETOPT("mute", "on");
		else if (!strcmp(s, "--overlay")) a.overlay = true;
		else if (!strcmp(s, "--hle")) a.hle = NEXT();
		else if (!strcmp(s, "--renderer")) SETOPT("renderer", NEXT());
		else if (!strcmp(s, "--filter")) SETOPT("filter", NEXT());
		else if (!strcmp(s, "--list-hle")) a.list_hle = true;
		else if (!strcmp(s, "--script")) a.script = NEXT();
		else if (!strcmp(s, "--realtime-script")) a.realtime_script = true;
		else if (!strcmp(s, "--frames")) a.frames = atol(NEXT());
		else if (!strcmp(s, "--screenshot")) a.screenshot = NEXT();
		else if (!strcmp(s, "--window-shot")) a.window_shot = NEXT();
		else if (!strcmp(s, "--outdir")) a.outdir = NEXT();
		else if (!strcmp(s, "--savedir")) a.savedir = NEXT();
		else if (!strcmp(s, "--no-throttle")) a.no_throttle = true;
		else if (!strcmp(s, "--no-mouse")) SETOPT("mouse", "off");
		else if (!strcmp(s, "--capture")) {
			const char *ad = NEXT();
			a.cap_addr = (uint32_t)strtoul(ad + (ad[0] == '$'), NULL, 16);
			a.cap_n = atoi(NEXT());
		} else if (!strcmp(s, "--capture-dir")) a.cap_dir = NEXT();
		else if (!strcmp(s, "--capture-every")) a.cap_every = atoi(NEXT());
		else if (!strcmp(s, "--capture-skip")) a.cap_skip = atoi(NEXT());
		else if (!strcmp(s, "-h") || !strcmp(s, "--help")) { usage(argv[0]); return 0; }
		else { fprintf(stderr, "unknown option %s\n", s); usage(argv[0]); return 2; }
	}
	present_filter_parse(options_str("filter"), &a.filter);
	a.scale = options_int("scale");
	a.fullscreen = options_bool("fullscreen");
	a.vsync = options_bool("vsync");
	a.mute = options_bool("mute");
	a.no_mouse = !options_bool("mouse");
	int volume = options_int("volume");
	if (options_source("renderer") == OPT_CLI)
		a.renderer = options_str("renderer");

	char savedir[1024];
	if (a.savedir)
		snprintf(savedir, sizeof savedir, "%s", a.savedir);
	else
		options_data_dir(savedir, sizeof savedir);
	slots_init(savedir);
	if (a.list_slots) {
		for (int n = 1; n <= SLOT_COUNT; n++) {
			slot_info si;
			if (slot_read(n, &si))
				printf("%d  %-9s room %-2d %-24s %s  %ld frames  %s\n", n, si.mystery ? "carnival" : "hotel", si.room,
				       si.roomname, si.date, si.playtime, si.password);
			else
				printf("%d  empty\n", n);
		}
		return 0;
	}
	if ((a.load_slot && (a.load_slot < 1 || a.load_slot > SLOT_COUNT)) || (a.save_slot && (a.save_slot < 1 || a.save_slot > SLOT_COUNT))) {
		fprintf(stderr, "slots are 1..%d\n", SLOT_COUNT);
		return 2;
	}

	/* the ROM: --rom, ini, next to the binary, ./rom/baserom.md, then ask (picker / drop window) */
	bool noninteractive = a.script || a.frames >= 0 || a.screenshot || a.window_shot || a.list_hle || a.cap_n > 0;
	char rompath[1400], err[512];
	if (!rom_locate(a.rom, options_str("rom"), argv[0], rompath, sizeof rompath)) {
		if (noninteractive || !rom_ask_user(rompath, sizeof rompath, err, sizeof err)) {
			fprintf(stderr, "Scooby-Doo Mystery ROM not found. Put the ROM (sha1 %s) next to the program,\n"
			        "or pass --rom FILE, or set rom = FILE in %s\n", SCOOBY_ROM_SHA1, cfgpath);
			return 1;
		}
	}
	const char *rom = rompath;
	mkdirs(savedir);
	mkdirs(a.outdir);

	if (!emu_init(rom, savedir, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	if (!noninteractive) {      /* remember a ROM that worked */
		char *abs = realpath(rom, NULL);
		if (abs && strcmp(abs, options_str("rom"))) {
			options_set("rom", abs, OPT_INI, err, sizeof err);
			if (!options_save(cfgpath))
				fprintf(stderr, "cannot write %s\n", cfgpath);
		}
		free(abs);
	}
	keys_configure();
	sndlog_init();   /* SCOOBY_SNDLOG=file: mailbox write log (src/sndlog.c) */
	/* before the HLE spec: the native renderer activates the "native" group (#25) */
	/* the native renderer is the player's default (#25: pixel-identical to GPGX on every gate
	 * route, all gates incl. both full walkthroughs pass with it); tools keep gpgx */
	if (!a.renderer && !getenv("SCOOBY_RENDERER"))
		a.renderer = options_str("renderer");
	if (a.renderer && !render_emu_select(a.renderer)) {
		fprintf(stderr, "unknown --renderer %s (gpgx | native)\n", a.renderer);
		return 2;
	}
	if (a.hle && !hle_configure(a.hle))
		return 2;
	ipprof_init(a.hle ? a.hle : "default");   /* SCOOBY_IPPROF=file: interpreted-instruction profile (src/ipprof.c) */
	if (a.list_hle) {
		hle_list();
		return 0;
	}
	if (a.cap_n > 0) {
		char d[1024];
		if (a.cap_dir)
			snprintf(d, sizeof d, "%s", a.cap_dir);
		else
			snprintf(d, sizeof d, "runs/states/%06X", a.cap_addr);
		capture_start(a.cap_addr, a.cap_n, d, a.cap_every, a.cap_skip);
		printf("capturing entry states of $%06X into %s\n", a.cap_addr, d);
	}

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	static video_t v;
	if (!video_init(&v, a.scale, a.fullscreen, a.vsync, a.filter))
		fprintf(stderr, "video unavailable (%s), running without a window\n", SDL_GetError());
	mouse_set_enabled(!a.no_mouse);
	if (!a.no_mouse && v.win)
		SDL_ShowCursor(SDL_DISABLE);       /* the host cursor is drawn into the game image */

	SDL_AudioDeviceID adev = 0;
	int rate = (int)(emu_sample_rate() + 0.5);
	if (!a.mute) {
		SDL_AudioSpec want = { 0 }, have;
		want.freq = rate;
		want.format = AUDIO_S16SYS;
		want.channels = 2;
		want.samples = 512;
		adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
		if (adev)
			SDL_PauseAudioDevice(adev, 0);
		else
			fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
	}
	/* queue target: ~3 frames of audio (50 ms) keeps latency low without underruns */
	const Uint32 q_target = (Uint32)(rate * 4 * 0.050);

	script *scr = NULL;
	if (a.script) {
		scr = script_open(a.script, a.outdir);
		if (!scr)
			return 1;
	}
	bool batch = a.frames >= 0 || a.screenshot || a.window_shot || a.save_slot;
	int rc = 0;
	bool post_done = false;
	long play_frames = 0;           /* emulated frames in a mystery: the saves' play time */
	char menu_status[64] = "";

	bool quit = false, paused = false, step = false, ff = false, overlay = a.overlay;
	int slot = 0;
	long frames_after_script = 0;
	char msg[128] = "";
	int msg_timer = 0;
	double fps_show = 0;
	Uint64 perf_freq = SDL_GetPerformanceFrequency(), t_start = SDL_GetPerformanceCounter();
	Uint64 t_fps = t_start, next_tick = t_start;
	long fps_frames = 0, total = 0;
	const double frame_dt = 1.0 / emu_fps();
	const bool mouse_on = !a.no_mouse;
	if (a.load_slot && !a.script) {       /* straight into a save */
		post_done = true;
		if (!slot_load_begin(a.load_slot, err, sizeof err)) {
			fprintf(stderr, "%s\n", err);
			return 1;
		}
	}

	while (!quit) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_QUIT:
				quit = true;
				break;
			case SDL_DROPFILE: {         /* a ROM dropped on the window: check it and remember it */
				char derr[400];
				if (rom_verify(e.drop.file, derr, sizeof derr)) {
					options_set("rom", e.drop.file, OPT_INI, derr, sizeof derr);
					options_save(cfgpath);
					snprintf(msg, sizeof msg, "ROM REMEMBERED");
				} else {
					snprintf(msg, sizeof msg, "NOT THE RIGHT ROM");
				}
				msg_timer = 150;
				SDL_free(e.drop.file);
				break;
			}
			case SDL_KEYDOWN: {
				SDL_Keycode k = e.key.keysym.sym;
				if (hostmenu_active()) {         /* the saves menu has the keyboard */
					int sl;
					hm_action act = hostmenu_key(k, &sl);
					g_keys = 0;
					menu_do(act, sl, play_frames, menu_status, sizeof menu_status, adev, mouse_on);
					break;
				}
				if (!e.key.repeat) {
					if (k == SDLK_F8 && !slot_loader_active()) {
						hostmenu_open();
						g_keys = 0;
						mouse_leave();
						menu_status[0] = 0;
						SDL_ShowCursor(SDL_ENABLE);
						break;
					}
					if (k == SDLK_ESCAPE)
						quit = true;
					else if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
						bool fs = SDL_GetWindowFlags(v.win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
						SDL_SetWindowFullscreen(v.win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
						break;
					} else if (k == SDLK_F1)
						overlay = !overlay;
					else if (k == SDLK_F3) {
						v.filter = (present_filter)((v.filter + 1) % 3);
						if (v.tex)
							SDL_DestroyTexture(v.tex);
						v.tex = NULL;
						snprintf(msg, sizeof msg, "FILTER %s", present_filter_name(v.filter));
						msg_timer = 90;
					}
					else if (k == SDLK_F2) {
						static bool hle_on = true;
						hle_on = !hle_on;
						hle_set_all(hle_on);
						snprintf(msg, sizeof msg, "HLE %s", hle_on ? "ON" : "OFF");
						msg_timer = 120;
					} else if (k == SDLK_F5 || k == SDLK_F9) {
						char p[1200];
						snprintf(p, sizeof p, "%s/scooby.slot%d.state", savedir, slot);
						bool ok = k == SDLK_F5 ? emu_state_save_file(p) : emu_state_load_file(p);
						snprintf(msg, sizeof msg, "%s SLOT %d%s", k == SDLK_F5 ? "SAVED" : "LOADED", slot, ok ? "" : " FAILED");
						msg_timer = 120;
						printf("%s %s%s\n", k == SDLK_F5 ? "save" : "load", p, ok ? "" : " FAILED");
						if (adev && k == SDLK_F9)
							SDL_ClearQueuedAudio(adev);
					} else if (k == SDLK_F6 || k == SDLK_F7) {
						slot = (slot + (k == SDLK_F7 ? 1 : 9)) % 10;
						snprintf(msg, sizeof msg, "SLOT %d", slot);
						msg_timer = 90;
					} else if (k == SDLK_F12) {
						char p[1200];
						snprintf(p, sizeof p, "%s/shot-%06ld.ppm", a.outdir, emu_frame_count());
						emu_write_ppm(p);
						snprintf(msg, sizeof msg, "SCREENSHOT %06ld", emu_frame_count());
						msg_timer = 90;
						printf("wrote %s\n", p);
					} else if (k == SDLK_p || k == SDLK_PAUSE)
						paused = !paused;
					else if (k == SDLK_n || k == SDLK_PERIOD) {
						paused = true;
						step = true;
					}
				}
				if (k == SDLK_TAB)
					ff = true;
				g_keys |= key_to_pad(k);
				break;
			}
			case SDL_KEYUP:
				if (e.key.keysym.sym == SDLK_TAB)
					ff = false;
				g_keys &= ~key_to_pad(e.key.keysym.sym);
				break;
			case SDL_MOUSEMOTION: {
				int mx, my;
				SDL_GetMouseState(&mx, &my);   /* window coordinates (events are rescaled) */
				if (hostmenu_active()) {
					int gx, gy, sl;
					if (window_to_game(&v, mx, my, &gx, &gy) > 0)
						hostmenu_pointer(gx, gy, 0, &sl);
					break;
				}
				mouse_from_window(&v, mx, my);
				break;
			}
			case SDL_MOUSEBUTTONDOWN: {
				int mx, my;
				SDL_GetMouseState(&mx, &my);
				if (hostmenu_active()) {
					int gx, gy, sl;
					if (window_to_game(&v, mx, my, &gx, &gy) > 0) {
						hm_action act = hostmenu_pointer(gx, gy, e.button.button == SDL_BUTTON_LEFT ? 1 : 3, &sl);
						menu_do(act, sl, play_frames, menu_status, sizeof menu_status, adev, mouse_on);
					}
					break;
				}
				mouse_from_window(&v, mx, my);
				mouse_click(e.button.button == SDL_BUTTON_RIGHT ? MOUSE_RIGHT :
				            e.button.button == SDL_BUTTON_MIDDLE ? MOUSE_MIDDLE : MOUSE_LEFT);
				break;
			}
			case SDL_MOUSEWHEEL:
				mouse_wheel(e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y);
				break;
			case SDL_WINDOWEVENT:
				if (e.window.event == SDL_WINDOWEVENT_LEAVE)
					mouse_leave();
				break;
			case SDL_CONTROLLERDEVICEADDED:
				SDL_GameControllerOpen(e.cdevice.which);
				break;
			case SDL_CONTROLLERBUTTONDOWN:
				if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
					paused = !paused;
				g_padbtn |= button_to_pad(e.cbutton.button);
				break;
			case SDL_CONTROLLERBUTTONUP:
				g_padbtn &= ~button_to_pad(e.cbutton.button);
				break;
			case SDL_CONTROLLERAXISMOTION:
				if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)
					g_axis_x = e.caxis.value;
				else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
					g_axis_y = e.caxis.value;
				break;
			}
		}
		if (quit)
			break;

		bool scripted = false;
		bool ran = false;
		const bool frozen = paused || hostmenu_active();
		if (!scr && !post_done) {            /* --save-slot / --load-slot once the script is over */
			post_done = true;
			if (a.save_slot && !slot_save_current(a.save_slot, play_frames, err, sizeof err)) {
				fprintf(stderr, "save slot %d: %s\n", a.save_slot, err);
				rc = 1;
				break;
			}
			if (a.save_slot)
				printf("saved slot %d\n", a.save_slot);
			if (a.load_slot && !slot_load_begin(a.load_slot, err, sizeof err)) {
				fprintf(stderr, "%s\n", err);
				rc = 1;
				break;
			}
		}
		if (slot_loader_active()) {          /* a game save is being loaded: press the game's own buttons */
			unsigned pad;
			if (slot_loader_step(&pad)) {
				emu_set_pad(0, mouse_frame(pad));
				scripted = true;
			} else {
				bool ok = slot_loader_result(msg, sizeof msg);
				msg_timer = 150;
				printf("%s\n", msg);
				if (ok)
					play_frames = slot_loaded_playtime();
				else
					rc = 1;
				if (adev)
					SDL_ClearQueuedAudio(adev);
			}
		} else if (scr) {
			unsigned pad;
			if (script_step(scr, &pad)) {
				emu_set_pad(0, mouse_frame(pad));
				scripted = true;
			} else {
				if (script_errors(scr))
					fprintf(stderr, "script finished with %d error(s)\n", script_errors(scr));
				script_close(scr);
				scr = NULL;
				if (batch && a.frames < 0 && !a.load_slot && !a.save_slot)
					break;
				continue;
			}
		}
		if (!scripted) {
			if (batch && frames_after_script >= a.frames && a.frames >= 0)
				break;
			/* the mouse state machine advances only on frames that are emulated */
			emu_set_pad(0, (!frozen || step) ? mouse_frame(live_pad()) : live_pad());
		}
		if (scripted || !frozen || step) {
			emu_run_frame();
			ran = true;
			step = false;
			total++;
			fps_frames++;
			if (slot_in_game())
				play_frames++;
			if (!scripted)
				frames_after_script++;
			if (capture_done()) {
				printf("captured %d states\n", capture_count());
				capture_stop();
			}
		}

		bool fast = a.no_throttle || ff || ((scripted || batch) && !a.realtime_script);
		if (slot_loader_active())
			fast = true;
		/* audio: queue this frame's samples, and pace on the queue length */
		if (ran && adev && !fast) {
			size_t n;
			const int16_t *s = emu_audio(&n);
			if (volume < 100) {
				static int16_t vbuf[4096];
				size_t m = n * 2 < 4096 ? n * 2 : 4096;
				for (size_t i = 0; i < m; i++)
					vbuf[i] = (int16_t)(s[i] * volume / 100);
				SDL_QueueAudio(adev, vbuf, (Uint32)(m * 2));
			} else {
				SDL_QueueAudio(adev, s, (Uint32)(n * 4));
			}
		} else if (adev && fast) {
			SDL_ClearQueuedAudio(adev);
		}

		/* video (skip most frames when running unthrottled) */
		int w, h;
		bool show = !fast || (total % 30) == 0 || frozen;
		if (v.ren && show && game_frame(&v, &w, &h)) {
			if (overlay) {
				char t[256];
				uint32_t sx = emu_ram32(0xFF04DC) >> 16, sy = emu_ram32(0xFF04F4) >> 16;
				snprintf(t, sizeof t, "F %ld  %.1f FPS%s%s\nHLE %lu/F  SLOT %d\nSHAGGY %u,%u\nPAD %02X  AQ %u MS\nMOUSE %s\nRENDER %s %s",
				         emu_frame_count(), fps_show, paused ? "  PAUSED" : "", ff ? "  FF" : "",
				         hle_calls_this_frame(), slot, sx, sy, emu_ram8(0xFF09E0),
				         adev ? (unsigned)(SDL_GetQueuedAudioSize(adev) * 1000 / (rate * 4)) : 0,
				         mouse_enabled() ? mouse_status() : "OFF",
				         render_emu_kind() == RENDERER_NATIVE ? "NATIVE" : "GPGX", present_filter_name(v.filter));
				overlay_text_rgba(v.buf, w, h, 2, 2, render_rgba(15, 15, 0), t);
			}
			if (msg_timer > 0) {
				overlay_text_rgba(v.buf, w, h, 2, h - 8, render_rgba(15, 15, 15), msg);
				msg_timer--;
			}
			mouse_draw_rgba(v.buf, w, h);
			hostmenu_draw(v.buf, w, h, menu_status);
			video_present(&v, v.buf, w, h);
		}

		/* throttle */
		if (!fast) {
			if (adev) {
				while (SDL_GetQueuedAudioSize(adev) > q_target)
					SDL_Delay(1);
			} else {
				next_tick += (Uint64)(frame_dt * perf_freq);
				Uint64 now = SDL_GetPerformanceCounter();
				if (next_tick > now)
					SDL_Delay((Uint32)((next_tick - now) * 1000 / perf_freq));
				else if (now - next_tick > perf_freq / 4)
					next_tick = now;
			}
		} else {
			next_tick = SDL_GetPerformanceCounter();
		}

		Uint64 now = SDL_GetPerformanceCounter();
		if (now - t_fps >= perf_freq) {
			fps_show = fps_frames * (double)perf_freq / (double)(now - t_fps);
			fps_frames = 0;
			t_fps = now;
			if (v.win) {
				char title[128];
				snprintf(title, sizeof title, "Scooby-Doo Mystery - %.1f fps%s", fps_show, paused ? " (paused)" : "");
				SDL_SetWindowTitle(v.win, title);
			}
		}
	}

	double secs = (double)(SDL_GetPerformanceCounter() - t_start) / (double)perf_freq;
	if (a.screenshot) {
		if (emu_write_ppm(a.screenshot))
			printf("wrote %s\n", a.screenshot);
		else
			fprintf(stderr, "cannot write %s\n", a.screenshot);
	}
	if (a.window_shot) {
		int w, h;
		if (v.ren && game_frame(&v, &w, &h)) {
			if (overlay) {
				char t[64];
				snprintf(t, sizeof t, "F %ld\nOVERLAY TEST 0123456789", emu_frame_count());
				overlay_text_rgba(v.buf, w, h, 2, 2, render_rgba(15, 15, 0), t);
			}
			mouse_draw_rgba(v.buf, w, h);
			video_present(&v, v.buf, w, h);
		}
		if (window_shot(&v, a.window_shot))
			printf("wrote %s\n", a.window_shot);
		else
			fprintf(stderr, "cannot write window shot: %s\n", SDL_GetError());
	}
	if (a.dump_ram && !emu_write_ram(a.dump_ram)) {
		fprintf(stderr, "cannot write %s\n", a.dump_ram);
		rc = 1;
	}
	for (int i = 0; i < hle_count(); i++)
		if (hle_calls(i))
			printf("hle %-20s $%06X calls %lu\n", hle_get(i)->name, hle_get(i)->addr, hle_calls(i));
	printf("%ld frames in %.2f s (%.1f fps)\n", total, secs, total / (secs > 0 ? secs : 1));
	if (capture_count())
		printf("captured %d entry states\n", capture_count());
	capture_stop();
	if (adev)
		SDL_CloseAudioDevice(adev);
	SDL_Quit();
	emu_shutdown();
	return rc;
}
