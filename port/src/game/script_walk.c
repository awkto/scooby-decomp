/* Script walk ops (GitLab #19, part of the script interpreter; byte code: docs/script.md,
 * status: docs/subsystems/script.md). The walk-to routines they call are in walkto.c.
 *
 * Op $0E walk_spawn / $1B walk_xy ($0049BA, 10 / 12 bytes) and op $0F walk_obj ($004C20,
 * 12 bytes) move Shaggy, Scooby or an actor object. They save D7 on the stack
 * (`move.w d7,-(sp)`) and then call the blocking walk-to routines or wait for an animation,
 * so they are ranges with stack-delta exits (HLE_EXIT_SP, docs/porting.md "Ranges that push
 * or pop"): the first range exits at the blocking call with SP - 2, the range after the
 * call pops D7 and exits at the handler's rts with SP + 2.
 *
 *   $0E/$1B: [op][who|$8000][speed][anim][x][y]   ($0E: x = entry point number, no y)
 *     who 1 Shaggy, 2 Scooby, >= 3 object; speed < 0 = put it there (and play `anim` once
 *     if >= 0), else walk there at that speed with animation base `anim`.
 *   $0F:     [op][who|$8000][obj][anim][dx][speed]
 *     walk `who` to object `obj` (the object's position + dx + the hotspot of its sprite
 *     frame), then (Shaggy/Scooby) take the object's direction as facing.
 *   Bit 15 of word 2 (read by the walk-to routines): don't wait for the arrival.
 */
#include "game.h"
#include "cc68k.h"

#define D(n) ctx->d[n]
#define A(n) ctx->a[n]

#define RAM_OBJECTS         0xFF1200
#define   OBJ_HOTSPOT         0x02     /* sprite set id of an actor object */
#define   OBJ_DIR             0x08
#define   OBJ_X               0x12
#define   OBJ_Y               0x14
#define   OBJ_FLAGS           0x18     /* bit 1: has an actor */
#define   OBJ_ACTOR           0x19     /* actor slot - 2, < 0 = none */
#define RAM_ROOM_ENTRIES    0xFF0696   /* long: 8 x (x, y) entry points, tiles */
#define RAM_ACTOR_ANIMSET   0xFF0470   /* 6 longs: sprite set of each actor */
#define RAM_ACTOR_ANIM      0xFF0488   /* 6 words */
#define RAM_SHAGGY_TIMER    0xFF0618   /* w: Shaggy's animation timer, -1 = restart */
#define RAM_SCOOBY_ANIM_LCK 0xFF061A
#define RAM_ACTOR_TIMER2    0xFF061C   /* actors 2-5 animation timer (words) */
#define RAM_ACTOR_X2        0xFF04E4   /* actors 2-5 x (16.16 longs); y at +$18 */
#define RAM_ACTOR_STEP2     0xFF051C   /* actors 2-5 x step (16.16 longs); y step at +$14 */
#define RAM_ACTOR_TARGET2   0xFF05E0   /* actors 2-5 target x (words); y at +$C */
#define RAM_SHAGGY_FACING   0xFF05B8
#define RAM_SCOOBY_FACING   0xFF05BA
#define RAM_ACTOR_START     0xFF0AB5   /* bit n: (re)start actor n's animation */
#define RAM_ACTORS_MOVING   0xFF0AB7
#define RAM_ACTORS_LOOPED   0xFF0ABD
#define SPRITE_SETS         0x032670   /* 36 longs */
static const uint32_t ACTOR_XW[2] = { 0xFF04DC, 0xFF04E0 };   /* Shaggy, Scooby x (pixel word) */
static const uint32_t ACTOR_YW[2] = { 0xFF04F4, 0xFF04F8 };

