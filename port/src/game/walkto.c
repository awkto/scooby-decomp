/* Script walk-to for Shaggy and Scooby: shaggy_walk_to $005002 .. scooby_walk_start $005B54
 * (GitLab #19, left over from #16; design: docs/subsystems/walking.md section 7, port
 * status: docs/subsystems/script.md).
 *
 * A verb script (op $0E/$0F/$1B) or Scooby's idle wander asks for a walk to (D0,D1):
 *
 *   1. planning (non-blocking): straight line if it is clear, else a route over the room's
 *      two waypoints (ROOM_SPAWN_TBL + $20). Leg targets go into the actor's waypoint table,
 *      the first leg's target into ACTOR_TARGET; the per-frame auto-walk in player_update
 *      (walk.c) does the moving and pops the legs.
 *   2. a turn animation towards the first leg, then (Shaggy only, unless the script op asks
 *      not to wait) wait until the actor arrived and play the stand animation.
 *
 * The waits call actor_animate ($00B0B8) once per frame, which waits for VBlank. So the
 * code is HLE_RANGEs that stop in front of each `bsr actor_animate` (docs/porting.md,
 * "Waiting for VBlank"). shaggy_walk_to saves the verb-menu bit of VBLANK_FLAGS on the stack
 * (`move.b (vblank_flags),-(sp)`) before its first wait and restores it after the last one:
 * those ranges exit with a declared stack delta (HLE_EXIT_SP, docs/porting.md "Ranges that
 * push or pop").
 *
 * The non-blocking helpers (line test, direction, waypoint legs) are HLE_FUNCs too, so they
 * are difftested on their own; the ranges call them directly. They keep the original
 * register results because the planner and the ranges' exits expose them.
 *
 * Stack image: a range that exits below its entry SP leaves bytes that difftest compares
 * ([exit SP, entry SP)). shaggy_walk_to's byte push writes only the high byte of the stack
 * word; the low byte is whatever the planner's last `bsr` return address or `movem` push
 * left there. The planner therefore pushes the original return addresses around its
 * top-level calls (wcall), so the stack bytes are identical.
 */
#include "game.h"
#include "cc68k.h"
#include "walk.h"

#define D(n) ctx->d[n]
#define A(n) ctx->a[n]

#define ACTORS_TURN_DONE   ACTORS_ANIM_LOOPED   /* the turn animation finished */
#define WAYPOINTS(spots)   ((spots) + 0x20)     /* 2 x (x, y) words, tile units */
#define SHAGGY_ANIM_TIMER  0xFF0618             /* w: animation timer, -1 = restart */

/* ------------------------------------------------------------------ actor descriptions */
typedef struct walker walker;
typedef void (*wfn)(hle_ctx *ctx, const walker *w);
struct walker {
	int bit;                /* 0 Shaggy, 1 Scooby (bit in the ACTORS_* sets) */
	uint32_t x, y;          /* ACTOR_X/Y, 16.16 */
	uint32_t dir_x;         /* auto-walk direction x; y at +2 */
	uint32_t wp_count;      /* waypoint legs left; the table follows (x[2] y[2] dx[2] dy[2] facing[2]) */
	uint32_t ret_off;       /* Scooby's planner is Shaggy's code + $6B6 (return addresses) */
};
static const walker SHAGGY = { 0, SHAGGY_X, SHAGGY_Y, SHAGGY_DIR_X, SHAGGY_WP_COUNT, 0 };
static const walker SCOOBY = { 1, ACTOR_X(1), ACTOR_Y(1), SCOOBY_DIR_X, SCOOBY_WP_COUNT, 0x6B6 };

/* Cost model (docs/porting.md "Long-running functions"): the C code adds the original's
 * cycles of each piece it runs to g_cost (measured with difftest: a line-test step is
 * ~440 cycles, set_dir ~1000, ...); every HLE entry charges the sum. */
static long g_cost;
#define CHARGE(stmt) do { g_cost = 0; stmt; ctx->cycles = (int)g_cost; } while (0)

/* bit helpers on a RAM byte: btst/bset/bclr #n (Z = !old bit) */
static bool rbit(hle_ctx *ctx, uint32_t a, int n) { cc_btst(ctx, RAM8(a), n); return (RAM8(a) >> n) & 1; }
static void rbset(hle_ctx *ctx, uint32_t a, int n) { rbit(ctx, a, n); RAM8(a) |= (uint8_t)(1u << n); }
static void rbclr(hle_ctx *ctx, uint32_t a, int n) { rbit(ctx, a, n); RAM8(a) &= (uint8_t)~(1u << n); }
/* move.w #v,(a) / move.w Dn,(a): store a word, N Z from it */
static void stw(hle_ctx *ctx, uint32_t a, uint32_t v) { RAM16(a) = (uint16_t)v; cc_logic16(ctx, v); }

