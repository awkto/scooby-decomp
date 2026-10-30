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

/* ------------------------------------------------------------------ $10 say: the rest */
/* text.c (#20) ports op $10 say as ranges that stop at every stack push/pop and leave
 * Shaggy's pose waits in 68k. With stack-delta exits (HLE_EXIT_SP) the remaining pieces
 * are C too: the pushes/pops of D7 ($004D84/$004EDC), of the old facing ($004DBE/$004E80)
 * and of the font base ($004E1C/$004E5C), and the pose animation waits. Pose = bit 13 of
 * the op's word 2: Shaggy turns to facing bits 14-15 (turn animation from the table at
 * $0FC740), stands while the line is shown, and turns back afterwards. */
#define SAY_POSE_TABLE 0x0FC740
HLE_RANGE(0x004D84, script_op_say_push_d7, 8, HLE_EXIT_SP(0x004D86, -2))
{
	push16(ctx, (uint16_t)D(7));
	cc_logic16(ctx, D(7));
	hle_jump(ctx, 0x004D86);
}
HLE_RANGE(0x004DBE, script_op_say_push_facing, 8, HLE_EXIT_SP(0x004DC0, -2))
{
	push16(ctx, (uint16_t)D(1));                       /* the old facing */
	cc_logic16(ctx, D(1));
	hle_jump(ctx, 0x004DC0);
}
/* after `bsr actor_animate`: first until the turn animation started (done bit cleared),
 * then until it is done; then the stand pose and the box (text_clear, as text.c's
 * say_begin does on the path without a pose) */
HLE_RANGE(0x004DDE, script_op_say_pose_started, 30, 0x004DDA, 0x004DEA)
{
	hle_jump(ctx, bit_test(ctx, RAM_ACTORS_ANIMDONE, 0) ? 0x004DDA : 0x004DEA);
}
HLE_RANGE(0x004DEE, script_op_say_pose_done, 60, 0x004DEA, 0x004E1C)
{
	if (!bit_test(ctx, RAM_ACTORS_ANIMDONE, 0)) {
		hle_jump(ctx, 0x004DEA);
		return;
	}
	set_w(&D(0), RAM16(RAM_SHAGGY_FACING));
	set_w(&D(0), cc_add(ctx, D(0), 4, 16));
	RAM16(RAM_ACTOR_ANIM) = (uint16_t)D(0);
	cc_logic16(ctx, D(0));
	bit_set(ctx, RAM_ACTOR_START, 0);
	int keep = ctx->cycles;
	text_clear(ctx);                                   /* $004E12 bsr text_clear */
	ctx->cycles += keep;
	set_w(&D(2), RAM16(0xFF068C));                     /* the font base, pushed next */
	cc_logic16(ctx, D(2));
	hle_jump(ctx, 0x004E1C);
}
HLE_RANGE(0x004E1C, script_op_say_push_font, 8, HLE_EXIT_SP(0x004E1E, -2))
{
	push16(ctx, (uint16_t)D(2));
	cc_logic16(ctx, D(2));
	hle_jump(ctx, 0x004E1E);
}
HLE_RANGE(0x004E5C, script_op_say_pop_font, 8, HLE_EXIT_SP(0x004E5E, 2))
{
	set_w(&D(2), pop16(ctx));
	cc_logic16(ctx, D(2));
	hle_jump(ctx, 0x004E5E);
}
/* $004E80: pop the old facing, turn back to it */
HLE_RANGE(0x004E80, script_op_say_pose_back, 106, HLE_EXIT_SP(0x004EA4, 2))
{
	set_w(&D(0), pop16(ctx));
	RAM16(RAM_SHAGGY_FACING) = (uint16_t)D(0);
	set_w(&D(0), cc_add(ctx, D(0), D(0), 16));
	set_w(&D(1), lslw(ctx, D(1), 3));
	set_w(&D(0), cc_add(ctx, D(0), D(1), 16));
	A(0) = SAY_POSE_TABLE;
	uint16_t anim = ROM16(SAY_POSE_TABLE + (int16_t)D(0));
	RAM16(RAM_ACTOR_ANIM) = anim;
	cc_logic16(ctx, anim);
	bit_set(ctx, RAM_ACTOR_START, 0);
	hle_jump(ctx, 0x004EA4);
}
HLE_RANGE(0x004EA8, script_op_say_back_started, 30, 0x004EA4, 0x004EB4)
{
	hle_jump(ctx, bit_test(ctx, RAM_ACTORS_ANIMDONE, 0) ? 0x004EA4 : 0x004EB4);
}
/* pop D7 and skip the 8-byte op */
static void say_finish(hle_ctx *ctx)
{
	set_w(&D(7), pop16(ctx));
	cc_logic16(ctx, D(7));
	A(5) += 8;
	hle_jump(ctx, 0x004EE2);
}
HLE_RANGE(0x004EB8, script_op_say_back_done, 30, 0x004EB4, HLE_EXIT_SP(0x004EE2, 2))
{
	if (!bit_test(ctx, RAM_ACTORS_ANIMDONE, 0)) {
		hle_jump(ctx, 0x004EB4);
		return;
	}
	set_w(&D(0), RAM16(RAM_SHAGGY_FACING));
	set_w(&D(0), cc_add(ctx, D(0), 4, 16));
	RAM16(RAM_ACTOR_ANIM) = (uint16_t)D(0);
	cc_logic16(ctx, D(0));
	bit_set(ctx, RAM_ACTOR_START, 0);
	say_finish(ctx);
}
HLE_RANGE(0x004EDC, script_op_say_done, 20, HLE_EXIT_SP(0x004EE2, 2)) { say_finish(ctx); }