static inline uint16_t opw(hle_ctx *c, int off) { return bus_r16(c->a[5] + off); }
static inline bool exec_mode(const hle_ctx *c) { return !(c->sr & CC_Z); }
/* btst/bset on a RAM byte, bit number mod 8 (Z = !old bit) */
static bool bit_test(hle_ctx *c, uint32_t a, uint32_t bit)
{
	cc_btst(c, RAM8(a), bit & 7);
	return (RAM8(a) >> (bit & 7)) & 1;
}
static void bit_set(hle_ctx *c, uint32_t a, uint32_t bit) { bit_test(c, a, bit); RAM8(a) |= (uint8_t)(1u << (bit & 7)); }
/* move.w #v / Dn,(mem): store, N Z from the word */
static void stw(hle_ctx *c, uint32_t a, uint32_t v) { RAM16(a) = (uint16_t)v; cc_logic16(c, v); }
/* mulu.w #26,Dn on the low word: full 32-bit product */
static uint32_t mul26(hle_ctx *c, uint32_t v)
{
	uint32_t p = (v & 0xFFFF) * 26;
	cc_logic32(c, p);
	return p;
}

/* ================================================================== $0E / $1B */
#define WS_RTS12 0x004B1C   /* rts after `lea 12(a5),a5` ($1B) */
#define WS_RTS10 0x004B22   /* rts after `lea 10(a5),a5` ($0E) */

/* the end of walk_spawn: `move.w (sp)+,d7` (if pushed), skip the op */
static void walk_spawn_next(hle_ctx *ctx, int popped)
{
	if (popped) {
		set_w(&D(7), pop16(ctx));
		cc_logic16(ctx, D(7));
	}
	uint16_t op = opw(ctx, 0);
	set_w(&D(0), op);
	bool xy = cmpw(ctx, op, 0x0E) != 0;
	A(5) += xy ? 0x0C : 0x0A;
	hle_jump(ctx, xy ? WS_RTS12 : WS_RTS10);
}

/* target of $0E/$1B into (D0,D1) (rx = 0) or (D1,D2) (rx = 1): $1B gives pixels, $0E an
 * entry point of the room (tiles; A0 = the entry table) */
static void walk_spawn_target(hle_ctx *ctx, int rx)
{
	uint32_t *x = &D(rx), *y = &D(rx + 1);
	uint16_t op = opw(ctx, 0);
	set_w(&D(0), op);
	if (cmpw(ctx, op, 0x0E) != 0) {
		set_w(x, opw(ctx, 8));
		set_w(y, opw(ctx, 10));
		return;
	}
	set_w(x, lslw(ctx, opw(ctx, 8), 2));
	set_w(y, bus_r16(A(0) + 2 + (int16_t)*x));
	set_w(x, bus_r16(A(0) + (int16_t)*x));
	set_w(x, lslw(ctx, *x, 3));
	set_w(y, lslw(ctx, *y, 3));
}

HLE_RANGE(0x0049BA, script_op_walk_spawn, 230, WS_RTS12, WS_RTS10,
          HLE_EXIT_SP(0x004A30, -2), HLE_EXIT_SP(0x004A48, -2), HLE_EXIT_SP(0x004AAE, -2),
          HLE_EXIT_SP(0x004AC6, -2), HLE_EXIT_SP(0x004B0A, -2))
{
	if (!exec_mode(ctx)) {
		walk_spawn_next(ctx, 0);
		return;
	}
	push16(ctx, (uint16_t)D(7));
	uint16_t who = opw(ctx, 2) & 0x7FFF;
	set_w(&D(0), who);
	cc_logic16(ctx, who);
	if (who == 0) {
		walk_spawn_next(ctx, 1);
		return;
	}
	A(0) = RAM32(RAM_ROOM_ENTRIES);
	set_w(&D(0), cc_sub(ctx, D(0), 1, 16, true));
	if (who > 1)
		set_w(&D(0), cc_sub(ctx, D(0), 1, 16, true));
	if (who > 2) {
		/* an actor object: actor_walk_to (D0 who, D1/D2 target, D3 speed, D7 anim) */
		set_w(&D(0), who);
		cc_logic16(ctx, who);
		set_w(&D(3), opw(ctx, 4));
		walk_spawn_target(ctx, 1);
		set_w(&D(7), opw(ctx, 6));
		set_w(&D(0), who);
		cc_logic16(ctx, who);
		hle_jump(ctx, 0x004B0A);
		return;
	}
	int s = who - 1;                                   /* 0 Shaggy, 1 Scooby */
	walk_spawn_target(ctx, 0);
	set_w(&D(7), opw(ctx, 6));
	uint16_t speed = opw(ctx, 4);
	cc_logic16(ctx, speed);
	if (!(speed & 0x8000)) {                           /* walk there */
		set_w(&D(2), speed);
		hle_jump(ctx, s ? 0x004AC6 : 0x004A48);        /* bsr scooby_walk_wait / shaggy_walk_to */
		return;
	}
	/* put the actor there, optionally play animation `anim` once */
	RAM16(ACTOR_XW[s]) = (uint16_t)D(0);
	stw(ctx, ACTOR_YW[s], D(1));
	cc_logic16(ctx, D(7));
	if (D(7) & 0x8000) {
		walk_spawn_next(ctx, 1);
		return;
	}
	RAM16(RAM_ACTOR_ANIM + 2 * s) = (uint16_t)D(7);
	if (s)
		stw(ctx, RAM_SCOOBY_ANIM_LCK, 1);
	else
		stw(ctx, RAM_SHAGGY_TIMER, 0xFFFF);
	bit_set(ctx, RAM_ACTOR_START, s);
	hle_jump(ctx, s ? 0x004AAE : 0x004A30);            /* bsr actor_animate until it looped */
}

