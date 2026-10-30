/* More script op handlers and sub-ops (GitLab #19; byte code: docs/script.md, status:
 * docs/subsystems/script.md). Actor ops (face, wait_stop, path, stop_path), the inventory
 * ops (show_obj, set_icon) and the one-instruction sub-ops of op $0C.
 *
 * Handlers are entered by the dispatcher's `jsr (a0)` with Z = scan mode (docs: script.c).
 * Blocking ones (a frame of actor work, wait_vblank, inventory_refresh) are HLE_RANGEs that
 * stop in front of the call; the range after it finishes the op.
 */
#include "game.h"
#include "cc68k.h"

#define D(n) ctx->d[n]
#define A(n) ctx->a[n]

#define RAM_OBJECTS         0xFF1200
#define   OBJ_ROOM            0x06
#define   OBJ_ICON            0x04
#define   OBJ_FLAGS           0x18
#define   OBJ_ACTOR           0x19
#define RAM_ROOM_INDEX      0xFF06AC
#define RAM_INV_LIST        0xFF0A1E   /* words, < 0 terminated */
#define RAM_INV_PAGE        0xFF06EC
#define RAM_ACTOR_ANIM      0xFF0488
#define RAM_SHAGGY_FACING   0xFF05B8
#define RAM_SCOOBY_FACING   0xFF05BA
#define RAM_ACTOR_START     0xFF0AB5
#define RAM_ACTORS_MOVING   0xFF0AB7
#define RAM_ACTORS_ANIMDONE 0xFF0ABC
#define RAM_ACTORS_ON_PATH  0xFF0AC0
#define RAM_ACTORS_PATHONCE 0xFF0AC3
#define RAM_ACTOR_PATH_PTR  0xFF05F4   /* 6 longs */
#define RAM_ACTOR_PATH_POS  0xFF060C   /* 6 words */
#define RAM_ACTOR_PATH_DLY  0xFF0AA5   /* 6 bytes delay, +6: reload */
#define SHAGGY_FACE_TABLE   0x0FC740   /* w[4][4]: Shaggy's turn-on-the-spot animation [old][new] */
#define SCOOBY_FACE_TABLE   0x0FC700   /* same for Scooby */

static inline uint16_t opw(hle_ctx *c, int off) { return bus_r16(c->a[5] + off); }
static inline bool exec_mode(const hle_ctx *c) { return !(c->sr & CC_Z); }
static bool bit_test(hle_ctx *c, uint32_t a, uint32_t bit)
{
	cc_btst(c, RAM8(a), bit & 7);
	return (RAM8(a) >> (bit & 7)) & 1;
}
static void bit_set(hle_ctx *c, uint32_t a, uint32_t bit) { bit_test(c, a, bit); RAM8(a) |= (uint8_t)(1u << (bit & 7)); }
static void bit_clr(hle_ctx *c, uint32_t a, uint32_t bit) { bit_test(c, a, bit); RAM8(a) &= (uint8_t)~(1u << (bit & 7)); }

/* who (1 Shaggy, 2 Scooby, >= 3 object) to actor number in D0, the way wait_stop and
 * stop_path do it: subq #1 / subq #1 / moveq #1 / subq #1, mulu #26, slot byte, +2 */
static void who_to_actor(hle_ctx *ctx, uint16_t who)
{
	set_w(&D(0), who);
	set_w(&D(0), cc_sub(ctx, D(0), 1, 16, true));
	if (who == 1)
		return;                                        /* Shaggy: 0 */
	set_w(&D(0), cc_sub(ctx, D(0), 1, 16, true));
	if (who == 2) {
		D(0) = 1;                                      /* Scooby */
		cc_logic32(ctx, 1);
		return;
	}
	set_w(&D(0), cc_sub(ctx, D(0), 1, 16, true));
	D(0) = (D(0) & 0xFFFF) * 26;
	cc_logic32(ctx, D(0));
	A(0) = RAM_OBJECTS;
	set_b(&D(0), RAM8(RAM_OBJECTS + (int16_t)D(0) + OBJ_ACTOR));
	set_w(&D(0), D(0) & 0xFF);
	set_w(&D(0), cc_add(ctx, D(0), 2, 16));
}

/* ------------------------------------------------------------------ $1E face */
/* $00268E op $1E face [1E][who|$8000][dir] (6 bytes): turn Shaggy (who 1) or Scooby (any
 * other who) on the spot to facing `dir` with the turn animation [old facing][dir]. With
 * bit 15 of `who`, wait (a frame of actor work at a time) until the animation is done.
 * Quirk: Scooby's restart bit is only set when waiting. */
