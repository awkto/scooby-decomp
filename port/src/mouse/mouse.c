/* Mouse control, host side: see mouse.h and docs/mouse.md.
 *
 * The game only knows the pad.  A mouse action is turned into a short "plan" of steps
 * that each take one or more frames: open pointer mode (an A press edge, exactly what a
 * player would press), park the crosshair somewhere, let the game's hit test settle,
 * hold B on a verb cell, press B on an object, close pointer mode, or start a walk.
 * Steps check the game state every frame and give up when it is not what they expect
 * (a script started, the room changed, ...). */
#include "mouse.h"
#include "emu.h"
#include "mousehook.h"
#include "walkpath.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* pad bits (emu.h) */
#define DIRS (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)
#define SETTLE_FRAMES 3      /* pointer written -> pointer_update + main-loop hit test ran */
#define SETTLE_TIMEOUT 40    /* frames: the main loop can lag behind (scripts, Scooby) */
#define HOLD_TIMEOUT 30
#define OPEN_TIMEOUT 8
#define VERB_LOOK 10

typedef enum {
	OP_OPEN,          /* make sure pointer mode is open (inject A, wait for $FF09DC bit 3) */
	OP_CLOSE,         /* make sure it is closed (inject A, wait) */
	OP_AT,            /* park the crosshair at (x, y) instead of following the mouse */
	OP_FOLLOW,        /* crosshair follows the mouse again */
	OP_SETTLE,        /* wait until the game has hit-tested the crosshair position */
	OP_HOLD_VERB,     /* hold B until the main loop has selected verb n ($FF06C0) */
	OP_HOLD_PAGE,     /* hold B until the inventory page ($FF06EC) changes (one step) */
	OP_PRESS,         /* inject buttons `pad` for n frames */
	OP_GAP,           /* inject nothing for n frames */
	OP_DECIDE,        /* left/right click in pointer mode: look at the game state, plan more */
	OP_WALK,          /* plan and start a walk to screen (x, y) */
	OP_NEED_TARGET,   /* abort the plan unless an object is under the crosshair */
} op_t;

typedef struct {
	op_t op;
	int x, y, n, button;
	unsigned pad;
	int t;            /* frames spent in this step */
} step;

static struct {
	bool enabled;
	bool active;          /* mouse has control (moved/clicked since the last pad input) */
	bool inside;          /* over the game image */
	bool hover;           /* moved: open pointer mode for hovering when possible */
	bool opened;          /* we opened pointer mode (close it when the pad takes over) */
	int mx, my;
	step plan[24];
	int np, pos;
	bool override;        /* crosshair parked by the plan */
	int ox, oy;
	int still;            /* frames run with the same crosshair position written */
	int wx, wy;           /* position written last */
	const char *status;
} M = { .enabled = true, .status = "idle" };

/* SCOOBY_MOUSE_DEBUG=1: log decisions to stderr */
static void mlog(const char *fmt, ...)
{
	static int on = -1;
	if (on < 0)
		on = getenv("SCOOBY_MOUSE_DEBUG") != NULL;
	if (!on)
		return;
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "mouse %ld: ", emu_frame_count());
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

void mouse_set_enabled(bool on) { M.enabled = on; }
bool mouse_enabled(void) { return M.enabled; }
const char *mouse_status(void) { return M.status; }

/* ------------------------------------------------------------------ plan queue */
static void plan_clear(void)
{
	M.np = M.pos = 0;
	M.override = false;
}

static void push(op_t op, int x, int y, int n, unsigned pad)
{
	if (M.np < (int)(sizeof M.plan / sizeof M.plan[0]))
		M.plan[M.np++] = (step){ op, x, y, n, 0, pad, 0 };
}

static void push_click_step(int button, int x, int y)
{
	push(OP_DECIDE, x, y, 0, 0);
	M.plan[M.np - 1].button = button;
}

static bool busy(void) { return M.pos < M.np; }

/* ------------------------------------------------------------------ events */
void mouse_move(int gx, int gy)
{
	if (!M.enabled)
		return;
	if (gx != M.mx || gy != M.my || !M.inside)
		M.hover = true;
	M.mx = gx;
	M.my = gy;
	M.inside = true;
	M.active = true;
}

void mouse_leave(void) { M.inside = false; }