/* after `bsr actor_animate` of the put-and-animate paths: until the animation looped */
static void walk_spawn_anim(hle_ctx *ctx, int s)
{
	if (!bit_test(ctx, RAM_ACTORS_LOOPED, s)) {
		hle_jump(ctx, s ? 0x004AAE : 0x004A30);
		return;
	}
	walk_spawn_next(ctx, 1);
}
#define WS_POP_EXITS HLE_EXIT_SP(WS_RTS12, 2), HLE_EXIT_SP(WS_RTS10, 2)
HLE_RANGE(0x004A34, script_op_walk_spawn_anim0, 40, 0x004A30, WS_POP_EXITS) { walk_spawn_anim(ctx, 0); }
HLE_RANGE(0x004AB2, script_op_walk_spawn_anim1, 40, 0x004AAE, WS_POP_EXITS) { walk_spawn_anim(ctx, 1); }
/* after shaggy_walk_to / scooby_walk_wait / actor_walk_to: `bra $4B0E` / $4B0E itself */
HLE_RANGE(0x004A4C, script_op_walk_spawn_done0, 50, WS_POP_EXITS) { walk_spawn_next(ctx, 1); }
HLE_RANGE(0x004ACA, script_op_walk_spawn_done1, 50, WS_POP_EXITS) { walk_spawn_next(ctx, 1); }
HLE_RANGE(0x004B0E, script_op_walk_spawn_done, 40, WS_POP_EXITS) { walk_spawn_next(ctx, 1); }

/* ================================================================== actor_walk_to */
/* $004B24 actor_walk_to: actor object D0 (who >= 3) to (D1,D2). D3 < 0: put it there and
 * play animation D7 once (if >= 0). Else walk at speed D3 (x step = the slot's base step *
 * speed, y step half of that) with animation D7 (if >= 0) and, unless bit 15 of the op's
 * word 2 is set, wait for the arrival. Objects without an actor slot only get the x/y. */