/* bsr from the planner to a C subroutine, with the original's return address on the stack
 * (Shaggy's address; Scooby's is + w->ret_off). */
static void wcall(hle_ctx *ctx, const walker *w, uint32_t ret, wfn fn)
{
	push32(ctx, ret + w->ret_off);
	fn(ctx, w);
	ctx->a[7] += 4;
}

/* ------------------------------------------------------------------ line test */
/* $005378 walk_line_clear: is the straight line from LINE_FROM (16.16) to (D0,D1) free of
 * blocked pixels? Steps LINE_PROBE one pixel along the major axis (the minor axis by a
 * 16.16 fraction) until both coordinates reached the target, testing each point with
 * walk_blocked_line_probe. The end point is not tested.
 * out: D3 = 0 (Z=1) clear, 1 (Z=0) blocked. D0/D1/D7 kept, D2 low word kept (upper word =
 *      x step), D4/D5/A0 as the last probe left them.
 * Quirk: on an exact diagonal (|dx| == |dy|) `divu` overflows and the y step becomes 0,
 * so the probe runs along x only. */
static void line_clear(hle_ctx *ctx)
{
	uint16_t keep_d2 = (uint16_t)D(2);
	set_w(&D(2), RAM16(LINE_FROM_X));
	set_w(&D(3), RAM16(LINE_FROM_Y));
	set_w(&D(2), cc_sub(ctx, D(2), D(0), 16, true));
	set_w(&D(3), cc_sub(ctx, D(3), D(1), 16, true));
	set_w(&D(2), neg16(ctx, D(2)));                      /* dx = target - from */
	set_w(&D(3), neg16(ctx, D(3)));
	int16_t dx = (int16_t)D(2), dy = (int16_t)D(3);
	set_w(&D(4), (uint16_t)dx);
	set_w(&D(5), (uint16_t)dy);
	if (dx < 0)
		set_w(&D(4), neg16(ctx, D(4)));                  /* |dx| */
	if (dy < 0)
		set_w(&D(5), neg16(ctx, D(5)));                  /* |dy| */
	if (dx == 0) {                                       /* vertical */
		D(2) = 0;
		D(3) = dy < 0 ? 0xFFFF0000u : 0x10000u;
	} else if (dy == 0) {                                /* horizontal */
		D(3) = 0;
		D(2) = dx < 0 ? 0xFFFF0000u : 0x10000u;
	} else {
		if ((int16_t)D(5) <= (int16_t)D(4)) {            /* x major: y step = |dy|/|dx| */
			D(5) = lsll(ctx, lsll(ctx, D(5), 8), 8);
			divu16(ctx, &D(5), D(4));
			D(5) &= 0xFFFF;
			D(4) = 0x10000;
		} else {                                         /* y major */
			D(4) = lsll(ctx, lsll(ctx, D(4), 8), 8);
			divu16(ctx, &D(4), D(5));
			D(5) = 0x10000;
			D(4) &= 0xFFFF;
		}
		if (dx < 0)
			D(4) = neg32(ctx, D(4));
		if (dy < 0)
			D(5) = neg32(ctx, D(5));
		D(2) = D(4);
		D(3) = D(5);
	}
	RAM32_W(LINE_PROBE_X, RAM32(LINE_FROM_X));
	RAM32_W(LINE_PROBE_Y, RAM32(LINE_FROM_Y));
	g_cost += 250;
	for (;;) {
		g_cost += 440;
		uint32_t s0 = D(0), s1 = D(1), s7 = D(7);        /* movem.l d0-d1/d7 around the probe */
		walk_blocked_line_probe(ctx);
		bool blocked = !(ctx->sr & CC_Z);
		D(0) = s0, D(1) = s1, D(7) = s7;
		if (blocked) {
			set_w(&D(2), keep_d2);
			D(3) = 1;
			cc_logic32(ctx, 1);
			return;
		}
		RAM32_W(LINE_PROBE_X, cc_add(ctx, RAM32(LINE_PROBE_X), D(2), 32));
		RAM32_W(LINE_PROBE_Y, cc_add(ctx, RAM32(LINE_PROBE_Y), D(3), 32));
		int16_t px = (int16_t)RAM16(LINE_PROBE_X), py = (int16_t)RAM16(LINE_PROBE_Y);
		if ((int32_t)D(2) >= 0 ? (int16_t)D(0) > px : (int16_t)D(0) < px)
			continue;                                    /* x not reached yet */
		if ((int32_t)D(3) >= 0 ? (int16_t)D(1) > py : (int16_t)D(1) < py)
			continue;
		set_w(&D(2), keep_d2);
		D(3) = 0;
		cc_logic32(ctx, 0);
		return;
	}
}
HLE_FUNC(0x005378, walk_line_clear, 0) { CHARGE(line_clear(ctx)); }