#define FACE_RTS       0x002730
#define FACE_WAIT_SHAG 0x0026D4
#define FACE_WAIT_SCOO 0x002716
HLE_RANGE(0x00268E, script_op_face, 200, FACE_RTS, FACE_WAIT_SHAG, FACE_WAIT_SCOO)
{
	if (!exec_mode(ctx)) {
		A(5) += 6;
		hle_jump(ctx, FACE_RTS);
		return;
	}
	set_w(&D(3), opw(ctx, 2));
	cc_btst(ctx, D(3), 15);
	set_w(&D(3), D(3) & 0x7FFF);
	set_w(&D(0), opw(ctx, 4));
	bool shaggy = cmpw(ctx, D(3), 1) == 0;
	uint32_t facing = shaggy ? RAM_SHAGGY_FACING : RAM_SCOOBY_FACING;
	set_w(&D(1), RAM16(facing));
	set_w(&D(1), lslw(ctx, D(1), 3));
	set_w(&D(2), D(0));
	set_w(&D(2), cc_add(ctx, D(2), D(2), 16));
	set_w(&D(1), cc_add(ctx, D(1), D(2), 16));
	RAM16(facing) = (uint16_t)D(0);
	A(0) = shaggy ? SHAGGY_FACE_TABLE : SCOOBY_FACE_TABLE;
	RAM16(RAM_ACTOR_ANIM + (shaggy ? 0 : 2)) = ROM16(A(0) + (int16_t)D(1));
	uint16_t mode = opw(ctx, 2);
	cc_logic16(ctx, mode);                             /* tst.w (2,a5) */
	if (shaggy)
		bit_set(ctx, RAM_ACTOR_START, 0);              /* (bset keeps N for the bpl) */
	if (!(mode & 0x8000)) {
		A(5) += 6;
		hle_jump(ctx, FACE_RTS);
		return;
	}
	if (!shaggy)
		bit_set(ctx, RAM_ACTOR_START, 1);
	hle_jump(ctx, shaggy ? FACE_WAIT_SHAG : FACE_WAIT_SCOO);
}
/* after `jsr actor_animate`: until the turn animation is done */
HLE_RANGE(0x0026DA, script_op_face_wait_shaggy, 30, FACE_WAIT_SHAG, FACE_RTS)
{
	if (!bit_test(ctx, RAM_ACTORS_ANIMDONE, 0)) {
		hle_jump(ctx, FACE_WAIT_SHAG);
		return;
	}
	A(5) += 6;
	hle_jump(ctx, FACE_RTS);
}
HLE_RANGE(0x00271C, script_op_face_wait_scooby, 40, FACE_WAIT_SCOO, FACE_RTS)
{
	if (!bit_test(ctx, RAM_ACTORS_ANIMDONE, 1)) {
		hle_jump(ctx, FACE_WAIT_SCOO);
		return;
	}
	RAM16(RAM_SCOOBY_FACING) = (uint16_t)D(0);
	cc_logic16(ctx, D(0));
	A(5) += 6;
	hle_jump(ctx, FACE_RTS);
}

/* ------------------------------------------------------------------ $1F wait_stop */
/* $004F70 op $1F wait_stop [1F][who] (4 bytes): one frame of actor work, repeated while
 * the actor is walking to a target or playing a scripted path. */
#define WST_FRAME 0x004FA0
#define WST_RTS   0x004FBC
HLE_RANGE(0x004F70, script_op_wait_stop, 120, WST_FRAME, WST_RTS)
{
	if (!exec_mode(ctx)) {
		A(5) += 4;
		hle_jump(ctx, WST_RTS);
		return;
	}
	who_to_actor(ctx, opw(ctx, 2));
	hle_jump(ctx, WST_FRAME);
}
HLE_RANGE(0x004FA4, script_op_wait_stop_test, 64, WST_FRAME, WST_RTS)
{
	if (bit_test(ctx, RAM_ACTORS_MOVING, D(0)) || bit_test(ctx, RAM_ACTORS_ON_PATH, D(0))) {
		hle_jump(ctx, WST_FRAME);
		return;
	}
	A(5) += 4;
	hle_jump(ctx, WST_RTS);
}

/* ------------------------------------------------------------------ $20 / $21 paths */
/* $0048F8 op $20 stop_path [20][who] (4 bytes): an actor walking to a target stops there
 * (moving bit cleared); otherwise its scripted path stops at the next terminator
 * (path_once bit). */
