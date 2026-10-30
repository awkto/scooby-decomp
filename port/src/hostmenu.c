/* GAME SAVES overlay: see hostmenu.h. */
#include "hostmenu.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

#include "overlay.h"
#include "saveslot.h"

static bool g_open;
static int g_sel;          /* 0..7 */
static int g_hover = -1, g_hover_btn;

/* layout, game pixels, relative to the box (the box is centred in the frame) */
#define BOX_W   236
#define BOX_H   216
#define ROW_Y   18
#define ROW_H   22
#define BTN_Y   3
#define BTN_H   14
#define SAVE_X  (BOX_W - 96)
#define SAVE_W  28
#define LOAD_X  (BOX_W - 66)
#define LOAD_W  28
#define DEL_X   (BOX_W - 36)
#define DEL_W   26

void hostmenu_open(void) { g_open = true; }
void hostmenu_close(void) { g_open = false; }
bool hostmenu_active(void) { return g_open; }

hm_action hostmenu_key(int k, int *slot)
{
	*slot = g_sel + 1;
	switch (k) {
	case SDLK_ESCAPE: case SDLK_F8: g_open = false; break;
	case SDLK_UP: g_sel = (g_sel + SLOT_COUNT - 1) % SLOT_COUNT; break;
	case SDLK_DOWN: g_sel = (g_sel + 1) % SLOT_COUNT; break;
	case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_l: return HM_LOAD;
	case SDLK_s: return HM_SAVE;
	case SDLK_DELETE: return HM_ERASE;
	default:
		if (k >= SDLK_1 && k <= SDLK_8)
			g_sel = k - SDLK_1;
	}
	return HM_NONE;
}

static void box_origin(int w, int h, int *x, int *y)
{
	*x = (w - BOX_W) / 2;
	*y = (h - BOX_H) / 2;
}

/* which row and button (0 none/row, 1 save, 2 load, 3 del) is under the pointer */
static bool hit(int gx, int gy, int w, int h, int *row, int *btn)
{
	int bx, by;
	box_origin(w, h, &bx, &by);
	int x = gx - bx, y = gy - by - ROW_Y;
	if (x < 0 || x >= BOX_W || y < 0 || y >= ROW_H * SLOT_COUNT)
		return false;
	*row = y / ROW_H;
	int ry = y % ROW_H;
	*btn = 0;
	if (ry >= BTN_Y && ry < BTN_Y + BTN_H) {
		if (x >= SAVE_X && x < SAVE_X + SAVE_W) *btn = 1;
		else if (x >= LOAD_X && x < LOAD_X + LOAD_W) *btn = 2;
		else if (x >= DEL_X && x < DEL_X + DEL_W) *btn = 3;
	}
	return true;
}

static int g_w = 256, g_h = 224;     /* frame size of the last draw, for hit testing */

hm_action hostmenu_pointer(int gx, int gy, int button, int *slot)
{
	int row, btn;
	*slot = 0;
	if (!hit(gx, gy, g_w, g_h, &row, &btn)) {
		g_hover = -1;
		return HM_NONE;
	}
	g_hover = row;
	g_hover_btn = btn;
	if (!button)
		return HM_NONE;
	g_sel = row;
	*slot = row + 1;
	if (button == 1)
		return btn == 1 ? HM_SAVE : btn == 3 ? HM_ERASE : HM_LOAD;     /* a click on the row itself loads */
	return HM_NONE;
}

static void fill(uint32_t *buf, int w, int h, int x, int y, int rw, int rh, uint32_t c)
{
	for (int j = y; j < y + rh; j++)
		for (int i = x; i < x + rw; i++)
			if (i >= 0 && j >= 0 && i < w && j < h)
				buf[j * w + i] = c;
}

static void button(uint32_t *buf, int w, int h, int x, int y, int bw, const char *label, bool on, bool hot)
{
	fill(buf, w, h, x, y, bw, BTN_H, hot ? 0xFF886040u : on ? 0xFF604028u : 0xFF302018u);
	overlay_text_rgba(buf, w, h, x + (bw - 4 * (int)strlen(label)) / 2 + 1, y + 4, on ? 0xFFFFFFFFu : 0xFF808080u, label);
}

void hostmenu_draw(uint32_t *buf, int w, int h, const char *status)
{
	if (!g_open)
		return;
	g_w = w;
	g_h = h;
	int bx, by;
	box_origin(w, h, &bx, &by);
	fill(buf, w, h, bx, by, BOX_W, BOX_H, 0xFF201008u);
	fill(buf, w, h, bx, by, BOX_W, 1, 0xFFA07040u);
	fill(buf, w, h, bx, by + BOX_H - 1, BOX_W, 1, 0xFFA07040u);
	overlay_text_rgba(buf, w, h, bx + 6, by + 6, 0xFF40E0FFu, "GAME SAVES");
	bool ingame = slot_in_game();
	for (int i = 0; i < SLOT_COUNT; i++) {
		int ry = by + ROW_Y + i * ROW_H;
		slot_info si;
		bool used = slot_read(i + 1, &si);
		fill(buf, w, h, bx + 2, ry, BOX_W - 4, ROW_H - 2, i == g_sel ? 0xFF503018u : i == g_hover ? 0xFF382010u : 0xFF281408u);
		char l1[96], l2[64];
		if (used) {
			snprintf(l1, sizeof l1, "%d %s: %s", i + 1, si.mystery ? "CARNIVAL" : "HOTEL", si.roomname[0] ? si.roomname : "?");
			long s = si.playtime * 100 / 5992;       /* NTSC frames -> seconds */
			snprintf(l2, sizeof l2, "%s  %ld:%02ld:%02ld", si.date, s / 3600, s / 60 % 60, s % 60);
		} else {
			snprintf(l1, sizeof l1, "%d EMPTY", i + 1);
			l2[0] = 0;
		}
		l1[26] = 0;
		overlay_text_rgba(buf, w, h, bx + 6, ry + 3, used ? 0xFFFFFFFFu : 0xFF909090u, l1);
		if (l2[0])
			overlay_text_rgba(buf, w, h, bx + 6, ry + 11, 0xFFC0C0C0u, l2);
		button(buf, w, h, bx + SAVE_X, ry + BTN_Y, SAVE_W, "SAVE", ingame, i == g_hover && g_hover_btn == 1);
		button(buf, w, h, bx + LOAD_X, ry + BTN_Y, LOAD_W, "LOAD", used, i == g_hover && g_hover_btn == 2);
		button(buf, w, h, bx + DEL_X, ry + BTN_Y, DEL_W, "DEL", used, i == g_hover && g_hover_btn == 3);
	}
	int fy = by + ROW_Y + SLOT_COUNT * ROW_H + 1;
	overlay_text_rgba(buf, w, h, bx + 6, fy, 0xFFA0A0A0u, "UP/DOWN  ENTER LOAD  S SAVE  DEL ERASE");
	overlay_text_rgba(buf, w, h, bx + 6, fy + 8, 0xFFA0A0A0u, "ESC OR F8 CLOSE   OR CLICK THE BUTTONS");
	if (status && *status)
		overlay_text_rgba(buf, w, h, bx + 76, by + 6, 0xFFFFFF60u, status);
}