/* $005364 shaggy_line_clear / $0059B0 scooby_line_clear: line from the actor to (D0,D1) */
static void actor_line_clear(hle_ctx *ctx, const walker *w)
{
	RAM32_W(LINE_FROM_X, RAM32(w->x));
	RAM32_W(LINE_FROM_Y, RAM32(w->y));
	cc_logic32(ctx, RAM32(w->y));
	line_clear(ctx);
}
HLE_FUNC(0x005364, shaggy_line_clear, 0) { CHARGE(actor_line_clear(ctx, &SHAGGY)); }
HLE_FUNC(0x0059B0, scooby_line_clear, 0) { CHARGE(actor_line_clear(ctx, &SCOOBY)); }

/* line start at waypoint `wp` (0/1), in pixels. A0 = the waypoint table. */
static void from_waypoint(hle_ctx *ctx, int wp)
{
	g_cost += 100;
	A(0) = WAYPOINTS(RAM32(ROOM_SPAWN_TBL));
	set_w(&D(3), bus_r16(A(0) + 4 * wp));
	set_w(&D(4), bus_r16(A(0) + 4 * wp + 2));
	set_w(&D(3), lslw(ctx, D(3), 3));
	set_w(&D(4), lslw(ctx, D(4), 3));
	RAM16(LINE_FROM_X) = (uint16_t)D(3);
	RAM16(LINE_FROM_Y) = (uint16_t)D(4);
	stw(ctx, LINE_FROM_X + 2, 0);
	stw(ctx, LINE_FROM_Y + 2, 0);
}
/* $0052C0 / $0052F4: is the line from waypoint 0 / 1 to (D0,D1) clear? (shared by both actors) */
static void wp0_line_clear(hle_ctx *ctx, const walker *w) { (void)w; from_waypoint(ctx, 0); line_clear(ctx); }
static void wp1_line_clear(hle_ctx *ctx, const walker *w) { (void)w; from_waypoint(ctx, 1); line_clear(ctx); }
HLE_FUNC(0x0052C0, waypoint0_line_clear, 0) { CHARGE(wp0_line_clear(ctx, 0)); }
HLE_FUNC(0x0052F4, waypoint1_line_clear, 0) { CHARGE(wp1_line_clear(ctx, 0)); }

/* ------------------------------------------------------------------ direction */
/* $00548E shaggy_set_dir / $0059DC scooby_set_dir: auto-walk direction from LINE_FROM to
 * (D0,D1) at speed D2 (pixels per frame). The major axis gets $100 (8.8), the minor one
 * the ratio; y counts double (the floor is seen at an angle). Both are then multiplied by
 * the speed into the actor's DIR_X/DIR_Y.
 * out: D6 = facing (0 right, 1 left, 2 up, 3 down), D4/D5 = dir x/y (32-bit products),
 *      D3 = scratch, D0-D2/D7 kept. */
static void set_dir(hle_ctx *ctx, const walker *w)
{
	uint32_t dir_x = w->dir_x, dir_y = w->dir_x + 2;
	g_cost += 1000;
	uint32_t s0 = D(0), s1 = D(1), s2 = D(2), s7 = D(7);
	set_w(&D(0), cc_sub(ctx, D(0), RAM16(LINE_FROM_X), 16, true));
	set_w(&D(1), cc_sub(ctx, D(1), RAM16(LINE_FROM_Y), 16, true));
	set_w(&D(1), lslw(ctx, D(1), 1));
	int16_t dx = (int16_t)D(0), dy = (int16_t)D(1);
	set_w(&D(4), (uint16_t)dx);
	if (dx < 0)
		set_w(&D(4), neg16(ctx, D(4)));
	set_w(&D(5), (uint16_t)dy);
	if (dy < 0)
		set_w(&D(5), neg16(ctx, D(5)));
	if ((int16_t)D(5) < (int16_t)D(4))
		D(6) = dx < 0 ? 1 : 0;                           /* mostly horizontal: left / right */
	else
		D(6) = dy < 0 ? 2 : 3;                           /* up / down */
	set_w(&D(2), lslw(ctx, D(2), 8));                    /* speed, 8.8 */
	set_w(&D(3), D(2));
	D(4) &= 0xFFFF;
	D(5) &= 0xFFFF;
	D(4) = lsll(ctx, D(4), 8);
	D(5) = lsll(ctx, D(5), 8);
	uint32_t ax = D(4), ay = D(5);                       /* move.l d4/d5,-(sp) */
	if (ay == 0) {                                       /* horizontal */
		RAM16(dir_y) = 0;
		set_w(&D(2), 0x100);
		if (dx < 0)
			set_w(&D(2), neg16(ctx, D(2)));
		RAM16(dir_x) = (uint16_t)D(2);
	} else if (ax == 0) {                                /* vertical */
		RAM16(dir_x) = 0;
		set_w(&D(3), 0x100);
		if (dy < 0)
			set_w(&D(3), neg16(ctx, D(3)));
		RAM16(dir_y) = (uint16_t)D(3);
	} else {
		divu16(ctx, &D(4), D(2));                        /* frames along x */
		divu16(ctx, &D(5), D(3));                        /* frames along y */
		if ((int16_t)D(5) > (int16_t)D(4)) {             /* y takes longer: y full speed */
			D(2) = ax;
			divu16(ctx, &D(2), D(5));
			set_w(&D(3), 0x100);
		} else {                                         /* x full speed */
			D(3) = ay;
			divu16(ctx, &D(3), D(4));
			set_w(&D(2), 0x100);
		}
		if (dx < 0)
			set_w(&D(2), neg16(ctx, D(2)));
		if (dy < 0)
			set_w(&D(3), neg16(ctx, D(3)));
		RAM16(dir_x) = (uint16_t)D(2);
		RAM16(dir_y) = (uint16_t)D(3);
	}
	set_w(&D(4), RAM16(dir_x));
	set_w(&D(5), RAM16(dir_y));
	D(0) = s0, D(1) = s1, D(2) = s2, D(7) = s7;
	D(4) = muls16(ctx, D(4), D(2));
	D(5) = muls16(ctx, D(5), D(2));
	RAM16(dir_x) = (uint16_t)D(4);
	stw(ctx, dir_y, D(5));
}
HLE_FUNC(0x00548E, shaggy_set_dir, 0) { CHARGE(set_dir(ctx, &SHAGGY)); }
HLE_FUNC(0x0059DC, scooby_set_dir, 0) { CHARGE(set_dir(ctx, &SCOOBY)); }