#define AW_RTS_NOACTOR 0x004B4A
#define AW_ANIM        0x004B9C
#define AW_RTS_PLACED  0x004BAA
#define AW_MOVING      0x004C10
#define AW_RTS         0x004C1E
HLE_RANGE(0x004B24, actor_walk_to, 220, AW_RTS_NOACTOR, AW_ANIM, AW_RTS_PLACED, AW_MOVING, AW_RTS)
{
	uint16_t x = (uint16_t)D(1), y = (uint16_t)D(2), anim = (uint16_t)D(7);
	set_w(&D(0), cc_sub(ctx, D(0), 3, 16, true));
	A(0) = RAM_OBJECTS;
	push16(ctx, anim);                                 /* popped again below */
	push16(ctx, y);
	push16(ctx, x);
	D(0) = mul26(ctx, D(0));
	uint32_t rec = RAM_OBJECTS + (uint32_t)(int32_t)(int16_t)D(0);
	RAM16(rec + OBJ_X) = x;
	RAM16(rec + OBJ_Y) = y;
	uint8_t slot = RAM8(rec + OBJ_ACTOR);
	set_b(&D(1), slot);
	cc_logic8(ctx, slot);
	if (slot & 0x80) {                                 /* no actor: just the position */
		A(7) += 6;
		hle_jump(ctx, AW_RTS_NOACTOR);
		return;
	}
	set_w(&D(1), slot);
	cc_logic16(ctx, slot);
	set_w(&D(7), slot);
	set_w(&D(7), cc_add(ctx, D(7), 2, 16));            /* actor number / bit */
	set_w(&D(1), cc_add(ctx, D(1), D(1), 16));         /* word index */
	set_w(&D(2), D(1));
	set_w(&D(2), cc_add(ctx, D(2), D(2), 16));         /* long index */
	int16_t wi = (int16_t)D(1), li = (int16_t)D(2);
	A(1) = RAM_ACTOR_STEP2;
	D(4) = RAM32(A(1) + li);
	D(5) = D(4);
	cc_logic16(ctx, D(3));
	if (D(3) & 0x8000) {                               /* put it there */
		A(0) = RAM_ACTOR_X2;
		RAM16(A(0) + li) = pop16(ctx);
		RAM16(A(0) + 0x18 + li) = pop16(ctx);
		set_w(&D(3), pop16(ctx));
		cc_logic16(ctx, D(3));
		if (D(3) & 0x8000) {
			hle_jump(ctx, AW_RTS_PLACED);
			return;
		}
		A(0) = RAM_ACTOR_ANIM + 4;
		RAM16(A(0) + wi) = (uint16_t)D(3);
		A(0) = RAM_ACTOR_TIMER2;
		stw(ctx, A(0) + wi, 0xFFFF);
		bit_set(ctx, RAM_ACTOR_START, D(7));
		hle_jump(ctx, AW_ANIM);
		return;
	}
	/* walk: step = base step * speed (speeds 0 and 1: the base step), y step half of it */
	set_w(&D(3), cc_sub(ctx, D(3), 2, 16, true));
	if (!(D(3) & 0x8000)) {
		do
			D(4) = cc_add(ctx, D(4), D(5), 32);
		while ((set_w(&D(3), D(3) - 1), (uint16_t)D(3) != 0xFFFF));   /* dbra */
	}
	RAM32_W(A(1) + li, D(4));
	unsigned out = D(4) & 1;                           /* lsr.l #1,d4 */
	D(4) >>= 1;
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, (out ? CC_X | CC_C : 0) | (D(4) ? 0 : CC_Z));
	RAM32_W(A(1) + 0x14 + li, D(4));
	A(3) = RAM_ACTOR_TARGET2;
	set_w(&D(3), pop16(ctx));
	stw(ctx, A(3) + wi, D(3));
	set_w(&D(4), pop16(ctx));
	stw(ctx, A(3) + 0x0C + wi, D(4));
	A(3) = RAM_ACTOR_ANIM + 4;
	uint16_t a = pop16(ctx);                           /* tst.w (sp) / (sp)+ */
	cc_logic16(ctx, a);
	if (!(a & 0x8000)) {
		RAM16(A(3) + wi) = a;
		A(0) = RAM_ACTOR_TIMER2;
		stw(ctx, A(0) + wi, 0xFFFF);
		bit_set(ctx, RAM_ACTOR_START, D(7));
	}
	bit_set(ctx, RAM_ACTORS_MOVING, D(7));
	A(3) = RAM_ACTOR_X2;
	uint16_t mode = opw(ctx, 2);
	cc_logic16(ctx, mode);
	hle_jump(ctx, (mode & 0x8000) ? AW_RTS : AW_MOVING);
}
/* after `bsr actor_animate`: until the animation looped / the actor arrived. btst.l Dn on
 * memory is a byte test (bit mod 8). */