HLE_FUNC(0x0048F8, script_op_stop_path, 120)
{
	if (exec_mode(ctx)) {
		who_to_actor(ctx, opw(ctx, 2));
		if (bit_test(ctx, RAM_ACTORS_MOVING, D(0)))
			bit_clr(ctx, RAM_ACTORS_MOVING, D(0));
		else
			bit_set(ctx, RAM_ACTORS_PATHONCE, D(0));
	}
	A(5) += 4;
}

/* $004944 op $21 path [21][len][who][delay] path data... (8 + len bytes): start a scripted
 * path for Shaggy (who 1), Scooby (2) or an actor object: path pointer = the data after
 * the header, delay/reload = `delay`, position 0, loop mode (path_once cleared), on. */
HLE_FUNC(0x004944, script_op_path, 160)
{
	if (exec_mode(ctx)) {
		uint16_t who = opw(ctx, 4);
		set_w(&D(0), who);
		set_w(&D(0), cc_sub(ctx, D(0), 1, 16, true));
		if (who != 1 && cmpw(ctx, D(0), 1) != 0) {
			set_w(&D(0), cc_sub(ctx, D(0), 2, 16, true));
			D(0) = (D(0) & 0xFFFF) * 26;
			cc_logic32(ctx, D(0));
			A(0) = RAM_OBJECTS;
			set_b(&D(0), RAM8(RAM_OBJECTS + (int16_t)D(0) + OBJ_ACTOR));
			set_w(&D(0), (uint16_t)(int16_t)(int8_t)D(0));   /* ext.w */
			set_w(&D(0), cc_add(ctx, D(0), 2, 16));
		}
		int16_t n = (int16_t)D(0);
		set_w(&D(1), D(0));
		set_w(&D(1), cc_add(ctx, D(1), D(1), 16));
		set_w(&D(2), D(1));
		set_w(&D(2), cc_add(ctx, D(2), D(2), 16));
		A(0) = RAM_ACTOR_PATH_PTR;
		A(1) = A(5) + 8;
		D(3) = A(1);
		RAM32_W(RAM_ACTOR_PATH_PTR + (int16_t)D(2), D(3));
		set_w(&D(3), opw(ctx, 6));
		A(0) = RAM_ACTOR_PATH_DLY;
		RAM8(RAM_ACTOR_PATH_DLY + n) = (uint8_t)D(3);
		RAM8(RAM_ACTOR_PATH_DLY + 6 + n) = (uint8_t)D(3);
		A(0) = RAM_ACTOR_PATH_POS;
		RAM16(RAM_ACTOR_PATH_POS + (int16_t)D(1)) = 0;
		bit_clr(ctx, RAM_ACTORS_PATHONCE, D(0));
		bit_set(ctx, RAM_ACTORS_ON_PATH, D(0));
	}
	uint16_t len = opw(ctx, 2);
	set_w(&D(0), len);
	cc_logic16(ctx, len);
	A(5) += 8 + (int32_t)(int16_t)len;
}

/* ------------------------------------------------------------------ inventory ops */
/* $002BF8 op $07 show_obj [07][who] (4 bytes): an object in the current room gets its
 * redraw bit; an object in the inventory (room 1) turns the inventory to the page with the
 * last item (the inventory list's length / 4) and redraws it (inventory_refresh $005CF6
 * waits for the HUD upload: the 68k makes that call). */
#define SO_REFRESH 0x002C4C
#define SO_RTS     0x002C54
HLE_RANGE(0x002BF8, script_op_show_obj, 240, SO_REFRESH, SO_RTS)
{
	if (exec_mode(ctx)) {
		set_w(&D(0), opw(ctx, 2));
		set_w(&D(0), cc_sub(ctx, D(0), 3, 16, true));
		D(0) = (D(0) & 0xFFFF) * 26;
		cc_logic32(ctx, D(0));
		A(0) = RAM_OBJECTS;
		uint32_t rec = RAM_OBJECTS + (int16_t)D(0);
		set_w(&D(1), RAM16(RAM_ROOM_INDEX));
		if (cmpw(ctx, D(1), RAM16(rec + OBJ_ROOM)) == 0) {
			RAM8(rec + OBJ_FLAGS) |= 1;
			cc_logic8(ctx, RAM8(rec + OBJ_FLAGS));
		} else if (cmpw(ctx, RAM16(rec + OBJ_ROOM), 1) == 0) {
			A(0) = RAM_INV_LIST;
			set_w(&D(0), 0xFFFF);
			uint16_t v;
			do {
				set_w(&D(0), cc_add(ctx, D(0), 1, 16));
				v = RAM16(A(0));
				A(0) += 2;
				cc_logic16(ctx, v);
			} while (!(v & 0x8000));
			set_w(&D(0), lsrw(ctx, D(0), 2));
			set_w(&D(1), RAM16(RAM_INV_PAGE));
			set_w(&D(1), cc_sub(ctx, D(1), 1, 16, true));
			if (cmpw(ctx, D(0), D(1)) != 0) {
				RAM16(RAM_INV_PAGE) = (uint16_t)D(0);
				cc_logic16(ctx, D(0));
				hle_jump(ctx, SO_REFRESH);
				return;
			}
		}
	}
	A(5) += 4;
	hle_jump(ctx, SO_RTS);
}
HLE_RANGE(0x002C50, script_op_show_obj_done, 8, SO_RTS) { A(5) += 4; hle_jump(ctx, SO_RTS); }