/* $00547A shaggy_dir_to / $0059C8 scooby_dir_to: set_dir from the actor's position */
static void dir_to(hle_ctx *ctx, const walker *w)
{
	RAM32_W(LINE_FROM_X, RAM32(w->x));
	RAM32_W(LINE_FROM_Y, RAM32(w->y));
	g_cost += 70;
	set_dir(ctx, w);
}
HLE_FUNC(0x00547A, shaggy_dir_to, 0) { CHARGE(dir_to(ctx, &SHAGGY)); }
HLE_FUNC(0x0059C8, scooby_dir_to, 0) { CHARGE(dir_to(ctx, &SCOOBY)); }

/* Waypoint leg setup ($00519C/$0051E4/$00522E/$005276 Shaggy, $005852/$00589A/$0058E4/$00592C
 * Scooby): direction from waypoint `wp` to (D0,D1) into leg `slot` of the waypoint table
 * (dir x at +8, dir y at +12, facing at +16). A0 = the room's waypoint table. */
static void leg_setup(hle_ctx *ctx, const walker *w, int wp, int slot)
{
	g_cost += 150;
	A(0) = WAYPOINTS(RAM32(ROOM_SPAWN_TBL));
	set_w(&D(3), lslw(ctx, bus_r16(A(0) + 4 * wp), 3));
	RAM16(LINE_FROM_X) = (uint16_t)D(3);
	stw(ctx, LINE_FROM_X + 2, 0);
	set_w(&D(3), lslw(ctx, bus_r16(A(0) + 4 * wp + 2), 3));
	RAM16(LINE_FROM_Y) = (uint16_t)D(3);
	stw(ctx, LINE_FROM_Y + 2, 0);
	set_dir(ctx, w);
	uint32_t t = w->wp_count + 2 + 2 * slot;
	RAM16(t + 8) = (uint16_t)D(4);
	RAM16(t + 12) = (uint16_t)D(5);
	stw(ctx, t + 16, D(6));
}
static void leg0_wp0(hle_ctx *ctx, const walker *w) { leg_setup(ctx, w, 0, 0); }
static void leg0_wp1(hle_ctx *ctx, const walker *w) { leg_setup(ctx, w, 1, 0); }
static void leg1_wp0(hle_ctx *ctx, const walker *w) { leg_setup(ctx, w, 0, 1); }
static void leg1_wp1(hle_ctx *ctx, const walker *w) { leg_setup(ctx, w, 1, 1); }
HLE_FUNC(0x00519C, shaggy_leg0_from_wp0, 0) { CHARGE(leg0_wp0(ctx, &SHAGGY)); }
/* (the planner range calls the same C; w0_full / w1_part give states for $51E4 and $5276, $522E is
 * tested by --entry from those) */
