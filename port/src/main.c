/* scoobyport: native SDL2 host for Scooby-Doo Mystery (Genesis Plus GX core + HLE layer).
 *
 * Keys: arrows = D-pad, Z/A = A, X/S = B, C/D = C, Enter = Start
 *       F1 debug overlay, F2 toggle all C replacements (HLE), F5 save state, F9 load state,
 *       F6/F7 previous/next state slot, P pause, N or . frame advance (pauses),
 *       Tab (hold) fast forward, F11 or Alt+Enter fullscreen, F12 screenshot, Esc quit.
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
#include "hle.h"
#include "mouse.h"
#include "overlay.h"
#include "script.h"

typedef struct {
	const char *rom, *script, *outdir, *screenshot, *window_shot, *savedir, *hle;
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
	       "  --rom PATH              ROM (default rom/baserom.md; sha1 must match)\n"
	       "  --scale N               initial window scale (default 3); integer scaling always\n"
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
	       "  --savedir DIR           savestate slots (default ~/.local/share/scoobyport)\n"
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

static unsigned key_to_pad(SDL_Keycode k)
{
	switch (k) {
	case SDLK_UP: return PAD_UP;
	case SDLK_DOWN: return PAD_DOWN;
	case SDLK_LEFT: return PAD_LEFT;
	case SDLK_RIGHT: return PAD_RIGHT;
	case SDLK_z: case SDLK_a: return PAD_A;
	case SDLK_x: case SDLK_s: return PAD_B;
	case SDLK_c: case SDLK_d: return PAD_C;
	case SDLK_RETURN: case SDLK_KP_ENTER: return PAD_START;
	}
	return 0;
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
typedef struct {
	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex;
	int tw, th;
	uint16_t buf[512 * 512];
} video_t;

static bool video_init(video_t *v, int scale, bool fullscreen, bool vsync)
{
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
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
	SDL_RenderSetIntegerScale(v->ren, SDL_TRUE);
	return true;
}

static void video_present(video_t *v, const uint16_t *px, int w, int h)
{
	if (!v->ren || !px)
		return;
	if (!v->tex || v->tw != w || v->th != h) {
		if (v->tex)
			SDL_DestroyTexture(v->tex);
		v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, w, h);
		v->tw = w;
		v->th = h;
		SDL_RenderSetLogicalSize(v->ren, w, h);   /* + integer scale = letterboxed whole multiples */
	}
	SDL_UpdateTexture(v->tex, NULL, px, w * 2);
	SDL_SetRenderDrawColor(v->ren, 0, 0, 0, 255);
	SDL_RenderClear(v->ren);
	SDL_RenderCopy(v->ren, v->tex, NULL, NULL);
	SDL_RenderPresent(v->ren);
}

static bool window_shot(video_t *v, const char *path)
{
	int w, h;
	if (!v->ren || SDL_GetRendererOutputSize(v->ren, &w, &h))
		return false;
	uint8_t *rgb = calloc((size_t)w * h, 3);
	/* re-render the last frame (the back buffer is undefined after present) */
	SDL_SetRenderDrawColor(v->ren, 0, 0, 0, 255);
	SDL_RenderClear(v->ren);
	if (v->tex)
		SDL_RenderCopy(v->ren, v->tex, NULL, NULL);
	/* read the whole window, not just the letterboxed logical viewport */
	SDL_RenderSetLogicalSize(v->ren, 0, 0);
	SDL_RenderSetViewport(v->ren, NULL);
	bool ok = SDL_RenderReadPixels(v->ren, NULL, SDL_PIXELFORMAT_RGB24, rgb, w * 3) == 0;
	SDL_RenderSetLogicalSize(v->ren, v->tw, v->th);
	FILE *f = ok ? fopen(path, "wb") : NULL;
	if (f) {
		fprintf(f, "P6\n%d %d\n255\n", w, h);
		fwrite(rgb, 1, (size_t)w * h * 3, f);
		fclose(f);
	}
	free(rgb);
	return f != NULL;
}