/* Exits are ordinary objects (their default verb is Look) whose walk-on rectangle
 * triggers the room change.  A left click on an object whose default verb is Look and
 * whose rectangle Shaggy can step into therefore walks into it (doors, stairs, rugs);
 * Look stays on the right button.  Returns the record index or -1. */
static int walk_into_object(int target)
{
	walk_rect r;
	int fx, fy;
	if (target < 3 || mg_object_default_verb(target) != VERB_LOOK || !walk_trigger_of_record(target - 3, &r))
		return -1;
	mg_shaggy_pos(&fx, &fy);
	return walk_rect_reachable(fx, fy, &r) ? target - 3 : -1;
}

/* left click on the room/bar while the pointer is open or can be opened */
static void plan_pointer_click(int button)
{
	if (mg_mode_get() != MG_POINTER) {
		walk_cancel();
		push(OP_OPEN, 0, 0, 0, 0);
	}
	push(OP_FOLLOW, 0, 0, 0, 0);
	push(OP_SETTLE, 0, 0, 0, 0);
	push_click_step(button, M.mx, M.my);
}

/* walk to screen (x, y); record >= 0: into that object's walk-on rectangle */
static void plan_walk(int x, int y, int record)
{
	push(OP_CLOSE, 0, 0, 0, 0);
	push(OP_WALK, x, y, record + 1, 0);
}

void mouse_click(int button)
{
	if (!M.enabled)
		return;
	M.active = true;
	if (button == MOUSE_MIDDLE) {               /* Start: pause / in-game menu */
		plan_clear();
		push(OP_PRESS, 0, 0, 2, PAD_START);
		push(OP_GAP, 0, 0, 2, 0);
		return;
	}
	if (!M.inside)
		return;
	plan_clear();
	mg_mode mode = mg_mode_get();
	mlog("button %d at (%d,%d), mode %d", button, M.mx, M.my, mode);
	if (mode == MG_NONE || mode == MG_BUSY) {
		/* dialogue, cut scene, title...: the buttons the game waits for (A or B edges
		 * advance text, B confirms a choice; Start on the title / intro screens) */
		unsigned b = button == MOUSE_RIGHT ? PAD_A : mode == MG_NONE ? (PAD_START | PAD_B) : PAD_B;
		push(OP_PRESS, 0, 0, 2, b);
		push(OP_GAP, 0, 0, 2, 0);
		return;
	}
	bool on_bar = M.my >= MG_BAR_TOP;
	if (mode == MG_FREE && !on_bar && button == MOUSE_LEFT) {
		/* pointer closed (Shaggy standing or walking): an object with a verb needs the
		 * game's pointer flow, anything else is a walk - no need to stop first */
		int t = mg_hit_test(M.mx, M.my), rec = walk_into_object(t);
		if (!t || !mg_object_default_verb(t) || rec >= 0) {
			push(OP_WALK, M.mx, M.my, rec + 1, 0);
			return;
		}
	}
	if (mode == MG_FREE && button == MOUSE_RIGHT && (on_bar || !mg_hit_test(M.mx, M.my)))
		return;                                 /* nothing to look at, nothing to cancel */
	plan_pointer_click(button);
}

void mouse_wheel(int steps)
{
	if (!M.enabled || !steps)
		return;
	M.active = true;
	mg_mode mode = mg_mode_get();
	if (busy())
		return;
	if (mode == MG_POINTER && mg_inventory_view()) {
		int x, y;
		mg_page_arrow_center(steps < 0, &x, &y);
		push(OP_AT, x, y, 0, 0);
		push(OP_SETTLE, 0, 0, 0, 0);
		push(OP_HOLD_PAGE, 0, 0, 0, 0);         /* page arrows act while B is held */
		push(OP_GAP, 0, 0, 3, 0);
		push(OP_FOLLOW, 0, 0, 0, 0);
	} else if (mode == MG_BUSY || mode == MG_NONE) {
		push(OP_PRESS, 0, 0, 2, steps > 0 ? PAD_UP : PAD_DOWN);   /* dialogue choices */
		push(OP_GAP, 0, 0, 2, 0);
	}
}

/* ------------------------------------------------------------------ decisions */
/* OP_DECIDE: pointer mode is open and the crosshair has been on (x, y) long enough for
 * the main loop to have hit-tested it */