HLE_FUNC(0x0051E4, shaggy_leg0_from_wp1, 0) { CHARGE(leg0_wp1(ctx, &SHAGGY)); }
HLE_FUNC(0x00522E, shaggy_leg1_from_wp0, 0) { CHARGE(leg1_wp0(ctx, &SHAGGY)); }
HLE_FUNC(0x005276, shaggy_leg1_from_wp1, 0) { CHARGE(leg1_wp1(ctx, &SHAGGY)); }
HLE_FUNC(0x005852, scooby_leg0_from_wp0, 0) { CHARGE(leg0_wp0(ctx, &SCOOBY)); }
HLE_FUNC(0x00589A, scooby_leg0_from_wp1, 0) { CHARGE(leg0_wp1(ctx, &SCOOBY)); }
HLE_FUNC(0x0058E4, scooby_leg1_from_wp0, 0) { CHARGE(leg1_wp0(ctx, &SCOOBY)); }
HLE_FUNC(0x00592C, scooby_leg1_from_wp1, 0) { CHARGE(leg1_wp1(ctx, &SCOOBY)); }

/* $00532A shaggy_waypoints_reachable / $005976 scooby_waypoints_reachable: D0 bit n set if
 * waypoint n can be reached from the actor in a straight line. D1 = 2, D2.w = $FFFF,
 * A0 = waypoint table + 8. */
static void waypoints_reachable(hle_ctx *ctx, const walker *w)
{
	g_cost += 100;
	D(0) = 0;
	D(1) = 0;
	D(2) = 1;
	A(0) = WAYPOINTS(RAM32(ROOM_SPAWN_TBL));
	do {
		uint32_t s0 = D(0), s1 = D(1), sa0 = A(0);       /* movem.l d0-d1/a0 */
		g_cost += 150;
		set_w(&D(0), lslw(ctx, bus_r16(A(0)), 3));
		set_w(&D(1), lslw(ctx, bus_r16(A(0) + 2), 3));
		actor_line_clear(ctx, w);
		bool clear = ctx->sr & CC_Z;
		D(0) = s0, D(1) = s1, A(0) = sa0;
		if (clear) {
			cc_btst(ctx, D(0), D(1) & 31);
			D(0) |= 1u << (D(1) & 31);
		}
		cc_logic32(ctx, bus_r32(A(0)));                  /* tst.l (a0)+ */
		A(0) += 4;
		set_w(&D(1), cc_add(ctx, D(1), 1, 16));
		set_w(&D(2), D(2) - 1);                          /* dbra */
	} while ((uint16_t)D(2) != 0xFFFF);
}
HLE_FUNC(0x00532A, shaggy_waypoints_reachable, 0) { CHARGE(waypoints_reachable(ctx, &SHAGGY)); }
HLE_FUNC(0x005976, scooby_waypoints_reachable, 0) { CHARGE(waypoints_reachable(ctx, &SCOOBY)); }

/* ------------------------------------------------------------------ planner */
/* The route choice of shaggy_walk_to ($00501C-$005190) / scooby_walk_to ($0056D2-$005846).
 * in: D0,D1 target (pixels), D2 speed, D7 animation base. Leaves D0,D1 = first leg target
 * and the waypoint count / table set. */