HLE_RANGE(0x004BA0, actor_walk_to_anim, 27, AW_ANIM, AW_RTS_PLACED)
{
	hle_jump(ctx, bit_test(ctx, RAM_ACTORS_LOOPED, D(7)) ? AW_RTS_PLACED : AW_ANIM);
}
HLE_RANGE(0x004C14, actor_walk_to_moving, 26, AW_MOVING, AW_RTS)
{
	hle_jump(ctx, bit_test(ctx, RAM_ACTORS_MOVING, D(7)) ? AW_MOVING : AW_RTS);
}

/* ================================================================== $0F walk_obj */
/* $00197E sprite_frame_hotspot: D0 = frame index into the sprite set at A1; returns the
 * frame's hotspot (D0,D1) = words $E/$10 of the frame header that the frame's first
 * command-2 entry points to. A1 = that header. */
static void frame_hotspot(hle_ctx *ctx)
{
	uint32_t set = A(1);
	set_w(&D(0), lslw(ctx, D(0), 1));
	set_w(&D(0), cc_add(ctx, D(0), bus_r16(set + 2), 16));
	set_w(&D(0), bus_r16(set + (int16_t)D(0)));
	uint32_t p = set + (int16_t)D(0);
	while (cmpw(ctx, bus_r16(p), 2) != 0) {
		cc_logic32(ctx, bus_r32(p));                   /* tst.l (a1)+ */
		p += 4;
	}
	set_w(&D(0), bus_r16(p + 2));
	A(1) = set + (int16_t)D(0);
	set_w(&D(0), bus_r16(A(1) + 0x0E));
	set_w(&D(1), bus_r16(A(1) + 0x10));
	cc_logic16(ctx, D(1));
}
HLE_FUNC(0x00197E, sprite_frame_hotspot, 200) { frame_hotspot(ctx); }