/* $002DC0 op $0A set_icon [0A][who][icon] (6 bytes): the object's inventory icon; if it is
 * in the inventory, wait a frame and redraw the inventory (wait_vblank + inventory_refresh
 * stay 68k). */
#define SI_WAIT 0x002DE4
#define SI_RTS  0x002DF0
HLE_RANGE(0x002DC0, script_op_set_icon, 140, SI_WAIT, SI_RTS)
{
	if (exec_mode(ctx)) {
		set_w(&D(0), opw(ctx, 2));
		set_w(&D(0), cc_sub(ctx, D(0), 3, 16, true));
		D(0) = (D(0) & 0xFFFF) * 26;
		cc_logic32(ctx, D(0));
		A(0) = RAM_OBJECTS;
		uint32_t rec = RAM_OBJECTS + (int16_t)D(0);
		RAM16(rec + OBJ_ICON) = opw(ctx, 4);
		if (cmpw(ctx, RAM16(rec + OBJ_ROOM), 1) == 0) {
			hle_jump(ctx, SI_WAIT);
			return;
		}
	}
	A(5) += 6;
	hle_jump(ctx, SI_RTS);
}
HLE_RANGE(0x002DEC, script_op_set_icon_done, 8, SI_RTS) { A(5) += 6; hle_jump(ctx, SI_RTS); }

/* ------------------------------------------------------------------ sub-ops */
/* One-instruction sub-ops: bset/bclr #bit,(flag byte); Z = !old bit. Named after what the
 * flag is known to do (docs/script.md section 6), else after the bit. */
#define SUBOP_FLAG(ADDR, NAME, BYTE, BIT, SET)                   \
	HLE_FUNC(ADDR, NAME, 28)                                       \
	{                                                              \
		if (SET) bit_set(ctx, BYTE, BIT); else bit_clr(ctx, BYTE, BIT); \
	}
#define R_09DE 0xFF09DE   /* verb state: bit 6 close-up shown, bit 7 Scooby away */
#define R_09E8 0xFF09E8
#define R_09E9 0xFF09E9
#define R_09EB 0xFF09EB
#define R_FLAGS2 0xFF0ACA /* bit 1 skippable, bit 2 leave TALK, bit 3 then_talk */
SUBOP_FLAG(0x002FAE, script_sys_09eb_6_on, R_09EB, 6, 1)          /* $3C */
SUBOP_FLAG(0x002FB8, script_sys_09eb_6_off, R_09EB, 6, 0)         /* $3D */
SUBOP_FLAG(0x003014, script_sys_scooby_pose_off, R_09EB, 2, 0)    /* $36 */
SUBOP_FLAG(0x00312E, script_sys_09eb_0_on, R_09EB, 0, 1)          /* $33 */
SUBOP_FLAG(0x003138, script_sys_09eb_0_off, R_09EB, 0, 0)         /* $34 */
SUBOP_FLAG(0x003EAA, script_sys_no_idle_anim_on, R_09E9, 7, 1)    /* $30 */
SUBOP_FLAG(0x003EB4, script_sys_no_idle_anim_off, R_09E9, 7, 0)   /* $31 */
SUBOP_FLAG(0x003EBE, script_sys_09e9_6_on, R_09E9, 6, 1)          /* $2D */
SUBOP_FLAG(0x003EC8, script_sys_09e9_6_off, R_09E9, 6, 0)         /* $2E */
SUBOP_FLAG(0x003F3A, script_sys_09e9_3_on, R_09E9, 3, 1)          /* $29 */
SUBOP_FLAG(0x003F44, script_sys_09e9_3_off, R_09E9, 3, 0)         /* $2A */
SUBOP_FLAG(0x003F4E, script_sys_talk_end, R_FLAGS2, 2, 1)         /* $28 */
SUBOP_FLAG(0x00410C, script_sys_09e9_1_on, R_09E9, 1, 1)          /* $23 */
SUBOP_FLAG(0x004116, script_sys_09e9_1_off, R_09E9, 1, 0)         /* $24 */
SUBOP_FLAG(0x0041A2, script_sys_09e9_0_on, R_09E9, 0, 1)          /* $1F */
SUBOP_FLAG(0x0041AC, script_sys_09e9_0_off, R_09E9, 0, 0)         /* $20 */
SUBOP_FLAG(0x004322, script_sys_overlay_off, R_09E8, 4, 0)        /* $19 (clears bit 4 only) */
SUBOP_FLAG(0x00436A, script_sys_follow_mode_off, R_09E8, 3, 0)    /* $16 */
SUBOP_FLAG(0x0043AC, script_sys_09e8_2_on, R_09E8, 2, 1)          /* $11 */
SUBOP_FLAG(0x0043B6, script_sys_09e8_2_off, R_09E8, 2, 0)         /* $12 */