static void walk_plan(hle_ctx *ctx, const walker *w)
{
	const uint32_t wpt = w->wp_count + 2;              /* waypoint table */
	g_cost += 150;
	if (rbit(ctx, GAME_STATE, 4))                      /* story flag: never use waypoints */
		goto direct;
	wcall(ctx, w, 0x00502C, actor_line_clear);
	if (ctx->sr & CC_Z)                                /* straight line is clear */
		goto direct;
	{
		/* movem.l d0-d2/d7,-(sp) around the reachability test (pushed for real: the
		 * stack bytes are compared, see the top of the file) */
		uint32_t s0 = D(0), s1 = D(1), s2 = D(2), s7 = D(7);
		push32(ctx, s7), push32(ctx, s2), push32(ctx, s1), push32(ctx, s0);
		wcall(ctx, w, 0x005038, waypoints_reachable);
		uint16_t mask = (uint16_t)D(0);
		cc_logic16(ctx, mask);
		ctx->a[7] += 16;
		D(0) = s0, D(1) = s1, D(2) = s2, D(7) = s7;
		if (mask == 0)
			goto direct;                               /* no waypoint reachable: walk anyway */
		if (mask == 3) {
			/* both: take the waypoint nearer to the room origin (|W|^2 in tiles) */
			A(0) = WAYPOINTS(RAM32(ROOM_SPAWN_TBL));
			set_w(&D(3), bus_r16(A(0)));
			set_w(&D(4), bus_r16(A(0) + 2));
			set_w(&D(5), bus_r16(A(0) + 4));
			set_w(&D(6), bus_r16(A(0) + 6));
			D(3) = muls16(ctx, D(3), D(3));
			D(4) = muls16(ctx, D(4), D(4));
			D(5) = muls16(ctx, D(5), D(5));
			D(6) = muls16(ctx, D(6), D(6));
			D(3) = cc_add(ctx, D(3), D(4), 32);
			D(5) = cc_add(ctx, D(5), D(6), 32);
			cc_sub(ctx, D(3), D(5), 32, false);
			if ((int32_t)D(3) >= (int32_t)D(5))
				goto via_wp0_check;
			wcall(ctx, w, 0x00507E, wp1_line_clear);
			if (!(ctx->sr & CC_Z))
				goto direct;
			goto via_wp1;
		}
		if (mask == 1) {                               /* only waypoint 0 */
			wcall(ctx, w, 0x0050FA, wp0_line_clear);
			if (ctx->sr & CC_Z)
				goto via_wp0;
			wcall(ctx, w, 0x005100, wp1_line_clear);
			if (!(ctx->sr & CC_Z))
				goto direct;
			/* actor -> wp0 -> wp1 -> target */
			RAM16(wpt) = (uint16_t)D(0);
			RAM16(wpt + 4) = (uint16_t)D(1);
			wcall(ctx, w, 0x005114, leg0_wp1);
			set_w(&D(0), lslw(ctx, bus_r16(A(0) + 4), 3));
			set_w(&D(1), lslw(ctx, bus_r16(A(0) + 6), 3));
			RAM16(wpt + 2) = (uint16_t)D(0);
			RAM16(wpt + 6) = (uint16_t)D(1);
			wcall(ctx, w, 0x005130, leg1_wp0);
			set_w(&D(0), lslw(ctx, bus_r16(A(0)), 3));
			set_w(&D(1), lslw(ctx, bus_r16(A(0) + 2), 3));
			stw(ctx, w->wp_count, 2);
			return;
		}
		/* only waypoint 1 */
		wcall(ctx, w, 0x00514E, wp1_line_clear);
		if (ctx->sr & CC_Z)
			goto via_wp1;
		wcall(ctx, w, 0x005156, wp0_line_clear);
		if (!(ctx->sr & CC_Z))
			goto direct;
		/* actor -> wp1 -> wp0 -> target */
		RAM16(wpt) = (uint16_t)D(0);
		RAM16(wpt + 4) = (uint16_t)D(1);
		wcall(ctx, w, 0x00516A, leg0_wp0);
		set_w(&D(0), lslw(ctx, bus_r16(A(0)), 3));
		set_w(&D(1), lslw(ctx, bus_r16(A(0) + 2), 3));
		RAM16(wpt + 2) = (uint16_t)D(0);
		RAM16(wpt + 6) = (uint16_t)D(1);
		wcall(ctx, w, 0x005184, leg1_wp1);
		set_w(&D(0), lslw(ctx, bus_r16(A(0) + 4), 3));
		set_w(&D(1), lslw(ctx, bus_r16(A(0) + 6), 3));
		stw(ctx, w->wp_count, 2);
		return;
	}
via_wp0_check:
	wcall(ctx, w, 0x0050B8, wp0_line_clear);
	if (!(ctx->sr & CC_Z))
		goto direct;
via_wp0:                                               /* actor -> wp0 -> target */
	RAM16(wpt) = (uint16_t)D(0);
	RAM16(wpt + 4) = (uint16_t)D(1);
	wcall(ctx, w, 0x0050CC, leg0_wp0);
	A(0) = WAYPOINTS(RAM32(ROOM_SPAWN_TBL));
	set_w(&D(0), lslw(ctx, bus_r16(A(0)), 3));
	set_w(&D(1), lslw(ctx, bus_r16(A(0) + 2), 3));
	stw(ctx, w->wp_count, 1);
	return;
via_wp1:                                               /* actor -> wp1 -> target */
	RAM16(wpt) = (uint16_t)D(0);
	RAM16(wpt + 4) = (uint16_t)D(1);
	wcall(ctx, w, 0x005092, leg0_wp1);
	A(0) = WAYPOINTS(RAM32(ROOM_SPAWN_TBL));
	set_w(&D(0), lslw(ctx, bus_r16(A(0) + 4), 3));
	set_w(&D(1), lslw(ctx, bus_r16(A(0) + 6), 3));
	stw(ctx, w->wp_count, 1);
	return;
direct:
	stw(ctx, w->wp_count, 0);
}

/* Start of the walk (both actors): target, direction, turn animation towards it.
 * D7 = 0: the turn animation from the actor's turn table [facing][new facing], else
 * D7 + new facing. */