static void decide(const step *s)
{
	int x = s->x, y = s->y;
	if (y >= MG_BAR_TOP) {
		if (s->button == MOUSE_RIGHT) {         /* right click on the bar: cancel */
			push(OP_CLOSE, 0, 0, 0, 0);
			return;
		}
		switch (mg_bar_at(x, y)) {
		case BAR_VERB:                          /* verb select: B held while on the cell */
			push(OP_AT, x, y, 0, 0);
			push(OP_HOLD_VERB, 0, 0, mg_verb_at(x, y), 0);
			push(OP_GAP, 0, 0, 1, 0);
			break;
		case BAR_TOGGLE:                        /* C slides verbs <-> inventory */
			push(OP_PRESS, 0, 0, 1, PAD_C);
			push(OP_GAP, 0, 0, 17, 0);
			break;
		case BAR_PAGE_UP:
		case BAR_PAGE_DOWN:
			push(OP_HOLD_PAGE, 0, 0, 0, 0);
			push(OP_GAP, 0, 0, 3, 0);
			break;
		case BAR_ITEM:                          /* an item is an object: B edge */
			push(OP_PRESS, 0, 0, 1, PAD_B);
			push(OP_GAP, 0, 0, 2, 0);
			break;
		case BAR_NONE:
			break;
		}
		return;
	}
	if (s->button == MOUSE_RIGHT) {
		if (!mg_target() || mg_second_object()) {
			push(OP_CLOSE, 0, 0, 0, 0);         /* nothing under it: leave pointer mode */
			return;
		}
		/* LOOK at it: select the verb on the bar like a player would, come back, click */
		int vx, vy;
		mg_verb_cell_center(VERB_LOOK, &vx, &vy);
		push(OP_AT, vx, vy, 0, 0);
		push(OP_SETTLE, 0, 0, 0, 0);
		push(OP_HOLD_VERB, 0, 0, VERB_LOOK, 0);
		push(OP_GAP, 0, 0, 1, 0);
		push(OP_AT, x, y, 0, 0);
		push(OP_SETTLE, 0, 0, 0, 0);
		push(OP_NEED_TARGET, 0, 0, 0, 0);
		push(OP_PRESS, 0, 0, 1, PAD_B);
		push(OP_GAP, 0, 0, 2, 0);
		push(OP_FOLLOW, 0, 0, 0, 0);
		return;
	}
	int target = mg_second_object() ? mg_target2() : mg_target();
	int verb = mg_verb() ? mg_verb() : mg_default_verb();
	mlog("click (%d,%d): target %d verb %d default %d second %d", x, y, target, mg_verb(),
	     mg_default_verb(), mg_second_object());
	int rec = !mg_second_object() && !mg_verb() ? walk_into_object(target) : -1;
	if (target && (verb || mg_second_object()) && rec < 0) {
		/* the game's own click ($006292): B edge -> action_execute with that verb */
		push(OP_PRESS, 0, 0, 1, PAD_B);
		push(OP_GAP, 0, 0, 2, 0);
		return;
	}
	plan_walk(x, y, rec);                       /* floor, an exit, or an object without a verb */
}

/* OP_WALK: path from Shaggy to the clicked point (or into the walk-on rectangle under
 * it, e.g. a door), then hand the legs to the game's auto-walk */
static bool start_walk(int sx, int sy, int record)
{
	int wx, wy, fx, fy;
	if (sy < MG_ROOM_TOP)
		sy = MG_ROOM_TOP;
	if (sy >= MG_BAR_TOP)
		return true;
	mg_screen_to_world(sx, sy, &wx, &wy);
	mg_shaggy_pos(&fx, &fy);
	walk_rect r, *rp = NULL;
	if (record >= 0 && walk_trigger_of_record(record, &r))
		rp = &r;
	/* a click on (or within 4 px of) a walk-on rectangle goes into it */
	for (int d = 0; d <= 4 && !rp; d += 4) {
		static const int ox[5] = { 0, 1, -1, 0, 0 }, oy[5] = { 0, 0, 0, 1, -1 };
		for (int k = 0; k < 5 && !rp; k++)
			if (walk_trigger_at(wx + d * ox[k], wy + d * oy[k], &r) >= 0)
				rp = &r;
	}
	walk_point legs[64];
	int n = walk_find_path(fx, fy, wx, wy, rp, legs, 64);
	mlog("walk from (%d,%d) to world (%d,%d)%s: %d legs", fx, fy, wx, wy, rp ? " (walk-on rect)" : "", n);
	for (int i = 0; i < n; i++)
		mlog("  leg %d -> (%d,%d)", i, legs[i].x, legs[i].y);
	if (n < 0)
		return true;
	if (n == 0)
		return true;                            /* already there */
	return walk_start(legs, n);
}