#ifdef SCRIPT_UNTESTED   /* sub-ops $00 and $08 occur nowhere in the script data */
HLE_FUNC(0x002FAC, script_sys_nop, 16) { (void)ctx; }                 /* $00 */
SUBOP_FLAG(0x004534, script_sys_closeup_off, R_09DE, 6, 0)         /* $08 */
#endif

/* $004496 / $0044A8 sub $09 scooby_away / $0A scooby_back: verb state bit 7 and Scooby's
 * frozen bit (actor 1 of $FF0ABF) */
HLE_FUNC(0x004496, script_sys_scooby_away, 56)
{
	bit_set(ctx, R_09DE, 7);
	bit_set(ctx, 0xFF0ABF, 1);
}
HLE_FUNC(0x0044A8, script_sys_scooby_back, 56)
{
	bit_clr(ctx, R_09DE, 7);
	bit_clr(ctx, 0xFF0ABF, 1);
}

/* $002FFC sub $0B scooby_pose_on: Scooby's animation = his facing + 4 (the stand pose),
 * $FF09EB bit 2 */
HLE_FUNC(0x002FFC, script_sys_scooby_pose_on, 70)
{
	set_w(&D(0), RAM16(RAM_SCOOBY_FACING));
	set_w(&D(0), cc_add(ctx, D(0), 4, 16));
	RAM16(RAM_ACTOR_ANIM + 2) = (uint16_t)D(0);
	cc_logic16(ctx, D(0));
	bit_set(ctx, R_09EB, 2);
}

/* $003E96 sub $2F then_talk: after this action, TALK to the object in object 0's var
 * ($FF06DA = $FF120C), flags2 bit 3 */
HLE_FUNC(0x003E96, script_sys_then_talk, 60)
{
	bit_set(ctx, R_FLAGS2, 3);
	uint16_t v = RAM16(RAM_OBJECTS + 0x0C);
	RAM16(0xFF06DA) = v;
	cc_logic16(ctx, v);
}

/* $003F16 sub $2C shaggy_alt_sprite_off: back to Shaggy's normal sprite set ($032700) with
 * depth scaling; $FF09E9 bit 4 and the script-running bit of VBLANK_FLAGS cleared */
HLE_FUNC(0x003F16, script_sys_shaggy_alt_sprite_off, 100)
{
	bit_set(ctx, 0xFF0AB8, 0);
	RAM32_W(0xFF0470, 0x032700);
	cc_logic32(ctx, 0x032700);
	bit_clr(ctx, R_09E9, 4);
	bit_clr(ctx, VBLANK_FLAGS, 7);
}

/* $0040E0 sub $26 test_shaggy_path: object 0's var = 0 while Shaggy plays a scripted path,
 * else 1 */
HLE_FUNC(0x0040E0, script_sys_test_shaggy_path, 60)
{
	RAM16(RAM_OBJECTS + 0x0C) = 0;
	cc_logic16(ctx, 0);
	if (!bit_test(ctx, RAM_ACTORS_ON_PATH, 0)) {
		RAM16(RAM_OBJECTS + 0x0C) = 1;
		cc_logic16(ctx, 1);
	}
}