/* ------------------------------------------------------------------ blocking sub-ops */
/* Sub-ops that wait: ranges in front of each blocking call (actor_animate $00B0B8,
 * wait_vblank, palette_read_cram, bar_show...), the pieces after the calls finish them.
 * The VBlank spins (`tst.b (ram) / bne self`) stay 68k (docs/porting.md). */
#define RAM_09DC_FLAGS     0xFF09DC
#define RAM_ACTORS_ACTIVE  0xFF0AB4
#define RAM_ACTORS_LOOPED  0xFF0ABD
#define RAM_SHAGGY_X       0xFF04DC
#define RAM_SHAGGY_Y       0xFF04F4
#define RAM_PAN_STEP       0xFF08B4   /* word: camera pan speed (+-2, +-8 fast) */
#define RAM_PAN_END        0xFF08B6   /* word: camera x where the pan stops */
#define RAM_CAMERA_X       0xFF06CA
#define RAM_MAP_WIDTH      0xFF06C6
#define SOUND_PLAY         0x009762

/* rng_next ($0022D6) with D0 = range: D1 = new state, D0 = (range * low word) >> 16 */
static void rng_call(hle_ctx *ctx, uint16_t range)
{
	uint32_t s = RAM32(0xFF0428);
	s = ~((s << 1) | (s >> 31));
	RAM32_W(0xFF0428, s);
	D(1) = s;
	uint32_t p = (uint32_t)range * (s & 0xFFFF);
	D(0) = p >> 16;
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, ((p >> 15) & 1 ? CC_X | CC_C : 0) | (D(0) ? 0 : CC_Z));
}

/* $004374 sub $13 sfx_random_a / $004390 sub $14 sfx_random_b: a random exclamation,
 * sound $25 or $27 (Scooby) / $28 or $2A (Shaggy) - rng range 2 never gives the middle
 * one; tail jump to sound_play */
static void sfx_random(hle_ctx *ctx, uint16_t first)
{
	rng_call(ctx, 2);
	cc_logic16(ctx, D(0));
	if ((uint16_t)D(0)) {
		set_w(&D(0), cc_add(ctx, D(0), first + 1, 16));
	} else {
		D(0) = first;
		cc_logic32(ctx, first);
	}
	hle_call68k(ctx, SOUND_PLAY);                      /* bra sound_play */
}
HLE_FUNC(0x004374, script_sys_sfx_random_a, 120) { sfx_random(ctx, 0x25); }
HLE_FUNC(0x004390, script_sys_sfx_random_b, 120) { sfx_random(ctx, 0x28); }

/* $00454E sub $03 fade_out: fade to black (palette_read_cram, blocking) with the frame work
 * of vblank_main paused; VBLANK_FLAGS is saved on the stack around it */