/* ------------------------------------------------------------------ per frame */
static void abort_plan(const char *why)
{
	mlog("plan aborted: %s", why);
	plan_clear();
}

/* write the crosshair RAM (parked position or the mouse) every frame; M.still counts
 * the frames that have run with the same position written (the game's hit test is valid
 * after SETTLE_FRAMES).  The written value is compared, not the RAM: on the bar the game
 * snaps the crosshair x to the cell centre by itself. */
static void pointer_sync(bool new_frame)
{
	if (mg_mode_get() != MG_POINTER) {
		M.still = 0;
		M.wx = M.wy = -1000;
		return;
	}
	int x, y;
	if (M.override) {
		x = M.ox;
		y = M.oy;
	} else if (M.inside) {
		x = M.mx;
		y = M.my;
	} else {                                    /* mouse outside the image: leave it */
		if (new_frame)
			M.still++;
		return;
	}
	mg_pointer_place(x, y);
	if (x != M.wx || y != M.wy)
		M.still = 0;
	else if (new_frame)
		M.still++;
	M.wx = x;
	M.wy = y;
}

/* has the main loop hit-tested the crosshair where we put it?  Compared with the same
 * test done in C (mg_hit_test); on the bar pointer_update maps the cell every frame. */
static bool hit_test_current(void)
{
	if (M.wy >= MG_BAR_TOP || M.wy < MG_ROOM_TOP)
		return true;
	int want = mg_hit_test(M.wx, M.wy);
	if (mg_second_object())
		return mg_target2() == (want == mg_target() ? 0 : want);
	return mg_target() == want;
}

/* run plan steps; returns the buttons to inject this frame */
static unsigned run_plan(void)
{
	unsigned inject = 0;
	for (int guard = 0; guard < 32 && busy(); guard++) {
		step *s = &M.plan[M.pos];
		mg_mode mode = mg_mode_get();
		bool done = false;
		switch (s->op) {
		case OP_OPEN:
			if (mode == MG_POINTER) {
				done = true;
				break;
			}
			if (s->t > OPEN_TIMEOUT || (mode != MG_FREE && s->t == 0)) {
				abort_plan("cannot open pointer mode");
				return inject;
			}
			if (s->t == 0) {
				M.opened = true;
				inject |= PAD_A;                    /* press edge -> verb_menu_open_check */
			}
			s->t++;
			return inject;
		case OP_CLOSE:
			if (mode != MG_POINTER) {
				done = true;
				break;
			}
			if (s->t > OPEN_TIMEOUT) {
				abort_plan("cannot close pointer mode");
				return inject;
			}
			if (s->t == 0)
				inject |= PAD_A;                    /* A edge in pointer mode: leave ($0062E8) */
			s->t++;
			return inject;
		case OP_AT:
			M.override = true;
			M.ox = s->x;
			M.oy = s->y;
			pointer_sync(false);
			done = true;
			break;
		case OP_FOLLOW:
			M.override = false;
			pointer_sync(false);
			done = true;
			break;
		case OP_SETTLE:
			if (mode != MG_POINTER) {
				abort_plan("pointer closed");
				return inject;
			}
			if ((M.still >= SETTLE_FRAMES && hit_test_current()) || M.still >= SETTLE_TIMEOUT) {
				if (M.still >= SETTLE_TIMEOUT)
					mlog("settle timeout: target %d, expected %d", mg_target(), mg_hit_test(M.wx, M.wy));
				done = true;
				break;
			}
			return inject;
		case OP_HOLD_VERB:
			if (mode != MG_POINTER || s->t > HOLD_TIMEOUT) {
				abort_plan("verb not selected");
				return inject;
			}
			if (mg_verb() == s->n) {
				done = true;
				break;
			}
			s->t++;
			return inject | PAD_B;
		case OP_HOLD_PAGE:
			if (s->t == 0)
				s->x = mg_inv_page();
			if (mode != MG_POINTER || mg_inv_page() != s->x || s->t > HOLD_TIMEOUT) {
				done = true;
				break;
			}
			s->t++;
			return inject | PAD_B;
		case OP_NEED_TARGET:
			if (mode != MG_POINTER || !mg_target()) {
				abort_plan("no target");
				return inject;
			}
			done = true;
			break;
		case OP_PRESS:
			if (s->t++ < s->n)
				return inject | s->pad;
			done = true;
			break;
		case OP_GAP:
			if (s->t++ < s->n)
				return inject;
			done = true;
			break;
		case OP_DECIDE: {
			if (mode != MG_POINTER) {
				abort_plan("pointer closed");
				return inject;
			}
			if (mg_bar_sliding())
				return inject;                      /* wait for the slide to finish */
			step d = *s;
			M.pos++;
			/* the decision's steps replace the rest of the plan */
			plan_clear();
			decide(&d);
			continue;
		}
		case OP_WALK:
			if (mode == MG_FREE) {
				start_walk(s->x, s->y, s->n - 1);
				done = true;
				break;
			}
			if (s->t++ > OPEN_TIMEOUT) {
				abort_plan("cannot walk");
				return inject;
			}
			return inject;
		}
		if (done) {
			M.pos++;
			continue;
		}
	}
	if (!busy())
		plan_clear();
	return inject;
}

