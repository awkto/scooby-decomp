/* The browser host (GitLab #36, docs/web.md): the `scooby` program (native runner, no 68k core)
 * built with Emscripten, SDL2 from Emscripten's ports.
 *
 * What differs from the desktop host (port/src/main.c):
 * - No blocking loop.  The browser calls web_frame() once per display refresh
 *   (emscripten_set_main_loop with fps 0 = requestAnimationFrame).  The logic keeps its own clock:
 *   an accumulator of real time runs as many 59.92 Hz logic frames as are due (0, 1, or a few to
 *   catch up; after a long stall it restarts rather than fast-forwarding).  Every rAF presents a
 *   picture: on a display over 60 Hz the high-frame-rate path (#27, render/hifps.c) draws the
 *   in-between picture at the accumulator's phase, so 120/144/240 Hz displays get interpolated
 *   motion; at 60 Hz it presents the frame the logic just made.  No ASYNCIFY: nothing waits.
 * - The page (web/scooby.js) mounts IndexedDB at /scooby (IDBFS) before main() and puts the ROM in
 *   it (or in /tmp for a ROM served beside the page); main() gets its path as argv[1].  Saves,
 *   quick states and the options ini live in /scooby; every write asks the page to sync.
 * - Audio is SDL2's Web Audio device; the page resumes the AudioContext on the first gesture.
 *   The queue is kept near 60 ms by nudging the logic clock (as the desktop hifps path does).
 * - Esc does not quit; F11 is the page's fullscreen; the canvas follows its CSS size x
 *   devicePixelRatio (SDL_WINDOW_ALLOW_HIGHDPI).
 * - A fatal error of the native runner ("no C block at $X") is abort(): the page shows it. */
#include <SDL.h>
#include <emscripten.h>
#include <emscripten/html5.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "emu.h"
#include "hifps.h"
#include "hle.h"
#include "hostmenu.h"
#include "mouse.h"
#include "options.h"
#include "overlay.h"
#include "present.h"
#include "render_emu.h"
#include "romfind.h"
#include "saveslot.h"
#include "widescreen.h"

#define WEB_DIR "/scooby"                 /* IDBFS mount (web/scooby.js) */
#define WEB_INI WEB_DIR "/scooby.ini"
#define WEB_SAVES WEB_DIR "/saves"
#define HIFPS_STEPS 16                    /* in-between resolution: picture j of 16 nearest the phase */

/* ask the page to copy /scooby to IndexedDB (debounced there) */
static void web_sync(void)
{
	EM_ASM({ if (Module.scoobySync) Module.scoobySync(); });
}

/* the page's status line (fps, messages) */
static void web_status(const char *s)
{
	EM_ASM({ if (Module.scoobyStatus) Module.scoobyStatus(UTF8ToString($0)); }, s);
}

/* is the file at `path` the right ROM?  The page calls this before main(). 1 yes, 0 no */
EMSCRIPTEN_KEEPALIVE int web_rom_check(const char *path)
{
	char err[512];
	if (rom_verify(path, err, sizeof err))
		return 1;
	fprintf(stderr, "%s\n", err);
	return 0;
}

/* for the page and tests (tools/web_test.js): logic frames run, a work RAM word */
EMSCRIPTEN_KEEPALIVE double web_frame_count(void) { return (double)emu_frame_count(); }
EMSCRIPTEN_KEEPALIVE int web_ram16(int addr) { return emu_ram16((uint32_t)addr); }

/* ------------------------------------------------------------------ input (as main.c) */
static unsigned g_keys, g_padbtn;
static int g_axis_x, g_axis_y;

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
			if (k != SDLK_UNKNOWN && n < MAX_KEYS_PER_BUTTON)
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

/* a pad SDL has no controller mapping for: the browser's "standard" Gamepad API layout
 * (0 A/cross, 1 B/circle, 2 X/square, 3 Y/triangle, 5 RB, 8 back, 9 start, 12-15 D-pad) */