static void walk_start(hle_ctx *ctx, const walker *w, uint32_t turn_table, uint32_t anim_timer)
{
	g_cost += 250;
	RAM16(ACTOR_TARGET_X(w->bit)) = (uint16_t)D(0);
	stw(ctx, ACTOR_TARGET_Y(w->bit), D(1));
	uint32_t facing = w->bit ? SCOOBY_FACING : SHAGGY_FACING;
	if (w->bit == 0) {
		/* move.b (vblank_flags),-(sp): keep the verb-menu bit (restored by walk_finish);
		 * the byte goes into the high half of the stack word */
		ctx->a[7] -= 2;
		bus_w8(ctx->a[7], RAM8(VBLANK_FLAGS));
		cc_logic8(ctx, RAM8(VBLANK_FLAGS));
		rbclr(ctx, VBLANK_FLAGS, 3);
	}
	push32(ctx, w->bit ? 0x005AF0 : 0x0055B0);         /* bsr shaggy_dir_to / scooby_dir_to */
	dir_to(ctx, w);
	ctx->a[7] += 4;
	cc_sub(ctx, D(7), 0, 16, false);                   /* cmpi.w #0,d7 */
	if ((uint16_t)D(7) == 0) {
		set_w(&D(0), RAM16(facing));
		set_w(&D(0), lslw(ctx, D(0), 3));
		set_w(&D(0), cc_add(ctx, D(0), D(6), 16));
		set_w(&D(0), cc_add(ctx, D(0), D(6), 16));
		A(0) = turn_table;
		stw(ctx, ACTOR_ANIM(w->bit), ROM16(turn_table + (int16_t)D(0)));
	} else {
		set_w(&D(7), cc_add(ctx, D(7), D(6), 16));
		stw(ctx, ACTOR_ANIM(w->bit), D(7));
	}
	stw(ctx, facing, D(6));
	stw(ctx, anim_timer, 0xFFFF);
	rbset(ctx, ACTORS_ANIM_NEW, w->bit);
}

/* ------------------------------------------------------------------ Shaggy */
#define SW_RTS_EARLY  0x00501A   /* rts: already there */
#define SW_ANIM_TURN  0x0055F0   /* bsr actor_animate: turn animation */
#define SW_ANIM_WALK  0x00561A   /* bsr actor_animate: walking */
#define SW_ANIM_STAND 0x005644   /* bsr actor_animate: stand animation */
#define SW_RTS        0x005668   /* rts after the flag byte was popped */

/* $005002 shaggy_walk_to: in D0,D1 target pixel, D2 speed, D7 animation base (0 = turn
 * table), A5 = the script op (word 2 < 0: don't wait for the arrival). */
HLE_RANGE(0x005002, shaggy_walk_to, 60, SW_RTS_EARLY, HLE_EXIT_SP(SW_ANIM_TURN, -2))
{
	rbclr(ctx, ACTORS_MOVING, 0);
	if (cmpw(ctx, D(0), RAM16(SHAGGY_X)) == 0 && cmpw(ctx, D(1), RAM16(SHAGGY_Y)) == 0) {
		hle_jump(ctx, SW_RTS_EARLY);
		return;
	}
	CHARGE((walk_plan(ctx, &SHAGGY), walk_start(ctx, &SHAGGY, SHAGGY_TURN_TABLE, SHAGGY_ANIM_TIMER)));
	hle_jump(ctx, SW_ANIM_TURN);
}

/* pop the saved VBLANK_FLAGS byte and restore its verb-menu bit 3 */
static void shaggy_walk_finish(hle_ctx *ctx)
{
	uint8_t saved = bus_r8(ctx->a[7]);
	ctx->a[7] += 2;
	set_b(&D(0), saved & 0x08);
	RAM8(VBLANK_FLAGS) = (uint8_t)((RAM8(VBLANK_FLAGS) & 0xF7) | (saved & 0x08));
	cc_logic8(ctx, RAM8(VBLANK_FLAGS));
	hle_jump(ctx, SW_RTS);
}

/* $0055F4: turn animation running? then start moving and (unless the op says not to wait)
 * wait for the arrival */
HLE_RANGE(0x0055F4, shaggy_walk_turned, 63, SW_ANIM_TURN, SW_ANIM_WALK, HLE_EXIT_SP(SW_RTS, 2))
{
	if (rbit(ctx, ACTORS_ANIM_NEW, 0) || !rbit(ctx, ACTORS_TURN_DONE, 0)) {
		hle_jump(ctx, SW_ANIM_TURN);
		return;
	}
	rbset(ctx, ACTORS_MOVING, 0);
	uint16_t mode = bus_r16(ctx->a[5] + 2);
	cc_logic16(ctx, mode);
	if (mode & 0x8000) {
		shaggy_walk_finish(ctx);
		return;
	}
	hle_jump(ctx, SW_ANIM_WALK);
}