/* mouse position (window coordinates) -> game pixel, through the letterboxed integer
 * scaling of video_present.  Window coordinates are converted to renderer output pixels
 * first (they differ on HiDPI displays). */
static void mouse_from_window(video_t *v, int x, int y)
{
	int ww, wh, ow, oh, gx, gy;
	if (!v->win || !v->ren)
		return;
	SDL_GetWindowSize(v->win, &ww, &wh);
	if (SDL_GetRendererOutputSize(v->ren, &ow, &oh) || ww <= 0 || wh <= 0)
		return;
	int tw = v->tw ? v->tw : 256, th = v->th ? v->th : 224;
	if (mouse_map_window(x * ow / ww, y * oh / wh, ow, oh, tw, th, &gx, &gy))
		mouse_move(gx, gy);
	else
		mouse_leave();
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
	for (int i = 1; i < argc; i++) {
		const char *s = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (usage(argv[0]), exit(2), ""))
		if (!strcmp(s, "--rom")) a.rom = NEXT();
		else if (!strcmp(s, "--scale")) a.scale = atoi(NEXT());
		else if (!strcmp(s, "--fullscreen")) a.fullscreen = true;
		else if (!strcmp(s, "--no-vsync")) a.vsync = false;
		else if (!strcmp(s, "--mute")) a.mute = true;
		else if (!strcmp(s, "--overlay")) a.overlay = true;
		else if (!strcmp(s, "--hle")) a.hle = NEXT();
		else if (!strcmp(s, "--list-hle")) a.list_hle = true;
		else if (!strcmp(s, "--script")) a.script = NEXT();
		else if (!strcmp(s, "--realtime-script")) a.realtime_script = true;
		else if (!strcmp(s, "--frames")) a.frames = atol(NEXT());
		else if (!strcmp(s, "--screenshot")) a.screenshot = NEXT();
		else if (!strcmp(s, "--window-shot")) a.window_shot = NEXT();
		else if (!strcmp(s, "--outdir")) a.outdir = NEXT();
		else if (!strcmp(s, "--savedir")) a.savedir = NEXT();
		else if (!strcmp(s, "--no-throttle")) a.no_throttle = true;
		else if (!strcmp(s, "--no-mouse")) a.no_mouse = true;
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
	if (a.scale < 1)
		a.scale = 1;

	const char *rom = emu_find_rom(a.rom, argv[0]);
	if (!rom) {
		fprintf(stderr, "ROM not found: put it at rom/baserom.md or pass --rom\n");
		return 1;
	}
	char savedir[1024];
	if (a.savedir)
		snprintf(savedir, sizeof savedir, "%s", a.savedir);
	else if (getenv("XDG_DATA_HOME"))
		snprintf(savedir, sizeof savedir, "%s/scoobyport", getenv("XDG_DATA_HOME"));
	else
		snprintf(savedir, sizeof savedir, "%s/.local/share/scoobyport", getenv("HOME") ? getenv("HOME") : ".");
	mkdirs(savedir);
	mkdirs(a.outdir);

	char err[512];
	if (!emu_init(rom, savedir, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	if (a.hle && !hle_configure(a.hle))
		return 2;
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
	if (!video_init(&v, a.scale, a.fullscreen, a.vsync))
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
	bool batch = a.frames >= 0 || a.screenshot || a.window_shot;

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

	while (!quit) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_QUIT:
				quit = true;
				break;
			case SDL_KEYDOWN: {
				SDL_Keycode k = e.key.keysym.sym;
				if (!e.key.repeat) {
					if (k == SDLK_ESCAPE)
						quit = true;
					else if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
						bool fs = SDL_GetWindowFlags(v.win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
						SDL_SetWindowFullscreen(v.win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
						break;
					} else if (k == SDLK_F1)
						overlay = !overlay;
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
				mouse_from_window(&v, mx, my);
				break;
			}
			case SDL_MOUSEBUTTONDOWN: {
				int mx, my;
				SDL_GetMouseState(&mx, &my);
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
		if (scr) {
			unsigned pad;
			if (script_step(scr, &pad)) {
				emu_set_pad(0, mouse_frame(pad));
				scripted = true;
			} else {
				if (script_errors(scr))
					fprintf(stderr, "script finished with %d error(s)\n", script_errors(scr));
				script_close(scr);
				scr = NULL;
				if (batch && a.frames < 0)
					break;
			}
		}
		if (!scripted) {
			if (batch && frames_after_script >= a.frames && a.frames >= 0)
				break;
			/* the mouse state machine advances only on frames that are emulated */
			emu_set_pad(0, (!paused || step) ? mouse_frame(live_pad()) : live_pad());
		}
		if (scripted || !paused || step) {
			emu_run_frame();
			ran = true;
			step = false;
			total++;
			fps_frames++;
			if (!scripted)
				frames_after_script++;
			if (capture_done()) {
				printf("captured %d states\n", capture_count());
				capture_stop();
			}
		}

		bool fast = a.no_throttle || ff || ((scripted || batch) && !a.realtime_script);
		/* audio: queue this frame's samples, and pace on the queue length */
		if (ran && adev && !fast) {
			size_t n;
			const int16_t *s = emu_audio(&n);
			SDL_QueueAudio(adev, s, (Uint32)(n * 4));
		} else if (adev && fast) {
			SDL_ClearQueuedAudio(adev);
		}

		/* video (skip most frames when running unthrottled) */
		unsigned w, h;
		size_t pitch;
		const uint16_t *px = emu_video(&w, &h, &pitch);
		bool show = !fast || (total % 30) == 0 || paused;
		if (px && v.ren && show && w <= 512 && h <= 512) {
			memcpy(v.buf, px, (size_t)w * h * 2);
			if (overlay) {
				char t[256];
				uint32_t sx = emu_ram32(0xFF04DC) >> 16, sy = emu_ram32(0xFF04F4) >> 16;
				snprintf(t, sizeof t, "F %ld  %.1f FPS%s%s\nHLE %lu/F  SLOT %d\nSHAGGY %u,%u\nPAD %02X  AQ %u MS\nMOUSE %s",
				         emu_frame_count(), fps_show, paused ? "  PAUSED" : "", ff ? "  FF" : "",
				         hle_calls_this_frame(), slot, sx, sy, emu_ram8(0xFF09E0),
				         adev ? (unsigned)(SDL_GetQueuedAudioSize(adev) * 1000 / (rate * 4)) : 0,
				         mouse_enabled() ? mouse_status() : "OFF");
				overlay_text(v.buf, (int)w, (int)h, 2, 2, 0xFFE0, t);
			}
			if (msg_timer > 0) {
				overlay_text(v.buf, (int)w, (int)h, 2, (int)h - 8, 0xFFFF, msg);
				msg_timer--;
			}
			mouse_draw(v.buf, (int)w, (int)h);
			video_present(&v, v.buf, (int)w, (int)h);
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
		unsigned w, h;
		size_t pitch;
		const uint16_t *px = emu_video(&w, &h, &pitch);
		if (px && v.ren) {
			memcpy(v.buf, px, (size_t)w * h * 2);
			if (overlay) {
				char t[64];
				snprintf(t, sizeof t, "F %ld\nOVERLAY TEST 0123456789", emu_frame_count());
				overlay_text(v.buf, (int)w, (int)h, 2, 2, 0xFFE0, t);
			}
			video_present(&v, v.buf, (int)w, (int)h);
		}
		if (window_shot(&v, a.window_shot))
			printf("wrote %s\n", a.window_shot);
		else
			fprintf(stderr, "cannot write window shot: %s\n", SDL_GetError());
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
	return 0;
}