unsigned mouse_frame(unsigned pad_in)
{
	if (!M.enabled)
		return pad_in;
	walk_tick();
	mg_mode mode = mg_mode_get();
	if (mode != MG_POINTER && !busy())
		M.opened = false;
	if (pad_in) {
		/* the pad takes over.  If we opened pointer mode only to hover, a direction
		 * press closes it again (A edge) so that the D-pad walks as the player expects. */
		unsigned inject = 0;
		if (M.active && M.opened && mode == MG_POINTER && (pad_in & DIRS) && !busy())
			inject = PAD_A;
		M.active = false;
		M.hover = false;
		M.opened = false;
		plan_clear();
		M.status = "pad";
		return pad_in | inject;
	}
	if (!M.active)
		return pad_in;

	pointer_sync(true);
	unsigned inject = run_plan();
	/* hover: open pointer mode so that the game shows what is under the mouse */
	if (!busy() && M.hover && M.inside && mg_mode_get() == MG_FREE && !walk_active()) {
		push(OP_OPEN, 0, 0, 0, 0);
		inject |= run_plan();
	}
	if (mg_mode_get() == MG_POINTER)
		M.hover = false;
	M.status = busy() ? "busy" : walk_active() ? "walk" : mg_mode_get() == MG_POINTER ? "pointer" : "idle";
	return pad_in | inject;
}

/* ------------------------------------------------------------------ drawing */
/* 8x11 arrow, '#' = outline (black), 'o' = fill (white) */
static const char *const arrow[] = {
	"#       ",
	"##      ",
	"#o#     ",
	"#oo#    ",
	"#ooo#   ",
	"#oooo#  ",
	"#ooooo# ",
	"#oooooo#",
	"#ooo### ",
	"#o#o#   ",
	"## #o#  ",
	"    #   ",
};

void mouse_draw(uint16_t *px, int w, int h)
{
	if (!M.enabled || !M.inside)
		return;
	/* in pointer mode the game's crosshair is centred on the mouse in the room view */
	if (mg_mode_get() == MG_POINTER && M.my < MG_BAR_TOP && M.my >= MG_ROOM_TOP && !M.override && M.active)
		return;
	for (int r = 0; r < (int)(sizeof arrow / sizeof arrow[0]); r++)
		for (int c = 0; arrow[r][c]; c++) {
			int x = M.mx + c, y = M.my + r;
			if (x < 0 || y < 0 || x >= w || y >= h || arrow[r][c] == ' ')
				continue;
			px[y * w + x] = arrow[r][c] == '#' ? 0x0000 : 0xFFFF;
		}
}

bool mouse_map_window(int wx, int wy, int out_w, int out_h, int tex_w, int tex_h, int *gx, int *gy)
{
	if (tex_w <= 0 || tex_h <= 0)
		return false;
	int s = out_w / tex_w < out_h / tex_h ? out_w / tex_w : out_h / tex_h;
	if (s < 1)
		s = 1;
	int x0 = (out_w - tex_w * s) / 2, y0 = (out_h - tex_h * s) / 2;
	int dx = wx - x0, dy = wy - y0;
	if (dx < 0 || dy < 0)
		return false;
	*gx = dx / s;
	*gy = dy / s;
	return *gx < tex_w && *gy < tex_h;
}