HLE_RANGE(0x00454E, script_sys_fade_out, 40, HLE_EXIT_SP(0x00455C, -2))
{
	A(7) -= 2;
	bus_w8(A(7), RAM8(RAM_09DC_FLAGS));               /* move.b (vblank_flags),-(sp) */
	cc_logic8(ctx, RAM8(RAM_09DC_FLAGS));
	bit_clr(ctx, RAM_09DC_FLAGS, 6);
	hle_jump(ctx, 0x00455C);
}
HLE_RANGE(0x004560, script_sys_fade_out_done, 24, HLE_EXIT_SP(0x004566, 2))
{
	uint8_t v = bus_r8(A(7));
	A(7) += 2;
	RAM8(RAM_09DC_FLAGS) = v;
	cc_logic8(ctx, v);
	hle_jump(ctx, 0x004566);
}

/* $0043C0 sub $0F camera_pan_right / $004428 sub $10 camera_pan_left: lock the camera
 * ($FF09E8 bit 1), then let VBlank scroll it (2 px per frame, 8 with $FF09E9 bit 4) to the
 * room's right edge (map width - 32 cells) / to x = 0, one actor frame at a time until
 * $FF09E9 bit 5 is cleared. Nothing to do if the camera is already there. */
#define PAN_R_FRAME 0x004418
#define PAN_R_RTS   0x004426
#define PAN_L_FRAME 0x004470
#define PAN_L_RTS   0x004480
HLE_RANGE(0x0043C0, script_sys_camera_pan_right, 190, PAN_R_FRAME, PAN_R_RTS)
{
	bit_set(ctx, 0xFF09E8, 1);
	bit_clr(ctx, 0xFF06CB, 0);
	set_w(&D(0), RAM16(RAM_CAMERA_X));
	set_w(&D(0), cc_add(ctx, D(0), 8, 16));
	set_w(&D(0), D(0) & 0xFFF8);
	set_w(&D(1), RAM16(RAM_MAP_WIDTH));
	set_w(&D(1), cc_sub(ctx, D(1), 32, 16, true));
	set_w(&D(1), lslw(ctx, D(1), 3));
	cc_sub(ctx, D(0), D(1), 16, false);                /* cmp.w d1,d0 */
	if ((int16_t)D(0) >= (int16_t)D(1)) {
		hle_jump(ctx, PAN_R_RTS);
		return;
	}
	RAM16(RAM_PAN_STEP) = 2;
	if (bit_test(ctx, 0xFF09E9, 4))
		RAM16(RAM_PAN_STEP) = 8;
	RAM16(RAM_PAN_END) = (uint16_t)D(1);
	cc_logic16(ctx, D(1));
	bit_set(ctx, 0xFF09E9, 5);
	hle_jump(ctx, PAN_R_FRAME);
}
HLE_RANGE(0x00441C, script_sys_camera_pan_right_wait, 30, PAN_R_FRAME, PAN_R_RTS)
{
	hle_jump(ctx, bit_test(ctx, 0xFF09E9, 5) ? PAN_R_FRAME : PAN_R_RTS);
}
HLE_RANGE(0x004428, script_sys_camera_pan_left, 120, PAN_L_FRAME, PAN_L_RTS)
{
	bit_set(ctx, 0xFF09E8, 1);
	bit_clr(ctx, 0xFF06CB, 0);
	set_w(&D(0), RAM16(RAM_CAMERA_X));
	set_w(&D(0), D(0) & 0xFFFE);
	cc_logic16(ctx, D(0));
	if (!(uint16_t)D(0)) {
		hle_jump(ctx, PAN_L_RTS);
		return;
	}
	RAM16(RAM_PAN_END) = 0;
	RAM16(RAM_PAN_STEP) = 0xFFFE;
	cc_logic16(ctx, 0xFFFE);
	if (bit_test(ctx, 0xFF09E9, 4)) {
		RAM16(RAM_PAN_STEP) = 0xFFF8;
		cc_logic16(ctx, 0xFFF8);
	}
	bit_set(ctx, 0xFF09E9, 5);
	hle_jump(ctx, PAN_L_FRAME);
}
HLE_RANGE(0x004474, script_sys_camera_pan_left_wait, 30, PAN_L_FRAME, PAN_L_RTS)
{
	hle_jump(ctx, bit_test(ctx, 0xFF09E9, 5) ? PAN_L_FRAME : PAN_L_RTS);
}

/* $00481A sub $06 bar_show_if_hidden: bar_show ($004862, blocking) if $FF2A00 bit 1 says
 * the bar was hidden */