static unsigned joybutton_to_pad(Uint8 b)
{
	switch (b) {
	case 12: return PAD_UP;
	case 13: return PAD_DOWN;
	case 14: return PAD_LEFT;
	case 15: return PAD_RIGHT;
	case 2: case 3: return PAD_A;
	case 0: return PAD_B;
	case 1: case 5: return PAD_C;
	case 9: return PAD_START;
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
static struct {
	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex;
	int tw, th, gw, gh;
	present_filter filter;
	uint32_t buf[RENDER_MAX_W * RENDER_MAX_H];
	uint32_t *scaled;
	size_t scaled_cap;
} V;

/* keep the SDL window (= the canvas's CSS size; the drawing buffer is that x devicePixelRatio
 * with SDL_WINDOW_ALLOW_HIGHDPI) the size the page gave the canvas */
static void follow_canvas(void)
{
	double cw, ch;
	if (emscripten_get_element_css_size("#canvas", &cw, &ch) != EMSCRIPTEN_RESULT_SUCCESS || cw < 1 || ch < 1)
		return;
	int ww, wh;
	SDL_GetWindowSize(V.win, &ww, &wh);
	if (ww != (int)cw || wh != (int)ch)
		SDL_SetWindowSize(V.win, (int)cw, (int)ch);
	int ow, oh;
	if (!SDL_GetRendererOutputSize(V.ren, &ow, &oh))
		ws_set_window_aspect(ow, oh);      /* widescreen=auto follows the canvas */
}

static void video_present(const uint32_t *px, int w, int h)
{
	int ow, oh, r[4];
	SDL_GetRendererOutputSize(V.ren, &ow, &oh);
	int k = V.filter == PRESENT_NEAREST ? 1 : present_fit_scale(w, h, ow, oh);
	if (k > 8)
		k = 8;
	int tw = w * k, th = h * k;
	if (!V.tex || V.tw != tw || V.th != th) {
		if (V.tex)
			SDL_DestroyTexture(V.tex);
		V.tex = SDL_CreateTexture(V.ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, tw, th);
		SDL_SetTextureScaleMode(V.tex, V.filter == PRESENT_SMOOTH ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
		V.tw = tw;
		V.th = th;
	}
	V.gw = w;
	V.gh = h;
	if (k == 1) {
		SDL_UpdateTexture(V.tex, NULL, px, w * 4);
	} else {
		size_t need = (size_t)tw * th;
		if (need > V.scaled_cap) {
			free(V.scaled);
			V.scaled = malloc(need * 4);
			V.scaled_cap = need;
		}
		present_scale(px, w, h, w, k, V.filter, V.scaled);
		SDL_UpdateTexture(V.tex, NULL, V.scaled, tw * 4);
	}
	present_rect(V.filter, w, h, ow, oh, r);
	SDL_Rect dst = { r[0], r[1], r[2], r[3] };
	SDL_SetRenderDrawColor(V.ren, 0, 0, 0, 255);
	SDL_RenderClear(V.ren);
	SDL_RenderCopy(V.ren, V.tex, NULL, &dst);
	SDL_RenderPresent(V.ren);
}

/* window (CSS px) -> game pixel; 1 on the image, 0 off it, -1 unknown */
static int window_to_game(int x, int y, int *gx, int *gy)
{
	int ww, wh, ow, oh, r[4];
	SDL_GetWindowSize(V.win, &ww, &wh);
	if (SDL_GetRendererOutputSize(V.ren, &ow, &oh) || ww <= 0 || wh <= 0)
		return -1;
	int gw = V.gw ? V.gw : 256, gh = V.gh ? V.gh : 224;
	present_rect(V.filter, gw, gh, ow, oh, r);
	int px = x * ow / ww - r[0], py = y * oh / wh - r[1];
	if (px < 0 || py < 0 || px >= r[2] || py >= r[3] || r[2] <= 0 || r[3] <= 0)
		return 0;
	*gx = px * gw / r[2];
	*gy = py * gh / r[3];
	return 1;
}

static void mouse_from_window(int x, int y)
{
	int gx, gy, on = window_to_game(x, y, &gx, &gy);
	if (on == 0)
		mouse_leave();
	else if (on > 0) {
		int sx, sy;
		ws_frame_to_game(gx, gy, &sx, &sy);
		mouse_move(sx, sy);
	}
}

/* the frame to show: the native renderer's (the only one the native runner uses) */
static bool game_frame(int *w, int *h)
{
	int nw, nh;
	const uint32_t *nat = render_emu_frame(&nw, &nh);
	if (!nat)
		return false;
	for (int y = 0; y < nh; y++)
		memcpy(V.buf + (size_t)y * nw, nat + (size_t)y * RENDER_MAX_W, (size_t)nw * 4);
	*w = nw;
	*h = nh;
	return true;
}

/* ------------------------------------------------------------------ state */
static struct {
	SDL_AudioDeviceID adev;
	int rate, volume;
	double frame_ms, acc, last;       /* logic clock (ms) */
	bool started, paused, step, ff, overlay, mouse_on;
	long play_frames, ran_total;
	char msg[128], menu_status[64], ovl[256];
	int msg_timer;
	/* display refresh measurement (rAF intervals) */
	double raf_last, raf_sum;
	int raf_n, display_hz;
	bool hifps_user_off;              /* F4 / high_fps=off */
	/* fps counters */
	double t_fps;
	int n_logic, n_present;
	double fps_logic, fps_present;
} S;

static void set_msg(const char *m, int frames)
{
	snprintf(S.msg, sizeof S.msg, "%s", m);
	S.msg_timer = frames;
}

/* options that the F-keys change are kept in the ini (the page has no command line) */
static void save_option(const char *k, const char *v)
{
	char err[200];
	if (options_set(k, v, OPT_INI, err, sizeof err) && options_save(WEB_INI))
		web_sync();
}

static void hifps_apply(void)
{
	bool want = !S.hifps_user_off && S.display_hz > 65 && render_emu_kind() == RENDERER_NATIVE;
	int rate = want ? (int)(HIFPS_STEPS * emu_fps() + 0.5) : 0;
	if (rate != hifps_rate())
		hifps_set_rate(rate);
}

static void menu_do(hm_action act, int sl)
{
	char e[200];
	S.menu_status[0] = 0;
	if (!hostmenu_active())
		return;
	if (act == HM_SAVE) {
		if (slot_save_current(sl, S.play_frames, e, sizeof e)) {
			snprintf(S.menu_status, sizeof S.menu_status, "SAVED %d", sl);
			web_sync();
		} else
			snprintf(S.menu_status, sizeof S.menu_status, "%.50s", e);
	} else if (act == HM_ERASE) {
		bool ok = slot_erase(sl);
		snprintf(S.menu_status, sizeof S.menu_status, ok ? "ERASED %d" : "NOTHING TO ERASE", sl);
		if (ok)
			web_sync();
	} else if (act == HM_LOAD) {
		if (slot_load_begin(sl, e, sizeof e)) {
			hostmenu_close();
			if (S.adev)
				SDL_ClearQueuedAudio(S.adev);
		} else
			snprintf(S.menu_status, sizeof S.menu_status, "%.50s", e);
	}
}

static void handle_key(SDL_Keycode k, bool repeat)
{
	if (hostmenu_active()) {
		int sl;
		hm_action act = hostmenu_key(k, &sl);
		g_keys = 0;
		menu_do(act, sl);
		return;
	}
	if (!repeat) {
		char m[64];
		if (k == SDLK_F8 && !slot_loader_active()) {
			hostmenu_open();
			g_keys = 0;
			mouse_leave();
			S.menu_status[0] = 0;
			return;
		}
		if (k == SDLK_F1)
			S.overlay = !S.overlay;
		else if (k == SDLK_F3) {
			V.filter = (present_filter)((V.filter + 1) % 3);
			if (V.tex)
				SDL_DestroyTexture(V.tex);
			V.tex = NULL;
			snprintf(m, sizeof m, "FILTER %s", present_filter_name(V.filter));
			set_msg(m, 90);
			save_option("filter", present_filter_name(V.filter));
		} else if (k == SDLK_F5 || k == SDLK_F9) {
			bool ok = k == SDLK_F5 ? emu_state_save_file(WEB_SAVES "/scooby.quick.state")
			                       : emu_state_load_file(WEB_SAVES "/scooby.quick.state");
			snprintf(m, sizeof m, "%s%s", k == SDLK_F5 ? "STATE SAVED" : "STATE LOADED", ok ? "" : " - FAILED");
			set_msg(m, 120);
			if (ok && k == SDLK_F5)
				web_sync();
			if (ok && k == SDLK_F9) {
				hifps_reset();
				if (S.adev)
					SDL_ClearQueuedAudio(S.adev);
			}
		} else if (k == SDLK_F10) {
			ws_toggle();
			set_msg(ws_enabled() ? "WIDESCREEN ON" : "WIDESCREEN OFF", 90);
			save_option("widescreen", ws_enabled() ? "auto" : "4:3");
		} else if (k == SDLK_F4) {
			S.hifps_user_off = !S.hifps_user_off;
			hifps_apply();
			if (S.hifps_user_off)
				set_msg("HIGH FPS OFF", 90);
			else if (hifps_active()) {
				snprintf(m, sizeof m, "HIGH FPS ON (%d HZ DISPLAY)", S.display_hz);
				set_msg(m, 90);
			} else
				set_msg("HIGH FPS NEEDS A DISPLAY OVER 60 HZ", 120);
			save_option("high_fps", S.hifps_user_off ? "off" : "auto");
		} else if (k == SDLK_p || k == SDLK_PAUSE)
			S.paused = !S.paused;
		else if (k == SDLK_n || k == SDLK_PERIOD) {
			S.paused = true;
			S.step = true;
		}
	}
	if (k == SDLK_TAB)
		S.ff = true;
	g_keys |= key_to_pad(k);
}

static void poll_events(void)
{
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_KEYDOWN:
			handle_key(e.key.keysym.sym, e.key.repeat);
			break;
		case SDL_KEYUP:
			if (e.key.keysym.sym == SDLK_TAB)
				S.ff = false;
			g_keys &= ~key_to_pad(e.key.keysym.sym);
			break;
		case SDL_MOUSEMOTION:
			if (hostmenu_active()) {
				int gx, gy, sl;
				if (window_to_game(e.motion.x, e.motion.y, &gx, &gy) > 0)
					hostmenu_pointer(gx, gy, 0, &sl);
			} else
				mouse_from_window(e.motion.x, e.motion.y);
			break;
		case SDL_MOUSEBUTTONDOWN:
			if (hostmenu_active()) {
				int gx, gy, sl;
				if (window_to_game(e.button.x, e.button.y, &gx, &gy) > 0)
					menu_do(hostmenu_pointer(gx, gy, e.button.button == SDL_BUTTON_LEFT ? 1 : 3, &sl), sl);
				break;
			}
			mouse_from_window(e.button.x, e.button.y);
			mouse_click(e.button.button == SDL_BUTTON_RIGHT ? MOUSE_RIGHT :
			            e.button.button == SDL_BUTTON_MIDDLE ? MOUSE_MIDDLE : MOUSE_LEFT);
			break;
		case SDL_MOUSEWHEEL:
			mouse_wheel(e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y);
			break;
		case SDL_WINDOWEVENT:
			if (e.window.event == SDL_WINDOWEVENT_LEAVE)
				mouse_leave();
			break;
		case SDL_JOYDEVICEADDED:
			/* a mapped pad as a game controller, any other as the browser's standard layout */
			if (SDL_IsGameController(e.jdevice.which))
				SDL_GameControllerOpen(e.jdevice.which);
			else
				SDL_JoystickOpen(e.jdevice.which);
			set_msg("GAMEPAD CONNECTED", 120);
			break;
		case SDL_CONTROLLERBUTTONDOWN:
			if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
				S.paused = !S.paused;
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
		case SDL_JOYBUTTONDOWN:
		case SDL_JOYBUTTONUP:
			if (SDL_GameControllerFromInstanceID(e.jbutton.which))
				break;                   /* handled as a controller */
			if (e.type == SDL_JOYBUTTONDOWN) {
				if (e.jbutton.button == 8)
					S.paused = !S.paused;
				g_padbtn |= joybutton_to_pad(e.jbutton.button);
			} else
				g_padbtn &= ~joybutton_to_pad(e.jbutton.button);
			break;
		case SDL_JOYAXISMOTION:
			if (SDL_GameControllerFromInstanceID(e.jaxis.which))
				break;
			if (e.jaxis.axis == 0)
				g_axis_x = e.jaxis.value;
			else if (e.jaxis.axis == 1)
				g_axis_y = e.jaxis.value;
			break;
		}
	}
}

/* one logic frame (59.92 Hz); returns whether the emulator ran */
static bool logic_frame(void)
{
	bool scripted = false;
	const bool frozen = S.paused || hostmenu_active();
	if (slot_loader_active()) {          /* a game save is being loaded: the loader presses the buttons */
		unsigned pad;
		if (slot_loader_step(&pad)) {
			emu_set_pad(0, mouse_frame(pad));
			scripted = true;
		} else {
			bool ok = slot_loader_result(S.msg, sizeof S.msg);
			S.msg_timer = 150;
			if (ok)
				S.play_frames = slot_loaded_playtime();
			hifps_reset();
			if (S.adev)
				SDL_ClearQueuedAudio(S.adev);
		}
	}
	if (!scripted)
		emu_set_pad(0, (!frozen || S.step) ? mouse_frame(live_pad()) : live_pad());
	if (!(scripted || !frozen || S.step))
		return false;
	emu_run_frame();
	S.step = false;
	S.ran_total++;
	S.n_logic++;
	if (slot_in_game())
		S.play_frames++;
	return true;
}

static void queue_audio(void)
{
	size_t n;
	const int16_t *s = emu_audio(&n);
	if (!S.adev || !n)
		return;
	/* a suspended AudioContext (no gesture yet) does not drain: don't let the queue grow */
	if (SDL_GetQueuedAudioSize(S.adev) > (Uint32)(S.rate * 4 / 4))
		SDL_ClearQueuedAudio(S.adev);
	if (S.volume < 100) {
		static int16_t vbuf[4096];
		size_t m = n * 2 < 4096 ? n * 2 : 4096;
		for (size_t i = 0; i < m; i++)
			vbuf[i] = (int16_t)(s[i] * S.volume / 100);
		SDL_QueueAudio(S.adev, vbuf, (Uint32)(m * 2));
	} else
		SDL_QueueAudio(S.adev, s, (Uint32)(n * 4));
}

static void draw_overlays(int w, int h)
{
	if (S.overlay) {
		snprintf(S.ovl, sizeof S.ovl, "F %ld  %.1f LOGIC/S  %.1f PICT/S\nDISPLAY %d HZ%s\nPAD %02X  AQ %u MS\nMOUSE %s\n%s%s",
		         emu_frame_count(), S.fps_logic, S.fps_present, S.display_hz, hifps_active() ? "  HIGH FPS" : "",
		         emu_ram8(0xFF09E0), S.adev ? (unsigned)(SDL_GetQueuedAudioSize(S.adev) * 1000 / (S.rate * 4)) : 0,
		         mouse_enabled() ? mouse_status() : "OFF", present_filter_name(V.filter),
		         S.paused ? "  PAUSED" : S.ff ? "  FF" : "");
		overlay_text_rgba(V.buf, w, h, 2, 2, render_rgba(15, 15, 0), S.ovl);
	}
	if (S.msg_timer > 0)
		overlay_text_rgba(V.buf, w, h, 2, h - 8, render_rgba(15, 15, 15), S.msg);
	int mx, my, gx, gy;
	SDL_GetMouseState(&mx, &my);
	V.gw = w;
	V.gh = h;
	if (S.mouse_on && !hostmenu_active() && window_to_game(mx, my, &gx, &gy) > 0)
		mouse_draw_rgba_at(V.buf, w, h, gx, gy);
	else
		mouse_draw_rgba(V.buf, w, h);
	hostmenu_draw(V.buf, w, h, S.menu_status);
}

/* the browser's frame callback (requestAnimationFrame) */
static void web_frame(void)
{
	double now = emscripten_get_now();
	/* the display's refresh rate: median-ish of the first rAF intervals (re-measured every
	 * few seconds: the window may move to another display) */
	if (S.raf_last > 0) {
		double d = now - S.raf_last;
		if (d > 2 && d < 40) {
			S.raf_sum += d;
			if (++S.raf_n == 120) {
				int hz = (int)(1000.0 * S.raf_n / S.raf_sum + 0.5);
				if (abs(hz - S.display_hz) > 3) {
					S.display_hz = hz;
					hifps_apply();
				}
				S.raf_n = 0;
				S.raf_sum = 0;
			}
		}
	}
	S.raf_last = now;

	follow_canvas();
	poll_events();

	/* logic: as many 59.92 Hz frames as the real time since the last call allows */
	double dt = now - S.last;
	S.last = now;
	if (dt < 0 || dt > 250)              /* a hidden tab, a stall: restart the clock, don't fast-forward */
		dt = S.frame_ms;
	S.acc += S.ff ? 4 * dt : dt;
	int ran = 0;
	while (S.acc >= S.frame_ms) {
		S.acc -= S.frame_ms;
		if (ran >= (S.ff ? 8 : 4)) {     /* cannot keep up: drop the backlog */
			S.acc = 0;
			break;
		}
		if (logic_frame()) {
			ran++;
			if (S.msg_timer > 0)
				S.msg_timer--;
			if (!S.ff)
				queue_audio();
		}
	}
	/* the Web Audio clock is not this clock: keep the queue near 60 ms (1/32 frame nudges) */
	if (S.adev && ran && !S.ff) {
		Uint32 q = SDL_GetQueuedAudioSize(S.adev), fb = (Uint32)(S.rate * 4 * S.frame_ms / 1000.0);
		Uint32 target = (Uint32)(S.rate * 4 * 0.060);
		if (q > target + 2 * fb)
			S.acc -= S.frame_ms / 32;
		else if (q + fb < target)
			S.acc += S.frame_ms / 32;
	}

	/* picture: at 60 Hz the frame just made; over 60 Hz the in-between at the clock's phase
	 * (hifps shows the span R(k-1) -> R(k) during the frame after k, as on the desktop) */
	int w, h;
	bool have = false;
	const bool frozen = S.paused || hostmenu_active();
	if (hifps_active() && !frozen && !S.ff) {
		int j = (int)(S.acc / S.frame_ms * HIFPS_STEPS) + 1;
		if (j < HIFPS_STEPS) {
			bool moved;
			const uint32_t *px = hifps_frame(j, HIFPS_STEPS, &w, &h, &moved);
			if (px) {
				for (int y = 0; y < h; y++)
					memcpy(V.buf + (size_t)y * w, px + (size_t)y * RENDER_MAX_W, (size_t)w * 4);
				have = true;
			}
		}
	}
	if (!have && (ran || frozen || hifps_active() || S.mouse_on))
		have = game_frame(&w, &h);
	if (have) {
		draw_overlays(w, h);
		video_present(V.buf, w, h);
		S.n_present++;
	}

	if (now - S.t_fps >= 1000) {
		S.fps_logic = S.n_logic * 1000.0 / (now - S.t_fps);
		S.fps_present = S.n_present * 1000.0 / (now - S.t_fps);
		S.n_logic = S.n_present = 0;
		S.t_fps = now;
		char st[160];
		snprintf(st, sizeof st, "%.1f logic fps, %.1f pictures/s, display %d Hz%s%s", S.fps_logic, S.fps_present,
		         S.display_hz, hifps_active() ? " (interpolated)" : "", S.paused ? ", paused" : "");
		web_status(st);
	}
}

/* argv[1]: the ROM's path in the page's file system */
int main(int argc, char **argv)
{
	const char *rom = argc > 1 ? argv[1] : WEB_DIR "/rom/baserom.md";
	char err[512];
	mkdir(WEB_DIR, 0777);
	mkdir(WEB_SAVES, 0777);

	options_init();
	struct stat st;
	bool had_ini = stat(WEB_INI, &st) == 0;
	options_load(WEB_INI);
	if (options_source("widescreen") == OPT_DEFAULT)      /* the page's default: the window's aspect */
		options_set("widescreen", "auto", OPT_INI, err, sizeof err);
	if (!had_ini && options_save(WEB_INI))
		web_sync();
	if (!present_filter_parse(options_str("filter"), &V.filter))
		V.filter = PRESENT_NEAREST;
	S.mouse_on = options_bool("mouse");
	S.volume = options_int("volume");
	S.hifps_user_off = !strcmp(options_str("high_fps"), "off") || !strcmp(options_str("high_fps"), "0");
	if (!ws_configure(options_str("widescreen")))
		ws_configure("auto");

	slots_init(WEB_SAVES);
	if (!emu_init(rom, WEB_SAVES, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		EM_ASM({ if (Module.scoobyFatal) Module.scoobyFatal(UTF8ToString($0)); }, err);
		return 1;
	}
	keys_configure();
	if (!render_emu_select("native")) {     /* the native runner draws with the native renderer */
		fprintf(stderr, "native renderer unavailable\n");
		return 1;
	}

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	V.win = SDL_CreateWindow("Scooby-Doo Mystery", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 960, 672,
	                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	V.ren = V.win ? SDL_CreateRenderer(V.win, -1, SDL_RENDERER_ACCELERATED) : NULL;
	if (!V.ren && V.win)                     /* no WebGL: SDL's software renderer (canvas 2D) */
		V.ren = SDL_CreateRenderer(V.win, -1, SDL_RENDERER_SOFTWARE);
	if (!V.ren) {
		fprintf(stderr, "no renderer: %s\n", SDL_GetError());
		return 1;
	}
	follow_canvas();
	mouse_set_enabled(S.mouse_on);
	if (S.mouse_on)
		SDL_ShowCursor(SDL_DISABLE);         /* the game's arrow is drawn into the picture */

	S.rate = (int)(emu_sample_rate() + 0.5);
	if (!options_bool("mute")) {
		SDL_AudioSpec want = { 0 }, have;
		want.freq = S.rate;
		want.format = AUDIO_S16SYS;
		want.channels = 2;
		want.samples = 1024;
		S.adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
		if (S.adev)
			SDL_PauseAudioDevice(S.adev, 0);
		else
			fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
	}

	S.frame_ms = 1000.0 / emu_fps();
	S.last = S.t_fps = emscripten_get_now();
	S.started = true;
	EM_ASM({ if (Module.scoobyStarted) Module.scoobyStarted(); });
	emscripten_set_main_loop(web_frame, 0, 1);      /* 0: requestAnimationFrame; never returns */
	return 0;
}