/* $00561E: arrived? then the stand animation (walk animation + 4) */
HLE_RANGE(0x00561E, shaggy_walk_moving, 30, SW_ANIM_WALK, SW_ANIM_STAND)
{
	if (rbit(ctx, ACTORS_MOVING, 0)) {
		hle_jump(ctx, SW_ANIM_WALK);
		return;
	}
	set_w(&D(6), cc_add(ctx, D(6), 4, 16));
	RAM16(ACTOR_ANIM(0)) = (uint16_t)D(6);
	stw(ctx, SHAGGY_ANIM_TIMER, 0xFFFF);
	rbset(ctx, ACTORS_ANIM_NEW, 0);
	hle_jump(ctx, SW_ANIM_STAND);
}

/* $005648: stand animation started? then done */
HLE_RANGE(0x005648, shaggy_walk_standing, 39, SW_ANIM_STAND, HLE_EXIT_SP(SW_RTS, 2))
{
	if (rbit(ctx, ACTORS_ANIM_NEW, 0)) {
		hle_jump(ctx, SW_ANIM_STAND);
		return;
	}
	shaggy_walk_finish(ctx);
}

/* ------------------------------------------------------------------ Scooby */
#define SC_RTS_EARLY  0x0056D0
#define SC_ANIM_TURN  0x005B30
#define SC_RTS        0x005B54

static void scooby_start(hle_ctx *ctx)
{
	walk_start(ctx, &SCOOBY, SCOOBY_TURN_TABLE, SCOOBY_ANIM_LOCK);
	hle_jump(ctx, SC_ANIM_TURN);
}

/* $0056B0 scooby_walk_to: same as Shaggy's planner; returns once the turn animation is done
 * (the walk itself runs in the background, see $00566A for the waiting variant) */
HLE_RANGE(0x0056B0, scooby_walk_to, 70, SC_RTS_EARLY, SC_ANIM_TURN)
{
	rbclr(ctx, ACTORS_MOVING, 1);
	rbclr(ctx, GAME_FLAGS_09DE, 5);
	if (cmpw(ctx, D(0), RAM16(ACTOR_X(1))) == 0 && cmpw(ctx, D(1), RAM16(ACTOR_Y(1))) == 0) {
		hle_jump(ctx, SC_RTS_EARLY);
		return;
	}
	CHARGE((walk_plan(ctx, &SCOOBY), scooby_start(ctx)));
}

/* $005AD8 scooby_walk_direct: straight to (D0,D1), no route planning */
HLE_RANGE(0x005AD8, scooby_walk_direct, 0, SC_ANIM_TURN)
{
	stw(ctx, SCOOBY_WP_COUNT, 0);
	CHARGE(scooby_start(ctx));
}

/* $005B34: turn animation done? then set Scooby moving and return */
HLE_RANGE(0x005B34, scooby_walk_turned, 67, SC_ANIM_TURN, SC_RTS)
{
	if (rbit(ctx, ACTORS_ANIM_NEW, 1)) {
		hle_jump(ctx, SC_ANIM_TURN);
		return;
	}
	rbit(ctx, ACTORS_TURN_DONE, 1);
	if (ctx->sr & CC_Z) {
		hle_jump(ctx, SC_ANIM_TURN);
		return;
	}
	rbset(ctx, ACTORS_MOVING, 1);
	hle_jump(ctx, SC_RTS);
}

/* $00566A scooby_walk_wait: `bsr scooby_walk_to`, then (op word 2 >= 0) wait for the
 * arrival and play the stand animation. The bsr stays 68k; these are the pieces after it. */
#define SCW_ANIM_WALK  0x005674
#define SCW_ANIM_STAND 0x00569E
#define SCW_RTS        0x0056AE
HLE_RANGE(0x00566E, scooby_walk_wait_mode, 20, SCW_ANIM_WALK, SCW_RTS)
{
	uint16_t mode = bus_r16(ctx->a[5] + 2);
	cc_logic16(ctx, mode);
	hle_jump(ctx, (mode & 0x8000) ? SCW_RTS : SCW_ANIM_WALK);
}
HLE_RANGE(0x005678, scooby_walk_wait_moving, 30, SCW_ANIM_WALK, SCW_ANIM_STAND)
{
	if (rbit(ctx, ACTORS_MOVING, 1)) {
		hle_jump(ctx, SCW_ANIM_WALK);
		return;
	}
	set_w(&D(6), cc_add(ctx, D(6), 4, 16));
	RAM16(ACTOR_ANIM(1)) = (uint16_t)D(6);
	stw(ctx, SCOOBY_ANIM_LOCK, 0xFFFF);
	rbset(ctx, ACTORS_ANIM_NEW, 1);
	hle_jump(ctx, SCW_ANIM_STAND);
}
HLE_RANGE(0x0056A2, scooby_walk_wait_standing, 31, SCW_ANIM_STAND, SCW_RTS)
{
	hle_jump(ctx, rbit(ctx, ACTORS_ANIM_NEW, 1) ? SCW_ANIM_STAND : SCW_RTS);
}