HLE_RANGE(0x00481A, script_sys_bar_show_if_hidden, 30, 0x004826, 0x004828)
{
	hle_jump(ctx, bit_test(ctx, 0xFF2A00, 1) ? 0x004828 : 0x004826);
}

/* $0042A2 sub $17 overlay_a_on / $0042E0 sub $18 overlay_b_on: upload the overlay sprite
 * tiles (vram_write_words to VRAM $FF00) and switch the sprite overlay effect on
 * ($FF08A4 sprite attributes, $FF08A6 table $0323BE, $FF09E8 bits 4/5) */
HLE_FUNC(0x0042A2, script_sys_overlay_a_on, 200)
{
	RAM16(0xFF08A4) = 0x07F8;
	set_w(&D(0), 0xFF00);
	set_w(&D(1), 0x0080);
	A(0) = 0x0321BE;
	hle_call68k(ctx, 0x00992C);                        /* vram_write_words */
	RAM32_W(0xFF08A6, 0x0323BE);
	RAM8(0xFF0ACE) = 7;
	cc_logic8(ctx, 7);
	bit_set(ctx, 0xFF09E8, 5);
	bit_set(ctx, 0xFF09E8, 4);
}
HLE_FUNC(0x0042E0, script_sys_overlay_b_on, 200)
{
	RAM16(0xFF08A4) = 0x47F8;
	set_w(&D(0), 0xFF00);
	set_w(&D(1), 0x0040);
	A(0) = 0x0322BE;
	hle_call68k(ctx, 0x00992C);
	set_w(&D(0), cc_add(ctx, D(0), 0x80, 16));
	A(0) -= 0x80;
	hle_call68k(ctx, 0x00992C);
	RAM32_W(0xFF08A6, 0x0323BE);
	cc_logic32(ctx, 0x0323BE);
	bit_set(ctx, 0xFF09E8, 5);
	bit_set(ctx, 0xFF09E8, 4);
}

/* $00432C sub $15 follow_mode_on: (after `bsr actor_animate` until Shaggy's animation is
 * done) the first free actor slot 2-5 goes to $FF08B0, $FF09E8 bit 3 on, $FF0AC6 = 0, then
 * wait_vblank. The entry is the bsr itself (68k). */
HLE_RANGE(0x004330, script_sys_follow_mode_on, 80, 0x00432C, 0x004364)
{
	if (!bit_test(ctx, RAM_ACTORS_ANIMDONE, 0)) {
		hle_jump(ctx, 0x00432C);
		return;
	}
	D(0) = 2;
	while (bit_test(ctx, RAM_ACTORS_ACTIVE, D(0))) {
		set_w(&D(0), cc_add(ctx, D(0), 1, 16));
		if (cmpw(ctx, D(0), 6) == 0)
			break;
	}
	RAM16(0xFF08B0) = (uint16_t)D(0);
	bit_set(ctx, 0xFF09E8, 3);
	RAM8(0xFF0AC6) = 0;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);     /* clr.b */
	hle_jump(ctx, 0x004364);
}

/* $0040FC sub $25 wait_shaggy_path: actor frames until Shaggy's scripted path ended (the
 * entry is the bsr, 68k) */
HLE_RANGE(0x004100, script_sys_wait_shaggy_path, 30, 0x0040FC, 0x00410A)
{
	hle_jump(ctx, bit_test(ctx, RAM_ACTORS_ON_PATH, 0) ? 0x0040FC : 0x00410A);
}

/* $004120 sub $22 shaggy_anim_by_x: Shaggy plays animation $49 (x < 276) or $4A, actor
 * frames until it looped */
HLE_RANGE(0x004120, script_sys_shaggy_anim_by_x, 60, 0x004148)
{
	set_w(&D(1), 0x49);
	set_w(&D(0), RAM16(RAM_SHAGGY_X));
	if (cmpw(ctx, D(0), 0x114) >= 0)
		set_w(&D(1), cc_add(ctx, D(1), 1, 16));
	RAM16(RAM_ACTOR_ANIM) = (uint16_t)D(1);
	RAM16(0xFF0618) = 0xFFFF;
	cc_logic16(ctx, 0xFFFF);
	bit_set(ctx, RAM_ACTOR_START, 0);
	hle_jump(ctx, 0x004148);
}
HLE_RANGE(0x00414C, script_sys_shaggy_anim_by_x_wait, 30, 0x004148, 0x004158)
{
	hle_jump(ctx, bit_test(ctx, RAM_ACTORS_LOOPED, 0) ? 0x004158 : 0x004148);
}