#define WO_RTS 0x004D7E
HLE_RANGE(0x004C20, script_op_walk_obj, 660, WO_RTS, HLE_EXIT_SP(0x004D18, -2),
          HLE_EXIT_SP(0x004D44, -2), HLE_EXIT_SP(0x004D74, -2))
{
	if (!exec_mode(ctx)) {
		A(5) += 12;
		hle_jump(ctx, WO_RTS);
		return;
	}
	push16(ctx, (uint16_t)D(7));
	uint16_t who = opw(ctx, 2) & 0x7FFF;
	set_w(&D(0), who);
	set_w(&D(0), cc_sub(ctx, D(0), 1, 16, true));
	if (who == 1) {
		A(1) = RAM32(RAM_ACTOR_ANIMSET);               /* Shaggy's sprite set */
	} else if (set_w(&D(0), cc_sub(ctx, D(0), 1, 16, true)), who == 2) {
		A(1) = RAM32(RAM_ACTOR_ANIMSET + 4);           /* Scooby's */
	} else {
		/* an actor object: its sprite set by id (object word 2) */
		set_w(&D(0), cc_sub(ctx, D(0), 1, 16, true));
		D(0) = mul26(ctx, D(0));
		A(0) = RAM_OBJECTS;
		set_w(&D(0), RAM16(A(0) + (int16_t)D(0) + OBJ_HOTSPOT));
		A(0) = SPRITE_SETS;
		D(1) = 0x23;
		bool found = false;
		do {
			A(1) = ROM32(A(0));
			A(0) += 4;
			if (cmpw(ctx, D(0), bus_r16(A(1))) == 0) {
				found = true;
				break;
			}
			set_w(&D(1), D(1) - 1);
		} while ((uint16_t)D(1) != 0xFFFF);
		if (!found) {                                  /* unknown sprite set: nothing to do */
			set_w(&D(7), pop16(ctx));
			cc_logic16(ctx, D(7));
			A(5) += 12;
			hle_jump(ctx, WO_RTS);
			return;
		}
	}
	/* the target: the object's position + dx (word 8) */
	set_w(&D(0), opw(ctx, 8));
	uint16_t obj = opw(ctx, 4);
	set_w(&D(1), cc_sub(ctx, obj, 1, 16, true));
	if (obj == 1 || obj == 2) {                        /* Shaggy / Scooby as the target */
		if (obj == 2)
			set_w(&D(1), cc_sub(ctx, D(1), 1, 16, true));
		set_w(&D(0), cc_add(ctx, D(0), RAM16(ACTOR_XW[obj - 1]), 16));
		set_w(&D(1), cc_add(ctx, D(1), RAM16(ACTOR_YW[obj - 1]), 16));
	} else {
		set_w(&D(1), cc_sub(ctx, D(1), 1, 16, true));
		set_w(&D(1), cc_sub(ctx, D(1), 1, 16, true));
		D(1) = mul26(ctx, D(1));
		A(0) = RAM_OBJECTS + (int16_t)D(1);
		set_w(&D(0), cc_add(ctx, D(0), RAM16(A(0) + OBJ_DIR), 16));
		frame_hotspot(ctx);                            /* (bsr: its return address is scratch) */
		if (bit_test(ctx, A(0) + OBJ_FLAGS, 1)) {      /* an actor: from its x/y */
			set_w(&D(0), lslw(ctx, D(0), 1));
			set_w(&D(0), cc_add(ctx, D(0), RAM16(A(0) + OBJ_X), 16));
			set_w(&D(1), RAM16(A(0) + OBJ_Y));
			cc_logic16(ctx, D(1));
		} else {                                       /* a hotspot: from the room's hotspot table */
			set_w(&D(2), RAM16(A(0) + OBJ_HOTSPOT));
			set_w(&D(2), cc_sub(ctx, D(2), 1, 16, true));
			A(1) = RAM32(0xFF068E);                    /* mystery header */
			A(2) = bus_r32(A(1) + 0x24);
			set_w(&D(2), lslw(ctx, D(2), 2));
			A(2) = bus_r32(A(2) + (int16_t)D(2));
			A(2) += bus_r32(A(1) + 0x1C);
			set_w(&D(0), cc_add(ctx, D(0), bus_r16(A(2) + 8), 16));
			set_w(&D(1), cc_add(ctx, D(1), bus_r16(A(2) + 0x0A), 16));
		}
	}
	set_w(&D(7), opw(ctx, 6));
	set_w(&D(3), who);
	cc_logic16(ctx, who);
	if (cmpw(ctx, D(3), 1) == 0) {
		set_w(&D(2), opw(ctx, 0x0A));
		cc_logic16(ctx, D(2));
		hle_jump(ctx, 0x004D18);                       /* bsr shaggy_walk_to */
		return;
	}
	if (cmpw(ctx, D(3), 2) == 0) {
		set_w(&D(2), opw(ctx, 0x0A));
		cc_logic16(ctx, D(2));
		hle_jump(ctx, 0x004D44);                       /* bsr scooby_walk_direct */
		return;
	}
	set_w(&D(2), D(1));
	set_w(&D(1), D(0));
	set_w(&D(0), who);
	cc_logic16(ctx, who);
	set_w(&D(3), opw(ctx, 0x0A));
	cc_logic16(ctx, D(3));
	hle_jump(ctx, 0x004D74);                           /* bsr actor_walk_to */
}

/* after the walk: take the target object's direction as facing; pop D7, skip the op */
static void walk_obj_next(hle_ctx *ctx, uint32_t facing)
{
	if (facing) {
		A(0) = RAM_OBJECTS;
		set_w(&D(3), opw(ctx, 4));
		set_w(&D(3), cc_sub(ctx, D(3), 3, 16, true));
		D(3) = mul26(ctx, D(3));
		stw(ctx, facing, RAM16(A(0) + (int16_t)D(3) + OBJ_DIR));
	}
	set_w(&D(7), pop16(ctx));
	cc_logic16(ctx, D(7));
	A(5) += 12;
	hle_jump(ctx, WO_RTS);
}
HLE_RANGE(0x004D1C, script_op_walk_obj_shaggy, 128, HLE_EXIT_SP(WO_RTS, 2)) { walk_obj_next(ctx, RAM_SHAGGY_FACING); }
HLE_RANGE(0x004D48, script_op_walk_obj_scooby, 128, HLE_EXIT_SP(WO_RTS, 2)) { walk_obj_next(ctx, RAM_SCOOBY_FACING); }
HLE_RANGE(0x004D78, script_op_walk_obj_done, 16, HLE_EXIT_SP(WO_RTS, 2)) { walk_obj_next(ctx, 0); }