/* $00415A sub $21 wait_dpad_choice: actor frames until Left / Right / Down is held (pad
 * bits 2 / 3 / 1, active low): object 0's var = 0 / 1 / 2, $FF0AC3 bit 0 (the entry is
 * the bsr, 68k) */
HLE_RANGE(0x00415E, script_sys_wait_dpad_choice, 60, 0x00415A, 0x0041A0)
{
	int v;
	if (!bit_test(ctx, PAD1_CUR, 2))
		v = 0;
	else if (!bit_test(ctx, PAD1_CUR, 3))
		v = 1;
	else if (!bit_test(ctx, PAD1_CUR, 1))
		v = 2;
	else {
		hle_jump(ctx, 0x00415A);
		return;
	}
	RAM16(RAM_OBJECTS + 0x0C) = (uint16_t)v;
	cc_logic16(ctx, v);
	bit_set(ctx, RAM_ACTORS_PATHONCE, 0);
	hle_jump(ctx, 0x0041A0);
}

/* $003ED2 sub $2B shaggy_alt_sprite_on: Shaggy's other sprite set ($0B5D02) without depth
 * scaling, animation $1E until it looped, then the script-running bit of VBLANK_FLAGS off */
HLE_RANGE(0x003ED2, script_sys_shaggy_alt_sprite_on, 100, 0x003EFC)
{
	bit_set(ctx, 0xFF09E9, 4);
	bit_clr(ctx, 0xFF0AB8, 0);
	RAM32_W(0xFF0470, 0x0B5D02);
	RAM16(RAM_ACTOR_ANIM) = 0x1E;
	cc_logic16(ctx, 0x1E);
	bit_set(ctx, RAM_ACTOR_START, 0);
	hle_jump(ctx, 0x003EFC);
}
HLE_RANGE(0x003F00, script_sys_shaggy_alt_sprite_on_wait, 40, 0x003EFC, 0x003F14)
{
	if (!bit_test(ctx, RAM_ACTORS_LOOPED, 0)) {
		hle_jump(ctx, 0x003EFC);
		return;
	}
	bit_clr(ctx, RAM_09DC_FLAGS, 7);
	hle_jump(ctx, 0x003F14);
}

/* $00301E sub $35 shaggy_pose_45 / $0030A6 sub $37 shaggy_pose_47: Shaggy plays animation
 * $45 / $47 at a fixed spot ((296,64) / (88,80)); once it is done (and Scooby's path, unless
 * Scooby is away) actor 2 gets animation 0 / 1 (4 / 5 if Scooby is away), actors 0-1 are
 * frozen. The wait for the sprite DMA (`btst #0,($FF0AB6) / bne self`) stays 68k.
 * Both handlers have the same layout (offsets from the entry): +$08 spin, +$12 bset,
 * +$1A jsr (start), +$20 place, +$30 jsr (wait), +$36 test, +$78 jsr (pose), +$7E, +$86 rts */
typedef struct { uint32_t base; uint16_t anim, x, y, pose; } shaggy_pose;
static const shaggy_pose POSE45 = { 0x00301E, 0x45, 296, 64, 0 };
static const shaggy_pose POSE47 __attribute__((unused)) = { 0x0030A6, 0x47, 88, 80, 1 };
static void pose_set(hle_ctx *ctx, const shaggy_pose *p)
{
	RAM16(RAM_ACTOR_ANIM) = p->anim;
	cc_logic16(ctx, p->anim);
	hle_jump(ctx, p->base + 0x08);                     /* the spin */
}
static void pose_go(hle_ctx *ctx, const shaggy_pose *p)
{
	bit_set(ctx, RAM_ACTOR_START, 0);
	hle_jump(ctx, p->base + 0x1A);
}
static void pose_place(hle_ctx *ctx, const shaggy_pose *p)
{
	RAM16(RAM_SHAGGY_X) = p->x;
	RAM16(RAM_SHAGGY_Y) = p->y;
	cc_logic16(ctx, p->y);
	hle_jump(ctx, p->base + 0x30);
}
static void pose_wait(hle_ctx *ctx, const shaggy_pose *p)
{
	if (!bit_test(ctx, RAM_ACTORS_ANIMDONE, 0) ||
	    (!bit_test(ctx, 0xFF09DE, 7) && bit_test(ctx, RAM_ACTORS_ON_PATH, 1))) {
		hle_jump(ctx, p->base + 0x30);
		return;
	}
	uint16_t a = bit_test(ctx, 0xFF09DE, 7) ? 4 + p->pose : p->pose;
	RAM16(0xFF048C) = a;
	cc_logic16(ctx, a);
	bit_set(ctx, RAM_ACTOR_START, 2);
	hle_jump(ctx, p->base + 0x78);
}
static void pose_done(hle_ctx *ctx, const shaggy_pose *p)
{
	RAM8(0xFF0ABF) |= 3;
	cc_logic8(ctx, RAM8(0xFF0ABF));
	hle_jump(ctx, p->base + 0x86);
}
#define POSE_RANGES(P, N)                                                                  \
	HLE_RANGE(0x00301E + (N), script_sys_shaggy_pose_##P, 30, 0x003026 + (N))            \
	{ pose_set(ctx, &POSE##P); }                                                          \
	HLE_RANGE(0x003030 + (N), script_sys_shaggy_pose_##P##_go, 30, 0x003038 + (N))       \
	{ pose_go(ctx, &POSE##P); }                                                           \
	HLE_RANGE(0x00303E + (N), script_sys_shaggy_pose_##P##_place, 30, 0x00304E + (N))    \
	{ pose_place(ctx, &POSE##P); }                                                        \
	HLE_RANGE(0x003054 + (N), script_sys_shaggy_pose_##P##_wait, 60, 0x00304E + (N),    \
	          0x003096 + (N))                                                             \
	{ pose_wait(ctx, &POSE##P); }                                                         \
	HLE_RANGE(0x00309C + (N), script_sys_shaggy_pose_##P##_done, 30, 0x0030A4 + (N))     \
	{ pose_done(ctx, &POSE##P); }
POSE_RANGES(45, 0)
#ifdef SCRIPT_UNTESTED   /* sub $37 occurs nowhere in the script data */
POSE_RANGES(47, 0x88)
#endif

/* $0044C2 sub $07 closeup_picture (after the 68k spin `tst.b ($FF0AB6) / bne` at $0044BA
 * that waits for the sprite DMA): verb state bit 6; decompress the close-up tiles
 * ($031DFA) and DMA them to the UI tile base ($FF068A & $7FF); clear plane A ($C000, $800
 * words of tile base + 1) and draw the 11 x 11 picture map ($0320CC) at its top left. */
HLE_RANGE(0x0044C2, script_sys_closeup_picture, 400, 0x004532)
{
	bit_set(ctx, 0xFF09DE, 6);
	A(0) = 0x031DFA;
	A(1) = 0xFF3000;
	A(2) = 0xFF4000;
	hle_call68k(ctx, 0x0097AA);                        /* lzss_decompress */
	A(0) = A(2);
	D(1) = RAM32(A(1));                                /* unpacked size */
	set_w(&D(1), lsrw(ctx, D(1), 1));
	set_w(&D(0), RAM16(0xFF068A));
	set_w(&D(0), D(0) & 0x07FF);
	set_w(&D(0), lslw(ctx, D(0), 5));
	hle_call68k(ctx, 0x0099F6);                        /* vdp_dma_vram */
	set_w(&D(2), RAM16(0xFF068A));
	set_w(&D(2), cc_add(ctx, D(2), 1, 16));
	set_w(&D(1), 0x0800);
	set_w(&D(0), 0xC000);
	hle_call68k(ctx, 0x009BE4);                        /* vram_fill_words: plane A */
	A(0) = 0x0320CC;
	set_w(&D(2), 10);
	do {
		set_w(&D(3), 10);
		do {
			set_w(&D(1), bus_r16(A(0)));
			A(0) += 2;
			set_w(&D(1), cc_add(ctx, D(1), RAM16(0xFF068A), 16));
			hle_call68k(ctx, 0x009964);                /* vram_write_word */
			set_w(&D(0), cc_add(ctx, D(0), 2, 16));
			set_w(&D(3), D(3) - 1);
		} while ((uint16_t)D(3) != 0xFFFF);
		set_w(&D(0), cc_add(ctx, D(0), 0x6A, 16));
		set_w(&D(2), D(2) - 1);
	} while ((uint16_t)D(2) != 0xFFFF);
	hle_jump(ctx, 0x004532);
}
