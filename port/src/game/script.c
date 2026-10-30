/* Adventure script interpreter (GitLab #19; byte code: docs/script.md, status:
 * docs/subsystems/script.md).
 *
 * Scripts are word byte code in ROM. A5 = script pc, A6 = end. The two dispatch loops
 * (script_run = scan mode, script_run_frame = execute mode) call one handler per op through
 * script_op_table; the handler advances A5 itself. Handlers learn the mode from the Z flag
 * the dispatcher's `moveq #0/#1,d0` left behind: Z set = scan (only the block ops case /
 * talk_choice / on_event / if react), Z clear = execute.
 *
 * Most execute-mode ops wait for VBlank-driven state (an actor animation, a walk, a
 * countdown). Following docs/porting.md ("Waiting for VBlank"), those waits stay in 68k:
 *   - the dispatch loops are HLE_RANGEs that stop in front of `jsr (a0)` (the handler call),
 *     so a blocking handler is entered by the original jsr and returns into the next range;
 *   - non-blocking handlers are plain HLE_FUNCs;
 *   - blocking handlers are split into HLE_RANGEs in front of their blocking calls.
 * In a native build without the CPU core the ranges become one C loop and each blocking
 * handler a coroutine that yields at the marked points.
 *
 * Callees of other subsystems (sound, VDP, verb bar) are called through hle_call68k.
 */
#include "game.h"
#include "cc68k.h"

/* ---- RAM ---- */
#define RAM_MYSTERY_HDR     0xFF068E   /* long: active mystery header (ROM) */
#define RAM_OBJECTS         0xFF1200   /* object records, 26 bytes; script "who" n >= 3 = record n-3 */
#ifndef OBJ_SIZE
#define OBJ_SIZE            26
#endif
#define   OBJ_ROOM            0x06
#define   OBJ_HOTSPOT         0x02
#define   OBJ_VERBS           0x16       /* default verb bytes */
#define   OBJ_FLAGS           0x18       /* byte: bit0 redraw, bit1 actor, bit2 event pending */
#define   OBJ_ACTOR           0x19       /* byte: actor slot - 2, $FF = none */
#define RAM_GAME_STATE      0xFF2A00   /* story flags: Fxx.b = bit b of byte xx */
#define RAM_ROOM_INDEX      0xFF06AC   /* word: current room */
#define RAM_TARGET_OBJ      0xFF06AE   /* word: object under the pointer (who numbering) */
#define RAM_TARGET2_OBJ     0xFF06B6   /* word: second object of USE/GIVE */
#define RAM_VERB_CUR        0xFF06C0   /* word: selected verb */
#define RAM_VERB_REDRAW     0xFF06C2   /* word: verb whose label verb_label_redraw repaints */
#define RAM_VERB_STATE      0xFF09DE   /* byte: bit0 waiting for the second object */
#define RAM_09E8            0xFF09E8   /* byte: bit0 "give" (status line says "to"), ... */
#define RAM_09E9            0xFF09E9
#define RAM_09EB            0xFF09EB
#define RAM_SCRIPT_CB       0xFF0878   /* long: script_run or script_run_frame (nested blocks) */
#define RAM_TALK_COUNT      0xFF080E   /* word: talk_choice ops collected (max 5) */
#define RAM_TALK_POSE       0xFF0824   /* 5 words */
#define RAM_TALK_LEN        0xFF082E   /* 5 words: body length */
#define RAM_TALK_TEXT       0xFF0838   /* 5 longs: choice text */
#define RAM_TALK_REPLY      0xFF084C   /* 5 longs: reply text (= TALK_TEXT + $14) */
#define RAM_TALK_BODY       0xFF0860   /* 5 longs: body address (= TALK_TEXT + $28) */
#define RAM_SCRIPT_FLAGS    0xFF0AC9   /* byte, SF_* */
#define   SF_STOP             0x01
#define   SF_MATCH_CASE       0x02
#define   SF_COLLECT_TALK     0x04
#define   SF_MATCH_EVENT      0x10
#define   SF_NEED_SECOND      0x20
#define RAM_ACTORS_MOVING   0xFF0AB7   /* bit n: actor n walking to a target */
#define RAM_ACTORS_ANIMDONE 0xFF0ABC
#define RAM_ACTORS_FROZEN   0xFF0ABF   /* bit n: actor n frozen by a script (op $16) */
#define RAM_ACTORS_ON_PATH  0xFF0AC0
#define RAM_ACTORS_PATHONCE 0xFF0AC3
#define RAM_ACTOR_PATH_PTR  0xFF05F4   /* 6 longs */
#define RAM_ACTOR_PATH_POS  0xFF060C   /* 6 words */
#define RAM_ACTOR_PATH_DLY  0xFF0AA5   /* 6 bytes delay, +6: reload */
#define RAM_PALCYC          0xFF087C   /* palette cycling: 5 words first, +$A last, +$14 timer, +$1E speed */
#define RAM_PALCYC_ON       0xFF0ACD   /* byte: bit n = cycling slot n on */
#define RAM_PAL_TARGET      0xFF0778
#define RAM_RNG_STATE       0xFF0428

/* ---- ROM ---- */
#define SCRIPT_OP_TABLE     0x002354   /* 34 longs */
#define SUBOP_TABLE         0x002EB4   /* 62 longs */
#define SOUND_PLAY          0x009762
#define SOUND_STOP          0x009776
#define VDP_DMA_CRAM        0x009B10
#define VERB_LABEL_REDRAW   0x005BDC

/* ------------------------------------------------------------------ helpers */

static inline uint16_t opw(hle_ctx *c, int off) { return bus_r16(c->a[5] + off); }
static inline uint32_t opl(hle_ctx *c, int off) { return bus_r32(c->a[5] + off); }
static inline bool exec_mode(const hle_ctx *c) { return !(c->sr & CC_Z); }

/* "move.w off(a5),d0 / lea (0,a5,d0.w),a5": skip a block whose length is at off */
static void skip_by_word(hle_ctx *c, int off)
{
	uint16_t len = opw(c, off);
	set_w(&c->d[0], len);
	cc_logic16(c, len);
	c->a[5] += (uint32_t)(int32_t)(int16_t)len;
}

/* "subq.w #3,dN / mulu.w #26,dN": object record offset of who >= 3 (32-bit product;
 * callers index with its low word, sign-extended, as (0,a0,dN.w) does). */
static uint32_t obj_mul(hle_ctx *c, uint32_t reg, int sub)
{
	uint16_t w = (uint16_t)cc_sub(c, reg, (uint32_t)sub, 16, true);   /* subq sets X */
	uint32_t p = (uint32_t)w * OBJ_SIZE;
	cc_logic32(c, p);                    /* mulu: N Z, V=C=0 */
	return p;
}
static inline uint32_t obj_addr(uint32_t prod, int field)
{
	return RAM_OBJECTS + (uint32_t)(int32_t)(int16_t)prod + field;
}

/* "btst.l dN,(mem)" / bset / bclr on a byte: bit number mod 8, Z = !old bit */
static bool bit_test(hle_ctx *c, uint32_t addr, uint32_t bit)
{
	uint8_t v = RAM8(addr);
	cc_btst(c, v, bit & 7);
	return (v >> (bit & 7)) & 1;
}
static void bit_set(hle_ctx *c, uint32_t addr, uint32_t bit) { bit_test(c, addr, bit); RAM8(addr) |= (uint8_t)(1u << (bit & 7)); }
static void bit_clr(hle_ctx *c, uint32_t addr, uint32_t bit) { bit_test(c, addr, bit); RAM8(addr) &= (uint8_t)~(1u << (bit & 7)); }

/* ------------------------------------------------------------------ dispatch loops */
/* $0023DC script_run (scan mode, also the head of its loop):
 *   cmpa.l a5,a6 / ble rts / move.w (a5),d0 / lsl.w #2,d0 / lea op_table,a0 /
 *   movea.l (0,a0,d0.w),a0 / moveq #0,d0 / jsr (a0)            <- exit $0023F2 */
HLE_RANGE(0x0023DC, script_run, 40, 0x0023F2, 0x002404)
{
	cc_sub(ctx, ctx->a[6], ctx->a[5], 32, false);
	if ((int32_t)ctx->a[6] <= (int32_t)ctx->a[5]) {       /* ble: script finished */
		hle_jump(ctx, 0x002404);
		return;
	}
	uint16_t op = bus_r16(ctx->a[5]);
	uint32_t idx = lslw(ctx, op, 2);                       /* lsl.w sets X */
	ctx->a[0] = bus_r32(SCRIPT_OP_TABLE + (int16_t)idx);
	ctx->d[0] = 0;                       /* moveq #0,d0: Z set = scan mode */
	cc_logic32(ctx, 0);
	hle_jump(ctx, 0x0023F2);
}

/* $0023F4 (after the handler): btst #0,(script_flags) / bne rts / bra script_run */
HLE_RANGE(0x0023F4, script_run_next, 12, 0x0023DC, 0x002404)
{
	bool stop = bit_test(ctx, RAM_SCRIPT_FLAGS, 0);
	hle_jump(ctx, stop ? 0x002404 : 0x0023DC);
}

/* $002406 script_run_frame (execute mode):
 *   if (flags2 & 2) && (flags2 & 1): rts         Start skips a skippable cutscene
 *   cmpa.l a5,a6 / ble rts / fetch handler / moveq #1,d0 / jsr (a0)   <- exit $002430 */
HLE_RANGE(0x002406, script_run_frame, 48, 0x002430, 0x002444)
{
	uint8_t f2 = RAM8(0xFF0ACA);
	cc_btst(ctx, f2, 1);
	if (f2 & 2) {
		cc_btst(ctx, f2, 0);
		if (f2 & 1) {
			hle_jump(ctx, 0x002444);
			return;
		}
	}
	cc_sub(ctx, ctx->a[6], ctx->a[5], 32, false);
	if ((int32_t)ctx->a[6] <= (int32_t)ctx->a[5]) {
		hle_jump(ctx, 0x002444);
		return;
	}
	uint16_t op = bus_r16(ctx->a[5]);
	uint32_t idx = lslw(ctx, op, 2);
	ctx->a[0] = bus_r32(SCRIPT_OP_TABLE + (int16_t)idx);
	ctx->d[0] = 1;                       /* moveq #1,d0: Z clear = execute mode */
	cc_logic32(ctx, 1);
	hle_jump(ctx, 0x002430);
}

/* $002432: btst #0,(script_flags) / bne rts / cmpa.l a6,a5 / blt script_run_frame / rts */
HLE_RANGE(0x002432, script_run_frame_next, 16, 0x002406, 0x002444)
{
	if (bit_test(ctx, RAM_SCRIPT_FLAGS, 0)) {
		hle_jump(ctx, 0x002444);
		return;
	}
	cc_sub(ctx, ctx->a[5], ctx->a[6], 32, false);
	hle_jump(ctx, (int32_t)ctx->a[5] < (int32_t)ctx->a[6] ? 0x002406 : 0x002444);
}

/* ------------------------------------------------------------------ block ops */

/* op $00 nop ($00250C, tst.w (a5)+) is never executed by the data: not replaced. */

/* $002446 op $13 on_event [13][len] body: in an event scan (script_flags bit 4) stop here;
 * otherwise skip the block. */
HLE_FUNC(0x002446, script_op_on_event, 40)
{
	if (!exec_mode(ctx) && bit_test(ctx, RAM_SCRIPT_FLAGS, 4)) {
		bit_set(ctx, RAM_SCRIPT_FLAGS, 0);     /* match: run_object_events runs the body */
		return;
	}
	skip_by_word(ctx, 2);
}

/* $00246A op $01 case_verb [01][verb][with][len] body: in a verb scan (script_flags bit 1)
 * stop here when D7 is the verb (and, for USE/GIVE "with X", the second object is X);
 * otherwise skip the block. */
HLE_FUNC(0x00246A, script_op_case_verb, 80)
{
	uint16_t verb = (uint16_t)ctx->d[7];
	if (exec_mode(ctx) || !bit_test(ctx, RAM_SCRIPT_FLAGS, 1))
		goto skip;
	if (cmpw(ctx, verb, opw(ctx, 2)) != 0)
		goto skip;
	if (cmpw(ctx, verb, 5) == 0) {                         /* USE: "with" */
		bit_clr(ctx, RAM_09E8, 0);
	} else {
		if (cmpw(ctx, verb, 6) != 0)                       /* not GIVE: match */
			goto match;
		bit_set(ctx, RAM_09E8, 0);                         /* GIVE: "to" */
	}
	cc_logic16(ctx, opw(ctx, 4));
	if (opw(ctx, 4) == 0)                                  /* no second object needed */
		goto match;
	cc_logic16(ctx, RAM16(RAM_TARGET2_OBJ));
	if (RAM16(RAM_TARGET2_OBJ) == 0) {
		/* second object not chosen yet: ask for it, keep scanning */
		bit_set(ctx, RAM_SCRIPT_FLAGS, 5);
		if (!bit_test(ctx, RAM_VERB_STATE, 0)) {
			bit_set(ctx, RAM_VERB_STATE, 0);
			RAM16(RAM_TARGET2_OBJ) = 0;
			cc_logic16(ctx, 0);                            /* clr.w */
			goto skip;
		}
		cc_logic16(ctx, RAM16(RAM_TARGET2_OBJ));           /* tst.w: still 0 */
		goto skip;
	}
	cmpw(ctx, verb, opw(ctx, 2));
	set_w(&ctx->d[0], RAM16(RAM_TARGET2_OBJ));
	if (cmpw(ctx, ctx->d[0], opw(ctx, 4)) != 0)
		goto skip;
match:
	bit_set(ctx, RAM_SCRIPT_FLAGS, 0);
	return;
skip:
	skip_by_word(ctx, 6);
}

/* $0025AC op $02 talk_choice [02][text.l][reply.l][len][pose] body: while collecting
 * (script_flags bit 2, script_run_nested) record up to 5 choices for the TALK menu. */
HLE_FUNC(0x0025AC, script_op_talk_choice, 120)
{
	if (exec_mode(ctx) || !bit_test(ctx, RAM_SCRIPT_FLAGS, 2))
		goto skip;
	ctx->a[0] = RAM32(RAM_MYSTERY_HDR);
	ctx->d[1] = bus_r32(ctx->a[0] + 0x20);                 /* text base */
	set_w(&ctx->d[0], RAM16(RAM_TALK_COUNT));
	if (cmpw(ctx, ctx->d[0], 5) >= 0)                      /* menu full */
		goto skip;
	{
		int16_t n = (int16_t)ctx->d[0];
		ctx->a[0] = RAM_TALK_TEXT;
		ctx->a[1] = RAM_TALK_LEN;
		set_w(&ctx->d[3], (uint16_t)(n * 2));
		set_w(&ctx->d[0], (uint16_t)(n * 4));
		ctx->d[2] = ctx->d[1] + opl(ctx, 2);
		RAM32_W(RAM_TALK_TEXT + 4 * n, ctx->d[2]);
		ctx->d[2] = ctx->d[1] + opl(ctx, 6);
		RAM32_W(RAM_TALK_REPLY + 4 * n, ctx->d[2]);
		RAM16(RAM_TALK_LEN + 2 * n) = (uint16_t)(opw(ctx, 0xA) - 0xE);
		RAM16(RAM_TALK_POSE + 2 * n) = opw(ctx, 0xC);
		ctx->a[1] = ctx->a[5] + 0xE;
		RAM32_W(RAM_TALK_BODY + 4 * n, ctx->a[1]);
		RAM16(RAM_TALK_COUNT) = (uint16_t)cc_add(ctx, RAM16(RAM_TALK_COUNT), 1, 16);
	}
skip:
	skip_by_word(ctx, 0xA);
}

/* $002796 condition of if / if_else: [op][skip][else][a][b][c][d][mode]
 *   lhs = mode&1 ? flag(a,b) : mode&2 ? field(a,b) : a
 *   rhs = mode&4 ? flag(c,d) : mode&8 ? field(c,d) : c
 *   mode&$10 ==, &$20 >, &$40 <, else !=        (signed words)
 * Leaves D0.w = mode, D1/D2 = lhs/rhs, D3 = result, D4, A0 like the original. */
static bool script_cond(hle_ctx *ctx)
{
	uint16_t mode = opw(ctx, 0xE);
	set_w(&ctx->d[0], mode);
	for (int side = 0; side < 2; side++) {
		int o = side ? 0xA : 6;                           /* operand pair */
		uint32_t *dst = side ? &ctx->d[2] : &ctx->d[1];
		unsigned isflag = side ? 4 : 1, isfield = side ? 8 : 2;
		if (mode & isflag) {
			set_w(&ctx->d[3], opw(ctx, o));
			ctx->a[0] = RAM_GAME_STATE;
			set_w(&ctx->d[4], opw(ctx, o + 2));
			*dst = 0;
			if (bit_test(ctx, RAM_GAME_STATE + (int16_t)ctx->d[3], ctx->d[4]))
				*dst = 1;
		} else if (mode & isfield) {
			set_w(&ctx->d[3], opw(ctx, o));
			set_w(&ctx->d[4], opw(ctx, o + 2));
			ctx->a[0] = RAM_OBJECTS;
			if (!side && (uint16_t)ctx->d[4] == 1 && (uint16_t)ctx->d[3] == 6) {
				set_w(dst, RAM16(RAM_ROOM_INDEX));         /* (6, Shaggy) = current room */
				continue;
			}
			ctx->d[4] = obj_mul(ctx, ctx->d[4], 3);
			set_w(&ctx->d[3], cc_add(ctx, ctx->d[3], ctx->d[4], 16) & 0xFFFE);
			set_w(dst, RAM16(RAM_OBJECTS + (int16_t)ctx->d[3]));
		} else {
			set_w(dst, opw(ctx, o));
		}
	}
	int16_t lhs = (int16_t)ctx->d[1], rhs = (int16_t)ctx->d[2];
	bool r;
	if (mode & 0x10)      r = lhs == rhs;
	else if (mode & 0x20) r = lhs > rhs;
	else if (mode & 0x40) r = lhs < rhs;
	else                  r = lhs != rhs;
	ctx->d[3] = r;
	cc_logic16(ctx, r);                                    /* tst.w d3 */
	return r;
}

/* $002732 op $03 if [03][skip][-][cond]: both modes. True: enter the body at +$10;
 * false: skip it. */
HLE_FUNC(0x002732, script_op_if, 150)
{
	if (script_cond(ctx))
		ctx->a[5] += 0x10;
	else
		skip_by_word(ctx, 2);
}

/* ------------------------------------------------------------------ data ops */

/* rng_next ($0022D6) with d0 = range: steps the generator, returns (range * hi) >> 16 */
static uint16_t rng(hle_ctx *c, uint16_t range)
{
	uint32_t s = RAM32(RAM_RNG_STATE);
	s = ~((s << 1) | (s >> 31));
	RAM32_W(RAM_RNG_STATE, s);
	uint32_t p = (uint32_t)range * (s & 0xFFFF);
	cc_set(c, CC_X, (p >> 15) & 1 ? CC_X : 0);           /* lsr.l #8 twice: X = bit 15 */
	return (uint16_t)(p >> 16);
}

/* $002C56 op $08 set [08][dst_a][dst_b][src_a][src_b][mode]:
 *   src = mode&4 flag(src) | mode&8 field(src) | mode&$80 random(src_a) | src_a
 *   mode&1: flag(dst) = src != 0 (mode&$10; without it the original toggles with a buggy
 *   index register - never used by the data, left to the 68k code)
 *   else field(dst) =, -=, |=, += src  (mode bits 4, 5, 6, none) */
HLE_FUNC(0x002C56, script_op_set, 150)
{
	if (!exec_mode(ctx))
		goto done;
	uint16_t mode = opw(ctx, 0xA);
	if ((mode & 0x11) == 0x01) {                          /* buggy toggle: run the original */
		hle_call68k_orig(ctx, 0x002C56);
		return;
	}
	set_w(&ctx->d[0], mode);
	if (mode & 4) {
		set_w(&ctx->d[3], opw(ctx, 6));
		ctx->a[0] = RAM_GAME_STATE;
		set_w(&ctx->d[4], opw(ctx, 8));
		ctx->d[2] = bit_test(ctx, RAM_GAME_STATE + (int16_t)ctx->d[3], ctx->d[4]) ? 1 : 0;
	} else if (mode & 8) {
		set_w(&ctx->d[3], opw(ctx, 6));
		set_w(&ctx->d[4], opw(ctx, 8));
		ctx->a[0] = RAM_OBJECTS;
		ctx->d[4] = obj_mul(ctx, ctx->d[4], 3);
		set_w(&ctx->d[3], cc_add(ctx, ctx->d[3], ctx->d[4], 16) & 0xFFFE);
		set_w(&ctx->d[2], RAM16(RAM_OBJECTS + (int16_t)ctx->d[3]));
	} else {
		set_w(&ctx->d[2], opw(ctx, 6));
		if (mode & 0x80)
			set_w(&ctx->d[2], rng(ctx, (uint16_t)ctx->d[2]));  /* d0/d1 saved around rng_next */
	}
	uint16_t src = (uint16_t)ctx->d[2];
	if (mode & 1) {                                       /* story flag = (src != 0) */
		set_w(&ctx->d[3], opw(ctx, 2));
		ctx->a[0] = RAM_GAME_STATE;
		set_w(&ctx->d[4], opw(ctx, 4));
		cc_logic16(ctx, src);
		uint32_t a = RAM_GAME_STATE + (int16_t)ctx->d[3];
		if (src)
			bit_set(ctx, a, ctx->d[4]);
		else
			bit_clr(ctx, a, ctx->d[4]);
		goto done;
	}
	set_w(&ctx->d[3], opw(ctx, 2));
	set_w(&ctx->d[4], opw(ctx, 4));
	ctx->a[0] = RAM_OBJECTS;
	ctx->d[4] = obj_mul(ctx, ctx->d[4], 3);
	set_w(&ctx->d[3], cc_add(ctx, ctx->d[3], ctx->d[4], 16) & 0xFFFE);
	{
		uint32_t a = RAM_OBJECTS + (int16_t)ctx->d[3];
		uint16_t v = RAM16(a);
		if (mode & 0x10) {
			v = src;
			cc_logic16(ctx, v);
		} else if (mode & 0x20) {
			v = (uint16_t)cc_sub(ctx, v, src, 16, true);
		} else if (mode & 0x40) {
			v |= src;
			cc_logic16(ctx, v);
		} else {
			v = (uint16_t)cc_add(ctx, v, src, 16);
		}
		RAM16(a) = v;
	}
done:
	ctx->a[5] += 0xC;
}

/* $002D5E op $09 set_verb [09][who][byte verb, byte slot]: object default verb byte
 * +$16+slot = verb. If the object is hovered its old default verb label is repainted in
 * the normal colour; a selected verb is deselected (only ram_verb_redraw is set, the
 * original does not repaint it). */
HLE_FUNC(0x002D5E, script_op_set_verb, 200)
{
	if (exec_mode(ctx)) {
		uint16_t who = opw(ctx, 2);
		set_w(&ctx->d[2], who);
		ctx->d[0] = obj_mul(ctx, who, 3);
		ctx->a[0] = RAM_OBJECTS;
		int8_t slot = (int8_t)(opw(ctx, 4) & 0xFF);
		set_w(&ctx->d[1], (uint16_t)(int16_t)slot);
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], ctx->d[1], 16));
		ctx->a[1] = RAM_OBJECTS + OBJ_VERBS + (int16_t)ctx->d[0];
		if (cmpw(ctx, who, RAM16(RAM_TARGET_OBJ)) == 0) {
			int8_t old = (int8_t)RAM8(ctx->a[1]);
			set_w(&ctx->d[0], (uint16_t)(int16_t)old);
			RAM16(RAM_VERB_REDRAW) = (uint16_t)(int16_t)old;
			ctx->d[0] = 0;
			hle_call68k(ctx, VERB_LABEL_REDRAW);
		}
		RAM8(ctx->a[1]) = (uint8_t)(opw(ctx, 4) >> 8);
		cc_logic16(ctx, RAM16(RAM_VERB_CUR));
		if (RAM16(RAM_VERB_CUR)) {
			ctx->d[0] = 0;
			RAM16(RAM_VERB_REDRAW) = RAM16(RAM_VERB_CUR);
			RAM16(RAM_VERB_CUR) = 0;
			cc_logic16(ctx, 0);
		}
	}
	ctx->a[5] += 6;
}

/* $0048B6 op $0D sound [0D][id]: play id, or stop id&$7FFF when bit 15 is set */
HLE_FUNC(0x0048B6, script_op_sound, 60)
{
	if (exec_mode(ctx)) {
		uint16_t id = opw(ctx, 2);
		if (id & 0x8000) {
			ctx->d[0] = (uint32_t)(int32_t)(int16_t)(id & 0x7FFF);
			hle_call68k(ctx, SOUND_STOP);
		} else {
			ctx->d[0] = (uint32_t)(int32_t)(int16_t)id;
			hle_call68k(ctx, SOUND_PLAY);
		}
	}
	ctx->a[5] += 4;
}

/* $004F50 op $12 / $002E7E op $14 set_hotspot [op][who][id]: object +$02 = id */
static void set_hotspot(hle_ctx *ctx)
{
	if (exec_mode(ctx)) {
		ctx->d[0] = obj_mul(ctx, opw(ctx, 2), 3);
		ctx->a[0] = RAM_OBJECTS;
		uint16_t id = opw(ctx, 4);
		RAM16(obj_addr(ctx->d[0], OBJ_HOTSPOT)) = id;
		cc_logic16(ctx, id);
	}
	ctx->a[5] += 6;
}
/* $004F50 (op $12) is the same code as $002E7E but is never executed on any route, so it
 * has no difftest states: not registered (build with -DSCRIPT_UNTESTED to enable). */
#ifdef SCRIPT_UNTESTED
HLE_FUNC(0x004F50, script_op_set_hotspot, 70) { set_hotspot(ctx); }
#endif
HLE_FUNC(0x002E7E, script_op_set_hotspot2, 70) { set_hotspot(ctx); }

/* $002AD0 op $16 freeze [16][who]: set the actor's bit in $FF0ABF with interrupts off.
 * Leaves SR = $2000 (move.w #$2000,sr). */
HLE_FUNC(0x002AD0, script_op_freeze, 80)
{
	if (exec_mode(ctx)) {
		uint32_t d0 = (ctx->d[0] & 0xFFFF0000u) | opw(ctx, 2);
		uint16_t w = (uint16_t)(d0 - 1);
		if (w == 0) {
			d0 = (d0 & 0xFFFF0000u) | w;
		} else if ((uint16_t)(w - 1) == 0) {
			d0 = 1;
		} else {
			d0 = (uint32_t)(uint16_t)(w - 2) * OBJ_SIZE;
			ctx->a[0] = RAM_OBJECTS;
			uint8_t slot = RAM8(RAM_OBJECTS + (int16_t)d0 + OBJ_ACTOR);
			d0 = (d0 & 0xFFFFFF00u) | slot;
			d0 = (d0 & 0xFFFF0000u) | (uint16_t)(d0 + 2);
		}
		ctx->d[0] = d0;
		RAM8(RAM_ACTORS_FROZEN) |= (uint8_t)(1u << (d0 & 7));
		ctx->sr = 0x2000;
	}
	ctx->a[5] += 4;
}

/* $00255E op $1C test_moving / $002510 op $1D test_anim_done [op][who]:
 * $FF2A00 bit 2 (resp. 3) = the actor's bit in ram_actors_moving (resp. anim_done) */
static __attribute__((unused)) void test_actor_bit(hle_ctx *ctx, int flagbit, uint32_t bits_addr)
{
	if (exec_mode(ctx)) {
		bit_clr(ctx, RAM_GAME_STATE, flagbit);
		set_w(&ctx->d[0], opw(ctx, 2));
		uint16_t w = (uint16_t)cc_sub(ctx, ctx->d[0], 1, 16, true);
		set_w(&ctx->d[0], w);
		if (w == 0) {
			ctx->d[1] = 0;
			cc_logic32(ctx, 0);
		} else {
			w = (uint16_t)cc_sub(ctx, ctx->d[0], 1, 16, true);
			set_w(&ctx->d[0], w);
			if (w == 0) {
				ctx->d[1] = 1;
				cc_logic32(ctx, 1);
			} else {
				w = (uint16_t)cc_sub(ctx, ctx->d[0], 1, 16, true);
				ctx->d[0] = (uint32_t)w * OBJ_SIZE;
				ctx->a[0] = RAM_OBJECTS;
				uint8_t slot = RAM8(RAM_OBJECTS + (int16_t)ctx->d[0] + OBJ_ACTOR);
				set_b(&ctx->d[1], slot);
				set_w(&ctx->d[1], slot);
				set_w(&ctx->d[1], (uint16_t)cc_add(ctx, ctx->d[1], 2, 16));
			}
		}
		if (bit_test(ctx, bits_addr, ctx->d[1]))
			bit_set(ctx, RAM_GAME_STATE, flagbit);
	}
	ctx->a[5] += 4;
}
/* ops $1C/$1D are never executed on any route (no difftest states): not registered. */
#ifdef SCRIPT_UNTESTED
HLE_FUNC(0x00255E, script_op_test_moving, 90) { test_actor_bit(ctx, 2, RAM_ACTORS_MOVING); }
HLE_FUNC(0x002510, script_op_test_anim_done, 90) { test_actor_bit(ctx, 3, RAM_ACTORS_ANIMDONE); }
#endif

/* $002626 op $1A pal_cycle [1A][slot][first][last][speed]: slot 0-4 of the VBlank palette
 * cycler; speed < 0 turns the slot off and DMAs colours first..last-1 of the target palette
 * back to CRAM. */
HLE_FUNC(0x002626, script_op_pal_cycle, 150)
{
	if (exec_mode(ctx)) {
		set_w(&ctx->d[0], opw(ctx, 2));
		if (cmpw(ctx, ctx->d[0], 4) <= 0) {
			uint16_t slot = (uint16_t)ctx->d[0];
			uint16_t speed = opw(ctx, 8);
			cc_logic16(ctx, speed);
			if (speed & 0x8000) {
				bit_clr(ctx, RAM_PALCYC_ON, slot);
				uint16_t first = opw(ctx, 4);
				set_w(&ctx->d[0], first);
				set_w(&ctx->d[1], (uint16_t)cc_sub(ctx, opw(ctx, 6), first, 16, true));
				set_w(&ctx->d[0], (uint16_t)cc_add(ctx, first, first, 16));
				ctx->a[0] = RAM_PAL_TARGET + (int16_t)ctx->d[0];
				hle_call68k(ctx, VDP_DMA_CRAM);
			} else {
				set_w(&ctx->d[1], slot);
				set_w(&ctx->d[0], (uint16_t)cc_add(ctx, slot, slot, 16));
				ctx->a[0] = RAM_PALCYC;
				int o = (int16_t)ctx->d[0];
				RAM16(RAM_PALCYC + o) = opw(ctx, 4);
				RAM16(RAM_PALCYC + 0xA + o) = opw(ctx, 6);
				RAM16(RAM_PALCYC + 0x1E + o) = speed;
				RAM16(RAM_PALCYC + 0x14 + o) = speed;
				cc_logic16(ctx, speed);
				bit_set(ctx, RAM_PALCYC_ON, slot);
			}
		}
	}
	ctx->a[5] += 0xA;
}

/* ------------------------------------------------------------------ sub-ops (op $0C) */
/* One-instruction sub-ops: bset/bclr #bit,(flag byte); Z = !old bit. */
#define SUBOP_BIT(ADDR, NAME, BYTE, BIT, SET)                    \
	HLE_FUNC(ADDR, NAME, 28)                                       \
	{                                                              \
		if (SET) bit_set(ctx, BYTE, BIT); else bit_clr(ctx, BYTE, BIT); \
	}
SUBOP_BIT(0x00453E, script_sys_end, RAM_SCRIPT_FLAGS, 0, 1)     /* $01: stop the script */
SUBOP_BIT(0x004482, script_sys_09e8_1_on, RAM_09E8, 1, 1)       /* $0D camera locked */
SUBOP_BIT(0x00448C, script_sys_09e8_1_off, RAM_09E8, 1, 0)      /* $0E */
SUBOP_BIT(0x004298, script_sys_pal_flash_off, RAM_09E8, 7, 0)   /* $1B */
SUBOP_BIT(0x002FC2, script_sys_09eb_5_on, RAM_09EB, 5, 1)       /* $3A */
SUBOP_BIT(0x002FCC, script_sys_09eb_5_off, RAM_09EB, 5, 0)      /* $3B */
SUBOP_BIT(0x002FD6, script_sys_skippable_off, 0xFF0ACA, 1, 0)   /* $38 */

/* $004274 sub $1A pal_flash_on: palette flash effect parameters, $FF09E8 bit 7 */
HLE_FUNC(0x004274, script_sys_pal_flash_on, 60)
{
	RAM16(0xFF08AA) = 0xFFFF;
	RAM16(0xFF08AC) = 1;
	uint8_t v = ROM8(0x03247C);
	RAM8(0xFF0ACF) = v;
	cc_logic8(ctx, v);
	bit_set(ctx, RAM_09E8, 7);
}

/* $002FE0 sub $39 skippable_on: Start may skip this cutscene (flags2 bit 1); in a game
 * started from a password ($FF09ED bit 2) the skip request (bit 0) is set at once. */
HLE_FUNC(0x002FE0, script_sys_skippable_on, 50)
{
	bit_set(ctx, 0xFF0ACA, 1);
	if (bit_test(ctx, 0xFF09ED, 2))
		bit_set(ctx, 0xFF0ACA, 0);
}

/* ------------------------------------------------------------------ run_object_events */
/* $00AFDA run_object_events, called by the main loop every pass (and while text is shown):
 * for every object in the current room (or carried) whose record +$18 bit 2 ("event
 * pending", set by the VBlank object timers) is set, scan its script for an on_event block
 * (script_run with script_flags = $10, keeping bit 7) and run that block (script_run_frame).
 * The pushes/pops around the two script calls stay 68k; the object walk is C:
 *   $00AFEA run_object_events_setup   header pointers, count, then the walk
 *   $00B05E run_object_events_matched after the scan: run the block or go on walking
 *   $00B09A run_object_events_next    after the block: go on walking
 * Walk state lives in the 68k registers the original keeps it in: A0 = object record,
 * A1 = object script table entry (8 bytes), A2 = script base, D0.w = objects left - 1. */
#define ROE_CALL_SCAN 0x00B050      /* movem.l d0/a0-a2,-(sp) / jsr script_run */
#define ROE_CALL_RUN  0x00B078      /* movem.l ... / set A5/A6 / jsr script_run_frame */
#define ROE_DONE      0x00B0A6      /* restore flags, callback, registers; rts */

/* the walk from the object at A0 on; exits in front of a scan or at the end */
static void roe_walk(hle_ctx *ctx)
{
	for (;;) {
		uint32_t rec = ctx->a[0];
		if (bit_test(ctx, rec + OBJ_FLAGS, 2)) {
			uint16_t room = RAM16(rec + OBJ_ROOM);
			set_w(&ctx->d[1], room);
			bool here = cmpw(ctx, room, 1) == 0;            /* carried */
			if (!here)
				here = cmpw(ctx, room, RAM16(RAM_ROOM_INDEX)) == 0;
			if (here) {
				/* script_flags = (flags & $80) | $10: match on_event */
				uint8_t f = (uint8_t)((RAM8(RAM_SCRIPT_FLAGS) & 0x80) | SF_MATCH_EVENT);
				set_b(&ctx->d[1], f);
				RAM8(RAM_SCRIPT_FLAGS) = f;
				RAM32_W(RAM_SCRIPT_CB, 0x0023DC);           /* script_run */
				ctx->a[5] = bus_r32(ctx->a[1]) + ctx->a[2];
				uint16_t len = bus_r16(ctx->a[5]);
				set_w(&ctx->d[1], len);
				cc_logic16(ctx, len);
				ctx->a[5] += 6;
				ctx->a[6] = ctx->a[5] + (int16_t)len;
				hle_jump(ctx, ROE_CALL_SCAN);
				return;
			}
		}
		/* next object: lea 8(a1),a1 / lea 26(a0),a0 / dbf d0 */
		ctx->a[1] += 8;
		ctx->a[0] += OBJ_SIZE;
		uint16_t n = (uint16_t)(ctx->d[0] - 1);
		set_w(&ctx->d[0], n);
		if (n == 0xFFFF) {
			hle_jump(ctx, ROE_DONE);
			return;
		}
	}
}

HLE_RANGE(0x00AFEA, run_object_events_setup, 200, ROE_CALL_SCAN, ROE_DONE)
{
	uint32_t hdr = RAM32(RAM_MYSTERY_HDR);
	ctx->a[0] = hdr;
	ctx->a[1] = bus_r32(hdr + 0x28);                      /* object script table */
	ctx->a[2] = bus_r32(hdr + 0x2C);                      /* script base */
	ctx->a[0] = RAM_OBJECTS;
	set_w(&ctx->d[0], RAM16(0xFF06F4));                   /* ram_obj_last */
	set_w(&ctx->d[0], cc_sub(ctx, ctx->d[0], 1, 16, true));
	roe_walk(ctx);
}

HLE_RANGE(0x00B05E, run_object_events_matched, 60, ROE_CALL_RUN, ROE_CALL_SCAN, ROE_DONE)
{
	if (!bit_test(ctx, RAM_SCRIPT_FLAGS, 0)) {            /* no on_event block */
		ctx->a[1] += 8;
		ctx->a[0] += OBJ_SIZE;
		uint16_t n = (uint16_t)(ctx->d[0] - 1);
		set_w(&ctx->d[0], n);
		if (n == 0xFFFF)
			hle_jump(ctx, ROE_DONE);
		else
			roe_walk(ctx);
		return;
	}
	uint8_t f = RAM8(RAM_SCRIPT_FLAGS) & 0x80;
	RAM8(RAM_SCRIPT_FLAGS) = f;
	cc_logic8(ctx, f);
	bit_clr(ctx, ctx->a[0] + OBJ_FLAGS, 2);               /* event handled */
	hle_jump(ctx, ROE_CALL_RUN);
}

HLE_RANGE(0x00B09A, run_object_events_next, 40, ROE_CALL_SCAN, ROE_DONE)
{
	ctx->a[1] += 8;
	ctx->a[0] += OBJ_SIZE;
	uint16_t n = (uint16_t)(ctx->d[0] - 1);
	set_w(&ctx->d[0], n);
	if (n == 0xFFFF)
		hle_jump(ctx, ROE_DONE);
	else
		roe_walk(ctx);
}

/* ------------------------------------------------------------------ blocking ops, split */
/* Each of these stops in front of its blocking call (the 68k makes it) and continues in
 * the next range; skipping ends at the handler's own rts. */

/* $00274A op $04 if_else [04][then_len][else_len][cond], then-body at +$10, else-body at
 * +then_len: true -> exit $002752, where the 68k pushes A5/A6 and runs the then-body
 * through ram_script_cb (nested run; it may block), then skips the else-body. False ->
 * fall into the else-body. */
HLE_RANGE(0x00274A, script_op_if_else, 160, 0x002752, 0x002794)
{
	if (script_cond(ctx)) {
		hle_jump(ctx, 0x002752);
		return;
	}
	skip_by_word(ctx, 2);
	hle_jump(ctx, 0x002794);
}

/* $002E9E op $0C sys [0C][sub]: execute mode -> exit $002EAC `jsr (a0)` with A0 =
 * script_subop_table[sub]; $002EAE afterwards: A5 += 4. */
HLE_RANGE(0x002E9E, script_op_sys, 40, 0x002EAC, 0x002EB2)
{
	if (!exec_mode(ctx)) {
		ctx->a[5] += 4;
		hle_jump(ctx, 0x002EB2);
		return;
	}
	set_w(&ctx->d[0], opw(ctx, 2));
	set_w(&ctx->d[0], lslw(ctx, ctx->d[0], 2));
	ctx->a[0] = bus_r32(SUBOP_TABLE + (int16_t)ctx->d[0]);
	hle_jump(ctx, 0x002EAC);
}
HLE_RANGE(0x002EAE, script_op_sys_done, 8, 0x002EB2)
{
	ctx->a[5] += 4;
	hle_jump(ctx, 0x002EB2);
}

/* $0048D8 op $15 wait [15][frames]: FRAME_COUNTDOWN = frames, then one actor frame
 * (sub_00B0B8 at $0048E4, blocks) until VBlank has counted it below zero. */
HLE_RANGE(0x0048D8, script_op_wait, 40, 0x0048E4, 0x0048F6)
{
	if (!exec_mode(ctx)) {
		ctx->a[5] += 4;
		hle_jump(ctx, 0x0048F6);
		return;
	}
	uint16_t n = opw(ctx, 2);
	RAM16(FRAME_COUNTDOWN) = n;
	cc_logic16(ctx, n);
	hle_jump(ctx, 0x0048E4);
}
HLE_RANGE(0x0048E8, script_op_wait_loop, 30, 0x0048E4, 0x0048F6)
{
	uint16_t n = RAM16(FRAME_COUNTDOWN);
	cc_logic16(ctx, n);
	if (!(n & 0x8000)) {
		hle_jump(ctx, 0x0048E4);
		return;
	}
	ctx->a[5] += 4;
	hle_jump(ctx, 0x0048F6);
}

/* $004EE4 op $18 goto_room_cut [18][room][spawn][facing]: set the room, Shaggy's facing
 * and the spawn offset, then room_load ($002138 via bsr at $004F04). */
#define RAM_SHAGGY_FACING 0xFF05B8
#define RAM_SPAWN_OFS     0xFF069E   /* word: spawn index * 4 */
HLE_RANGE(0x004EE4, script_op_goto_room_cut, 60, 0x004F04, 0x004F0C)
{
	if (!exec_mode(ctx)) {
		ctx->a[5] += 8;
		hle_jump(ctx, 0x004F0C);
		return;
	}
	RAM16(RAM_ROOM_INDEX) = opw(ctx, 2);
	RAM16(RAM_SHAGGY_FACING) = opw(ctx, 6);
	set_w(&ctx->d[0], lslw(ctx, opw(ctx, 4), 2));
	RAM16(RAM_SPAWN_OFS) = (uint16_t)ctx->d[0];
	cc_logic16(ctx, ctx->d[0]);
	hle_jump(ctx, 0x004F04);
}
HLE_RANGE(0x004F08, script_op_goto_room_cut_done, 8, 0x004F0C)
{
	ctx->a[5] += 8;
	hle_jump(ctx, 0x004F0C);
}

/* $004F0E op $11 goto_room [11][room][spawn][facing]: like $18, but fades out first
 * (palette_read_cram at $004F30 with VBlank frame work paused) and reloads with a fade in
 * (room_reload at $004F46). */
HLE_RANGE(0x004F0E, script_op_goto_room, 60, 0x004F30, 0x004F4E)
{
	if (!exec_mode(ctx)) {
		ctx->a[5] += 8;
		hle_jump(ctx, 0x004F4E);
		return;
	}
	RAM16(RAM_ROOM_INDEX) = opw(ctx, 2);
	RAM16(RAM_SHAGGY_FACING) = opw(ctx, 6);
	set_b(&ctx->d[5], RAM8(VBLANK_FLAGS));                /* saved across the fade */
	cc_logic8(ctx, ctx->d[5]);
	bit_clr(ctx, VBLANK_FLAGS, 6);
	hle_jump(ctx, 0x004F30);
}
HLE_RANGE(0x004F34, script_op_goto_room_load, 40, 0x004F46)
{
	RAM8(VBLANK_FLAGS) = (uint8_t)ctx->d[5];
	set_w(&ctx->d[0], lslw(ctx, opw(ctx, 4), 2));
	RAM16(RAM_SPAWN_OFS) = (uint16_t)ctx->d[0];
	cc_logic16(ctx, ctx->d[0]);
	hle_jump(ctx, 0x004F46);
}
HLE_RANGE(0x004F4A, script_op_goto_room_done, 8, 0x004F4E)
{
	ctx->a[5] += 8;
	hle_jump(ctx, 0x004F4E);
}

/* $002DF2 op $0B set_state [0B][who | $8000 wait][state]:
 *   actor objects: +$00 = state;
 *   others: +$02 (hot spot) = state if state != 0; in another room +$00 = state; in the
 *   current room +$00 = state (state 0: +$00 |= $C000 = hidden, unless already 0) and the
 *   redraw bit +$18 bit 0; with bit 15 the 68k then spins at $002E6E until the VBlank
 *   object code has redrawn it (cleared the bit). */
HLE_RANGE(0x002DF2, script_op_set_state, 120, 0x002E6E, 0x002E7C)
{
	if (!exec_mode(ctx))
		goto done;
	set_w(&ctx->d[0], opw(ctx, 2) & 0x7FFF);
	ctx->d[0] = obj_mul(ctx, ctx->d[0], 3);
	ctx->a[0] = RAM_OBJECTS;
	{
		uint32_t rec = obj_addr(ctx->d[0], 0);
		uint16_t state = opw(ctx, 4);
		if (bit_test(ctx, rec + OBJ_FLAGS, 1)) {           /* actor */
			RAM16(rec) = state;
			cc_logic16(ctx, state);
			goto done;
		}
		set_w(&ctx->d[1], RAM16(rec + OBJ_ROOM));
		cc_logic16(ctx, state);
		if (state) {
			RAM16(rec + OBJ_HOTSPOT) = state;
			cc_logic16(ctx, state);
		}
		if (cmpw(ctx, ctx->d[1], RAM16(RAM_ROOM_INDEX)) != 0) {
			RAM16(rec) = state;                            /* not visible: just store */
			cc_logic16(ctx, state);
			goto done;
		}
		cc_logic16(ctx, state);
		if (state == 0) {
			uint16_t cur = RAM16(rec);
			cc_logic16(ctx, cur);
			if (cur == 0)
				goto done;
			cur |= 0xC000;                                 /* hide */
			RAM16(rec) = cur;
			cc_logic16(ctx, cur);
		} else {
			RAM16(rec) = state;
			cc_logic16(ctx, state);
		}
		uint8_t f = RAM8(rec + OBJ_FLAGS) | 1;              /* redraw */
		RAM8(rec + OBJ_FLAGS) = f;
		cc_logic8(ctx, f);
		uint16_t who = opw(ctx, 2);
		cc_logic16(ctx, who);
		if (who & 0x8000) {
			hle_jump(ctx, 0x002E6E);                       /* wait for the redraw */
			return;
		}
	}
done:
	ctx->a[5] += 6;
	hle_jump(ctx, 0x002E7C);
}

/* ------------------------------------------------------------------ action_execute */
/* $0019B0 action_execute: the player used verb ram_action_verb on ram_target_obj (or walked
 * onto a hot spot: verb 11). Flow (stack pushes and the script calls stay 68k):
 *   1. object script of the target: A5/A6 = body, $FF06B0 = header word 2; push A5/A6;
 *      script_run_nested collects the talk_choice ops                       ($0019B0)
 *   2. verb pass: script_flags = 2, D7 = verb, push verb_state; script_run   ($001A16)
 *   3. a case matched: run its body with script_run_frame                    ($001A46)
 *   4. pop A5/A6; TALK with choices: the dialog menu sub_001C0C              ($001A84,
 *      $001A9C after the menu)
 *   5. nothing happened: "I can't <verb> that/him/her." in the text box     (ae_finish)
 *   6. deselect the verb; sys then_talk re-enters with TALK                  ($001BA0) */
#define RAM_ACTION_VERB   0xFF06BE
#define RAM_PRONOUN       0xFF06B0   /* word: object script header word 2 (high byte: pronoun bits) */
#define RAM_TEXT_BUF      0xFF08BC   /* status/text line buffer */
#define RAM_THEN_TALK_OBJ 0xFF06DA
#define VERB_FAIL_MSGS    0x02FEA0   /* 10 longs: "I can't open \1." ... (\1 = pronoun) */
#define STR_THAT          0x02FFDB
#define STR_HIM           0x02FFE0
#define STR_HER           0x02FFE4
#define STR_COPY          0x005B56
#define VERB_REDRAW_BOTH  0x001BDA   /* repaint the selected and the default verb label */
#define AE_START          0x0019B0
#define AE_RTS            0x001BD6

static void ae_deselect(hle_ctx *ctx);

/* $001AAC..: after the verb pass (and TALK menu): failure text unless a script ran */
static void ae_finish(hle_ctx *ctx)
{
	uint16_t verb = (uint16_t)ctx->d[7];
	if (bit_test(ctx, RAM_SCRIPT_FLAGS, 6)) {             /* a verb script ran */
		ae_deselect(ctx);
		return;
	}
	bool z;
	if (cmpw(ctx, verb, 5) == 0) {                         /* USE */
		bit_clr(ctx, RAM_09E8, 0);
		z = ctx->sr & CC_Z;
		goto use_give;
	}
	if (cmpw(ctx, verb, 6) == 0) {                         /* GIVE */
		bit_set(ctx, RAM_09E8, 0);
		cc_btst(ctx, ctx->d[0], 0);                        /* btst #0,d0 (saved verb_state) */
		z = ctx->sr & CC_Z;
		goto use_give;
	}
	goto check;
use_give:
	/* USE: Z from bclr #0,($09E8) (quirk); GIVE: Z = bit 0 of the verb_state saved before
	 * the verb pass clear */
	if (!z) {
		bit_clr(ctx, RAM_VERB_STATE, 0);
		goto check;
	}
	if (bit_test(ctx, RAM_SCRIPT_FLAGS, 5))                /* waits for a second object */
		goto check;
	if (bit_test(ctx, RAM_VERB_STATE, 0)) {
		bit_clr(ctx, RAM_VERB_STATE, 0);
		goto check;
	}
	RAM16(RAM_TARGET2_OBJ) = 0;
	cc_logic16(ctx, 0);
	bit_set(ctx, RAM_VERB_STATE, 0);
check:
	if (bit_test(ctx, RAM_VERB_STATE, 0) || cmpw(ctx, verb, 11) == 0) {
		ae_deselect(ctx);
		return;
	}
	/* build "I can't <verb> \1." with \1 = that / him / her */
	ctx->a[0] = VERB_FAIL_MSGS;
	set_w(&ctx->d[7], cc_sub(ctx, verb, 1, 16, true));
	set_w(&ctx->d[7], lslw(ctx, ctx->d[7], 2));
	ctx->a[2] = bus_r32(VERB_FAIL_MSGS + (int16_t)ctx->d[7]);
	ctx->a[1] = RAM_TEXT_BUF;
	for (;;) {
		uint8_t ch = bus_r8(ctx->a[2]++);
		bus_w8(ctx->a[1]++, ch);
		cc_logic8(ctx, ch);
		if (!ch)
			break;
		cc_sub(ctx, ch, 1, 8, false);
		if (ch != 1)
			continue;
		ctx->a[1]--;
		cc_logic8(ctx, 1);                                 /* tst.b -(a1) */
		if (!bit_test(ctx, RAM_PRONOUN, 0))
			ctx->a[0] = STR_THAT;
		else if (!bit_test(ctx, RAM_PRONOUN, 1))
			ctx->a[0] = STR_HIM;
		else
			ctx->a[0] = STR_HER;
		hle_call68k(ctx, STR_COPY);
		ctx->a[1]--;
		cc_logic8(ctx, 0);                                 /* tst.b -(a1): the NUL */
	}
	ctx->a[0] = RAM_TEXT_BUF;
	hle_jump(ctx, 0x001B9A);                               /* jsr sub_00A130: print, wait */
}

/* $001BA0: deselect the verb unless it was the one used; sys then_talk -> TALK again */
static void ae_deselect(hle_ctx *ctx)
{
	set_w(&ctx->d[0], RAM16(RAM_VERB_CUR));
	if (cmpw(ctx, ctx->d[0], RAM16(RAM_ACTION_VERB)) != 0) {
		RAM16(RAM_VERB_CUR) = 0;
		cc_logic16(ctx, 0);
	}
	if (!bit_test(ctx, 0xFF0ACA, 3)) {
		hle_jump(ctx, AE_RTS);
		return;
	}
	RAM16(RAM_ACTION_VERB) = 8;
	uint16_t t = RAM16(RAM_THEN_TALK_OBJ);
	RAM16(RAM_TARGET_OBJ) = t;
	cc_logic16(ctx, t);
	hle_jump(ctx, AE_START);
}

HLE_RANGE(0x0019B0, action_execute, 200, 0x001BD8, 0x0019F6, 0x001A0E)
{
	RAM16(RAM_TALK_COUNT) = 0;
	bit_clr(ctx, 0xFF0ACA, 3);
	ctx->a[0] = RAM32(RAM_MYSTERY_HDR);
	ctx->a[6] = bus_r32(ctx->a[0] + 0x2C);                /* script base */
	ctx->a[0] = bus_r32(ctx->a[0] + 0x28);                /* object script table */
	set_w(&ctx->d[0], RAM16(RAM_TARGET_OBJ));
	set_w(&ctx->d[0], cc_sub(ctx, ctx->d[0], 3, 16, true));
	if (ctx->d[0] & 0x8000) {                              /* Shaggy, Scooby, nothing */
		hle_jump(ctx, 0x001BD8);
		return;
	}
	if (cmpw(ctx, RAM16(RAM_ACTION_VERB), 2) == 0) {       /* TAKE something carried: no-op */
		set_w(&ctx->d[1], (uint16_t)ctx->d[0]);
		ctx->d[1] = (ctx->d[1] & 0xFFFF) * OBJ_SIZE;
		ctx->a[1] = RAM_OBJECTS;
		if (cmpw(ctx, RAM16(obj_addr(ctx->d[1], OBJ_ROOM)), 1) == 0) {
			hle_jump(ctx, 0x0019F6);
			return;
		}
	}
	set_w(&ctx->d[0], lslw(ctx, ctx->d[0], 3));
	ctx->a[6] += bus_r32(ctx->a[0] + (int16_t)ctx->d[0]);
	ctx->a[5] = ctx->a[6];
	uint16_t len = bus_r16(ctx->a[5]);
	set_w(&ctx->d[0], len);
	uint16_t pron = bus_r16(ctx->a[5] + 2);
	RAM16(RAM_PRONOUN) = pron;
	cc_logic16(ctx, bus_r16(ctx->a[5] + 4));
	ctx->a[5] += 6;
	/* quirk: the end is header + len, 6 bytes short of the body end that
	 * run_object_events uses (header + 6 + len); ops starting in the last 6 bytes are never
	 * scanned here (no data puts a case there) */
	ctx->a[6] += (int16_t)len;
	hle_jump(ctx, 0x001A0E);                               /* push A5/A6, script_run_nested */
}

HLE_RANGE(0x001A16, action_execute_verb_pass, 120, 0x001A3A)
{
	uint16_t n = RAM16(RAM_TALK_COUNT);
	cc_logic16(ctx, n);
	if (n)
		hle_call68k(ctx, VERB_REDRAW_BOTH);
	RAM8(RAM_SCRIPT_FLAGS) = SF_MATCH_CASE;
	RAM32_W(RAM_SCRIPT_CB, 0x0023DC);
	set_w(&ctx->d[7], RAM16(RAM_ACTION_VERB));
	cc_logic16(ctx, ctx->d[7]);
	hle_jump(ctx, 0x001A3A);                               /* push verb_state, script_run */
}

HLE_RANGE(0x001A46, action_execute_run_case, 120, 0x001A7C, 0x001A80)
{
	if (!bit_test(ctx, RAM_SCRIPT_FLAGS, 0)) {             /* no case for this verb */
		hle_jump(ctx, 0x001A80);
		return;
	}
	bit_set(ctx, RAM_SCRIPT_FLAGS, 6);
	uint16_t len = opw(ctx, 6);
	set_w(&ctx->d[0], len);
	ctx->a[6] = ctx->a[5] + (int16_t)len;
	ctx->a[5] += 8;
	RAM32_W(RAM_SCRIPT_CB, 0x002406);
	bit_clr(ctx, RAM_SCRIPT_FLAGS, 0);
	hle_call68k(ctx, VERB_REDRAW_BOTH);
	hle_jump(ctx, 0x001A7C);                               /* script_run_frame */
}

HLE_RANGE(0x001A84, action_execute_after, 300, 0x001A8C, 0x001B9A, AE_START, AE_RTS)
{
	if (cmpw(ctx, ctx->d[7], 8) == 0) {
		hle_jump(ctx, 0x001A8C);                           /* TALK: the dialog menu */
		return;
	}
	ae_finish(ctx);
}

HLE_RANGE(0x001A9C, action_execute_after_talk, 300, 0x001B9A, AE_START, AE_RTS)
{
	uint16_t n = RAM16(RAM_TALK_COUNT);
	cc_logic16(ctx, n);
	if (n)
		bit_set(ctx, RAM_SCRIPT_FLAGS, 6);
	ae_finish(ctx);
}

HLE_RANGE(0x001BA0, action_execute_done, 60, AE_START, AE_RTS)
{
	ae_deselect(ctx);
}

/* ------------------------------------------------------------------ room scripts */
/* $00221A room_enter_script (end of room_load) and $002278 room_after_fade_script (end of
 * room_fade_in): run the current room's script (room entry +$0C resp. +$10, offset from
 * header+$2C; [len][word] body) in execute mode with VBF_SCRIPT set. The 68k pushes the
 * callback and ram_vblank_flags first; afterwards VBF_SCRIPT is restored to its old state.
 *   $002226 / $002284  set up A5/A6 -> exit at bsr script_run_frame
 *   $002266 / $0022C4  after `move.b (sp)+,d0`: restore VBF_SCRIPT -> exit at the pop of
 *                      the callback */
static void room_script_setup(hle_ctx *ctx, int field, uint32_t exit)
{
	RAM32_W(RAM_SCRIPT_CB, 0x002406);
	set_w(&ctx->d[0], RAM16(RAM_ROOM_INDEX));
	set_w(&ctx->d[0], cc_sub(ctx, ctx->d[0], 2, 16, true));
	ctx->d[0] = (ctx->d[0] & 0xFFFF) * 0x14;              /* room entry */
	ctx->a[0] = RAM32(RAM_MYSTERY_HDR);
	ctx->a[1] = bus_r32(ctx->a[0] + 8);
	ctx->d[0] = bus_r32(ctx->a[1] + field + (int16_t)ctx->d[0]);
	ctx->a[5] = bus_r32(ctx->a[0] + 0x2C) + ctx->d[0];
	uint16_t len = bus_r16(ctx->a[5]);
	set_w(&ctx->d[0], len);
	cc_logic16(ctx, bus_r16(ctx->a[5] + 2));
	ctx->a[5] += 4;
	ctx->a[6] = ctx->a[5] + (int16_t)len;
	bit_set(ctx, VBLANK_FLAGS, 7);
	hle_jump(ctx, exit);
}
static void room_script_restore(hle_ctx *ctx, uint32_t exit)
{
	uint8_t old = (uint8_t)ctx->d[0] & 0x80;              /* andi.b #$80,d0 */
	set_b(&ctx->d[0], old);
	uint8_t v = RAM8(VBLANK_FLAGS) | old;
	RAM8(VBLANK_FLAGS) = v;
	cc_logic8(ctx, v);
	hle_jump(ctx, exit);
}
HLE_RANGE(0x002226, room_enter_script, 120, 0x002260) { room_script_setup(ctx, 0x0C, 0x002260); }
HLE_RANGE(0x002266, room_enter_script_done, 30, 0x002270) { room_script_restore(ctx, 0x002270); }
HLE_RANGE(0x002284, room_after_fade_script, 120, 0x0022BE) { room_script_setup(ctx, 0x10, 0x0022BE); }
HLE_RANGE(0x0022C4, room_after_fade_script_done, 30, 0x0022CE) { room_script_restore(ctx, 0x0022CE); }

/* ------------------------------------------------------------------ actor waits */
/* "who -> actor bit" into D0 as ops $17/$19/$1F/$20 compute it (exact register and flag
 * effects): 1 Shaggy -> 0, 2 Scooby -> 1, object -> its actor slot byte + 2 (andi: masked
 * with $FF first; without it the byte lands in a word whose high byte is from the mulu). */
static void actor_bit_d0(hle_ctx *ctx, bool andi)
{
	set_w(&ctx->d[0], opw(ctx, 2));
	uint16_t w = (uint16_t)cc_sub(ctx, ctx->d[0], 1, 16, true);
	set_w(&ctx->d[0], w);
	if (w == 0)
		return;
	w = (uint16_t)cc_sub(ctx, ctx->d[0], 1, 16, true);
	set_w(&ctx->d[0], w);
	if (w != 0) {
		ctx->d[0] = obj_mul(ctx, ctx->d[0], 1);
		ctx->a[0] = RAM_OBJECTS;
		uint8_t slot = RAM8(RAM_OBJECTS + (int16_t)ctx->d[0] + OBJ_ACTOR);
		set_b(&ctx->d[0], slot);
		cc_logic8(ctx, slot);
		if (andi) {
			set_w(&ctx->d[0], slot);
			cc_logic16(ctx, slot);
		}
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 2, 16));
		return;
	}
	ctx->d[0] = 1;
	cc_logic32(ctx, 1);
}

/* $002B0E op $19 unfreeze_wait [19][who]: set the actor's ram_actors_depth_new bit, then
 * one actor frame at a time (jsr sub_00B0B8 at $002B3E) until its animation looped, then
 * clear its freeze bit. */
HLE_RANGE(0x002B0E, script_op_unfreeze_wait, 80, 0x002B3E, 0x002B58)
{
	if (!exec_mode(ctx)) {
		ctx->a[5] += 4;
		hle_jump(ctx, 0x002B58);
		return;
	}
	actor_bit_d0(ctx, false);
	bit_set(ctx, 0xFF0ABE, ctx->d[0]);
	hle_jump(ctx, 0x002B3E);
}
HLE_RANGE(0x002B44, script_op_unfreeze_wait_loop, 40, 0x002B3E, 0x002B58)
{
	if (!bit_test(ctx, 0xFF0ABD, ctx->d[0])) {            /* ram_actors_anim_looped */
		hle_jump(ctx, 0x002B3E);
		return;
	}
	bit_clr(ctx, RAM_ACTORS_FROZEN, ctx->d[0]);
	ctx->a[5] += 4;
	hle_jump(ctx, 0x002B58);
}

/* $004FBE op $17 wait_anim [17][who]: actor frames (bsr sub_00B0B8 at $004FEE) until the
 * actor's ram_actors_anim_done bit is set. */
HLE_RANGE(0x004FBE, script_op_wait_anim, 80, 0x004FEE, 0x005000)
{
	if (!exec_mode(ctx)) {
		ctx->a[5] += 4;
		hle_jump(ctx, 0x005000);
		return;
	}
	actor_bit_d0(ctx, true);
	hle_jump(ctx, 0x004FEE);
}
HLE_RANGE(0x004FF2, script_op_wait_anim_loop, 40, 0x004FEE, 0x005000)
{
	if (!bit_test(ctx, RAM_ACTORS_ANIMDONE, ctx->d[0])) {
		hle_jump(ctx, 0x004FEE);
		return;
	}
	ctx->a[5] += 4;
	hle_jump(ctx, 0x005000);
}

/* ------------------------------------------------------------------ verb bar slide */
/* sub $1D bar_hide ($00482E): mark the bar hidden ($FF2A00 bit 1), wait for any slide to
 * finish (68k spin at $004836), save the bar (sub_0047AC at $004840), start the slide
 * ($FF0ACB = 16) and run actor frames ($00484C) until it is done; $FF0876 = plane address
 * of the saved bar.
 * sub $1C bar_show ($004862): slide back in, DMA the saved bar rows back, recolour the
 * font, clear $FF2A00 bit 1. */
#define RAM_BAR_SLIDE   0xFF0ACB
#define RAM_BAR_SAVED   0xFF0876
#define VDP_DMA_VRAM    0x0099F6
#define FONT_RECOLOUR   0x007A2A
HLE_RANGE(0x00482E, script_sys_bar_hide, 40, 0x004836)
{
	bit_set(ctx, RAM_GAME_STATE, 1);
	hle_jump(ctx, 0x004836);
}
HLE_RANGE(0x004844, script_sys_bar_hide_slide, 30, 0x00484C)
{
	RAM8(RAM_BAR_SLIDE) = 0x10;
	cc_logic8(ctx, 0x10);
	hle_jump(ctx, 0x00484C);
}
HLE_RANGE(0x004850, script_sys_bar_hide_loop, 40, 0x00484C, 0x004860)
{
	uint8_t s = RAM8(RAM_BAR_SLIDE);
	cc_logic8(ctx, s);
	if (!(s & 0x80)) {
		hle_jump(ctx, 0x00484C);
		return;
	}
	RAM16(RAM_BAR_SAVED) = (uint16_t)ctx->d[4];
	cc_logic16(ctx, ctx->d[4]);
	hle_jump(ctx, 0x004860);
}
HLE_RANGE(0x004862, script_sys_bar_show, 40, 0x004870)
{
	set_w(&ctx->d[4], RAM16(RAM_BAR_SAVED));
	RAM8(RAM_BAR_SLIDE) = 0x10;
	cc_logic8(ctx, 0x10);
	hle_jump(ctx, 0x004870);
}
HLE_RANGE(0x004874, script_sys_bar_show_loop, 600, 0x004870, 0x0048B4)
{
	uint8_t s = RAM8(RAM_BAR_SLIDE);
	cc_logic8(ctx, s);
	if (!(s & 0x80)) {
		hle_jump(ctx, 0x004870);
		return;
	}
	/* 6 rows of 32 words from the save buffer $FF2C00 back to the bar plane */
	set_w(&ctx->d[0], (uint16_t)ctx->d[4]);
	ctx->a[0] = 0xFF2C00;
	ctx->d[2] = 5;
	set_w(&ctx->d[1], 0x20);
	for (;;) {
		hle_call68k(ctx, VDP_DMA_VRAM);
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 0x80, 16));
		uint16_t n = (uint16_t)(ctx->d[2] - 1);
		set_w(&ctx->d[2], n);
		if (n == 0xFFFF)
			break;
	}
	set_w(&ctx->d[0], 0xF);
	ctx->d[1] = 1;
	set_w(&ctx->d[2], RAM16(0xFF068C) & 0x9FFF);          /* ram_font_base */
	cc_logic16(ctx, ctx->d[2]);
	hle_call68k(ctx, FONT_RECOLOUR);
	bit_clr(ctx, RAM_GAME_STATE, 1);
	hle_jump(ctx, 0x0048B4);
}

/* ------------------------------------------------------------------ move_obj */
/* $0028A0 op $05 move_obj [05][who | $8000][room]: put an object into a room (1 = carried).
 * Shaggy / Scooby are ignored.
 *   actor objects (+$18 bit 1): leaving the current room frees the actor slot; arriving in
 *     the current room takes the first free slot 2..5, finds the sprite set whose first
 *     word is the object's hot-spot id in the 36-entry table $032670 (speeds in the table
 *     $94 bytes before it), places the actor at the object's x/y, and runs actor frames
 *     (jsr sub_00B0B8 at $0029E4) until its first animation loop finished;
 *   other objects: leaving the current room hides it (+$00 |= $8000, redraw) and waits a
 *     frame (bsr wait_vblank at $002A28); entering it redraws it; into / out of the
 *     inventory updates ram_inv_list and the inventory page. Unless bit 15 (or the bar is
 *     hidden) the inventory is then rebuilt by the 68k at $002ABA (push A5, wait_vblank,
 *     inventory_refresh, inventory_arrows_update). */
#define RAM_ACTORS_ACTIVE   0xFF0AB4
#define RAM_ACTORS_DEPTH_ON 0xFF0AB8
#define RAM_ACTORS_LOOPED   0xFF0ABD
#define RAM_ACTOR_START     0xFF0AB5   /* bit n: start actor n's animation */
#define RAM_ACTOR_SPRITE    0xFF0470   /* 6 longs: sprite set */
#define RAM_ACTOR_ANIM      0xFF0488   /* 6 words: animation (ram_cur_index...) */
#define RAM_ACTOR_TIMER     0xFF0618
#define RAM_ACTOR_TARGET_X  0xFF05DC
#define RAM_INV_LIST        0xFF0A1E
#define RAM_INV_PAGE        0xFF06EC
#define SPRITE_SETS         0x032670   /* 36 longs */
#define MO_SKIP             0x002ACA
#define MO_RTS              0x002ACE

static void move_obj_tail(hle_ctx *ctx);

static void move_obj_skip(hle_ctx *ctx)
{
	ctx->a[5] += 6;
	hle_jump(ctx, MO_RTS);
}

/* actor object arriving in the current room: claim a slot and start it */
static void move_obj_spawn_actor(hle_ctx *ctx)
{
	uint32_t rec = ctx->a[0];
	ctx->d[0] = 2;
	cc_logic32(ctx, 2);
	for (;;) {
		if (!bit_test(ctx, RAM_ACTORS_ACTIVE, ctx->d[0]))
			break;
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 1, 16));
		if (cmpw(ctx, ctx->d[0], 6) == 0) {               /* no free slot */
			move_obj_skip(ctx);
			return;
		}
	}
	set_w(&ctx->d[2], RAM16(rec + OBJ_HOTSPOT));
	cc_logic16(ctx, ctx->d[2]);
	ctx->a[1] = SPRITE_SETS;
	ctx->d[1] = 0x23;
	cc_logic32(ctx, 0x23);
	for (;;) {
		ctx->a[2] = bus_r32(ctx->a[1]);
		ctx->a[1] += 4;
		if (cmpw(ctx, ctx->d[2], bus_r16(ctx->a[2])) == 0)
			break;
		uint16_t n = (uint16_t)(ctx->d[1] - 1);
		set_w(&ctx->d[1], n);
		if (n == 0xFFFF) {                                 /* unknown sprite set */
			move_obj_skip(ctx);
			return;
		}
	}
	uint16_t slot = (uint16_t)ctx->d[0];
	set_w(&ctx->d[1], slot);
	cc_logic16(ctx, slot);
	set_w(&ctx->d[2], slot);
	cc_logic16(ctx, slot);
	set_w(&ctx->d[2], lslw(ctx, ctx->d[2], 1));
	set_w(&ctx->d[1], lslw(ctx, ctx->d[1], 2));
	int16_t l = (int16_t)ctx->d[1], w = (int16_t)ctx->d[2];
	ctx->a[3] = 0xFF0514;                                  /* ram_actor_step_x */
	ctx->d[3] = bus_r32(ctx->a[1] - 0x94);                 /* walk speed of this sprite set */
	cc_logic32(ctx, ctx->d[3]);
	RAM32_W(0xFF0514 + l, ctx->d[3]);
	{
		uint32_t v = ctx->d[3];
		ctx->d[3] = v >> 1;                                /* lsr.l #1 */
		cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
		       ((v & 1) ? CC_X | CC_C : 0) | (ctx->d[3] ? 0 : CC_Z));
	}
	RAM32_W(0xFF0514 + 0x14 + l, ctx->d[3]);
	cc_logic32(ctx, ctx->d[3]);
	ctx->a[1] = RAM_ACTOR_SPRITE;
	RAM32_W(RAM_ACTOR_SPRITE + l, ctx->a[2]);
	cc_logic32(ctx, ctx->a[2]);
	ctx->a[1] = RAM_ACTOR_ANIM;
	RAM16(RAM_ACTOR_ANIM + w) = RAM16(rec);
	cc_logic16(ctx, RAM16(rec));
	RAM16(RAM_ACTOR_ANIM + 0x54 + l) = RAM16(rec + 0x12);   /* actor x (16.16 high word) */
	RAM16(RAM_ACTOR_ANIM + 0x6C + l) = RAM16(rec + 0x14);   /* actor y */
	cc_logic16(ctx, RAM16(rec + 0x14));
	ctx->a[1] = RAM_ACTOR_TIMER;
	RAM16(RAM_ACTOR_TIMER + l) = 0xFFFF;
	cc_logic16(ctx, 0xFFFF);
	ctx->a[1] = RAM_ACTOR_TARGET_X;
	RAM16(RAM_ACTOR_TARGET_X + w) = RAM16(rec + 0x12);
	RAM16(RAM_ACTOR_TARGET_X + 0xC + w) = RAM16(rec + 0x14);
	set_w(&ctx->d[1], slot);
	set_w(&ctx->d[1], cc_sub(ctx, ctx->d[1], 2, 16, true));
	RAM8(rec + OBJ_ACTOR) = (uint8_t)ctx->d[1];
	cc_logic8(ctx, ctx->d[1]);
	bit_clr(ctx, RAM_ACTORS_DEPTH_ON, slot);
	if (bit_test(ctx, rec + OBJ_FLAGS, 3))
		bit_set(ctx, RAM_ACTORS_DEPTH_ON, slot);
	bit_clr(ctx, 0xFF0AB6, slot);
	bit_clr(ctx, 0xFF0AB9, slot);
	bit_clr(ctx, RAM_ACTORS_LOOPED, slot);
	bit_set(ctx, RAM_ACTOR_START, slot);
	bit_set(ctx, RAM_ACTORS_ACTIVE, slot);
	hle_jump(ctx, 0x0029E4);                               /* wait for the first loop */
}

HLE_RANGE(0x0028A0, script_op_move_obj, 300, 0x0029E4, 0x002A28, 0x002ABA, MO_RTS)
{
	if (!exec_mode(ctx)) {
		move_obj_skip(ctx);
		return;
	}
	set_w(&ctx->d[0], opw(ctx, 2) & 0x7FFF);
	cc_logic16(ctx, ctx->d[0]);
	if ((uint16_t)ctx->d[0] == 0) {
		move_obj_skip(ctx);
		return;
	}
	set_w(&ctx->d[0], cc_sub(ctx, ctx->d[0], 1, 16, true));
	if ((uint16_t)ctx->d[0] == 0) {                        /* Shaggy */
		move_obj_skip(ctx);
		return;
	}
	set_w(&ctx->d[0], cc_sub(ctx, ctx->d[0], 1, 16, true));
	if ((uint16_t)ctx->d[0] == 0) {                        /* Scooby */
		move_obj_skip(ctx);
		return;
	}
	ctx->d[0] = obj_mul(ctx, ctx->d[0], 1);
	ctx->a[0] = obj_addr(ctx->d[0], 0);
	uint32_t rec = ctx->a[0];
	if (bit_test(ctx, rec + OBJ_FLAGS, 1)) {               /* actor object */
		set_w(&ctx->d[0], RAM16(RAM_ROOM_INDEX));
		if (cmpw(ctx, ctx->d[0], RAM16(rec + OBJ_ROOM)) == 0) {
			/* leaves the current room: free its actor slot */
			set_b(&ctx->d[1], RAM8(rec + OBJ_ACTOR));
			set_w(&ctx->d[1], ctx->d[1] & 0xFF);
			set_w(&ctx->d[1], cc_add(ctx, ctx->d[1], 2, 16));
			bit_clr(ctx, RAM_ACTORS_ACTIVE, ctx->d[1]);
			bit_clr(ctx, 0xFF0AB6, ctx->d[1]);
			RAM8(rec + OBJ_ACTOR) = 0xFF;
			cc_logic8(ctx, 0xFF);
		}
		RAM16(rec + OBJ_ROOM) = opw(ctx, 4);
		cc_logic16(ctx, opw(ctx, 4));
		if (cmpw(ctx, ctx->d[0], opw(ctx, 4)) != 0) {
			move_obj_skip(ctx);
			return;
		}
		move_obj_spawn_actor(ctx);
		return;
	}
	/* static object */
	set_w(&ctx->d[0], RAM16(rec + OBJ_ROOM));
	set_w(&ctx->d[1], opw(ctx, 4));
	cc_logic16(ctx, ctx->d[1]);
	if (cmpw(ctx, ctx->d[0], RAM16(RAM_ROOM_INDEX)) == 0) {
		cc_logic16(ctx, opw(ctx, 2));
		if (!(opw(ctx, 2) & 0x8000)) {
			uint16_t st = RAM16(rec);
			cc_logic16(ctx, st);
			if (st) {
				st |= 0x8000;                              /* hide it */
				RAM16(rec) = st;
				cc_logic16(ctx, st);
				uint8_t f = RAM8(rec + OBJ_FLAGS) | 1;
				RAM8(rec + OBJ_FLAGS) = f;
				cc_logic8(ctx, f);
				hle_jump(ctx, 0x002A28);                   /* bsr wait_vblank */
				return;
			}
		}
	}
	move_obj_tail(ctx);
}

/* $002A2C: the static-object part after the optional wait */
static void move_obj_tail(hle_ctx *ctx)
{
	uint32_t rec = ctx->a[0];
	uint16_t to = (uint16_t)ctx->d[1];
	RAM16(rec + OBJ_ROOM) = to;
	cc_logic16(ctx, to);
	if (cmpw(ctx, to, RAM16(RAM_ROOM_INDEX)) == 0) {
		cc_logic16(ctx, opw(ctx, 2));
		if (!(opw(ctx, 2) & 0x8000)) {
			uint8_t f = RAM8(rec + OBJ_FLAGS) | 1;         /* draw it */
			RAM8(rec + OBJ_FLAGS) = f;
			cc_logic8(ctx, f);
		}
	}
	bool add = false;
	if (cmpw(ctx, ctx->d[0], 1) == 0) {
		/* taken out of the inventory: remove it from the list */
		set_w(&ctx->d[0], opw(ctx, 2) & 0x7FFF);
		set_w(&ctx->d[0], cc_sub(ctx, ctx->d[0], 3, 16, true));
		ctx->a[0] = RAM_INV_LIST;
		do {
			cmpw(ctx, ctx->d[0], RAM16(ctx->a[0]));
			ctx->a[0] += 2;
		} while (!(ctx->sr & CC_Z));
		uint16_t v;
		do {
			v = RAM16(ctx->a[0]);
			ctx->a[0] += 2;
			RAM16(ctx->a[0] - 4) = v;
			cc_logic16(ctx, v);
		} while (!(v & 0x8000));
		if (cmpw(ctx, ctx->d[1], 1) != 0)
			goto refresh;
		add = true;
	} else {
		add = cmpw(ctx, ctx->d[1], 1) == 0;
	}
	if (!add) {
		move_obj_skip(ctx);
		return;
	}
	/* into the inventory: append, and show the last row */
	ctx->a[0] = RAM_INV_LIST;
	ctx->d[0] = 0;
	cc_logic32(ctx, 0);
	for (;;) {
		uint16_t v = RAM16(ctx->a[0]);
		ctx->a[0] += 2;
		cc_logic16(ctx, v);
		if (v & 0x8000)
			break;
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 1, 16));
	}
	set_w(&ctx->d[1], opw(ctx, 2) & 0x7FFF);
	set_w(&ctx->d[1], cc_sub(ctx, ctx->d[1], 3, 16, true));
	RAM16(ctx->a[0] - 2) = (uint16_t)ctx->d[1];
	RAM16(ctx->a[0]) = 0xFFFF;
	cc_logic16(ctx, 0xFFFF);
	set_w(&ctx->d[0], lsrw(ctx, ctx->d[0], 2));
	set_w(&ctx->d[1], RAM16(RAM_INV_PAGE));
	set_w(&ctx->d[1], cc_add(ctx, ctx->d[1], 1, 16));
	if (cmpw(ctx, ctx->d[1], ctx->d[0]) != 0) {
		RAM16(RAM_INV_PAGE) = (uint16_t)ctx->d[0];
		cc_logic16(ctx, ctx->d[0]);
	}
refresh:
	if (bit_test(ctx, RAM_VERB_STATE, 2)) {                /* bar hidden: no refresh */
		move_obj_skip(ctx);
		return;
	}
	cc_logic16(ctx, opw(ctx, 2));
	if (opw(ctx, 2) & 0x8000) {
		move_obj_skip(ctx);
		return;
	}
	hle_jump(ctx, 0x002ABA);
}

HLE_RANGE(0x0029EA, script_op_move_obj_wait, 60, 0x0029E4, MO_RTS)
{
	if (!bit_test(ctx, RAM_ACTORS_LOOPED, ctx->d[0])) {
		hle_jump(ctx, 0x0029E4);
		return;
	}
	RAM16(ctx->a[0] + OBJ_ROOM) = opw(ctx, 4);
	cc_logic16(ctx, opw(ctx, 4));
	move_obj_skip(ctx);
}

HLE_RANGE(0x002A2C, script_op_move_obj_static, 200, 0x002ABA, MO_RTS)
{
	move_obj_tail(ctx);
}

/* ------------------------------------------------------------------ anim_wait */
/* $002B5A op $06 anim_wait [06][who][anim]: start an animation and wait for it.
 *   Shaggy / Scooby: animation word ($FF0488 / $FF048A), start bit in $FF0AB5, then actor
 *   frames (jsr sub_00B0B8 at $002B8C) until VBlank has taken the start bit;
 *   object actors: animation $FF048C+2*slot, timer $FF061C+2*slot = -1, start bit, frames
 *   ($002BDC) until the animation looped.
 * Quirk: for objects the original then stores the animation into the state word of the
 * FIRST object record ($FF1200, `move.w 4(a5),0(a0)` without the index), not the target's;
 * an object without an actor slot only gets that store. */
#define RAM_ACTOR_TIMER2 0xFF061C
static void anim_wait_store(hle_ctx *ctx)
{
	uint16_t a = opw(ctx, 4);
	RAM16(ctx->a[0]) = a;                                  /* record 0, see above */
	cc_logic16(ctx, a);
	ctx->a[5] += 6;
	hle_jump(ctx, 0x002BF6);
}
HLE_RANGE(0x002B5A, script_op_anim_wait, 120, 0x002B8C, 0x002BDC, 0x002BF6)
{
	if (!exec_mode(ctx)) {
		ctx->a[5] += 6;
		hle_jump(ctx, 0x002BF6);
		return;
	}
	set_w(&ctx->d[0], opw(ctx, 2));
	set_w(&ctx->d[0], cc_sub(ctx, ctx->d[0], 1, 16, true));
	uint16_t anim = opw(ctx, 4);
	if ((uint16_t)ctx->d[0] == 0) {                        /* Shaggy */
		RAM16(RAM_ACTOR_ANIM) = anim;
		ctx->d[1] = 0;
		cc_logic32(ctx, 0);
	} else {
		set_w(&ctx->d[0], cc_sub(ctx, ctx->d[0], 1, 16, true));
		if ((uint16_t)ctx->d[0] != 0) {                    /* an object */
			ctx->d[0] = obj_mul(ctx, ctx->d[0], 1);
			ctx->a[0] = RAM_OBJECTS;
			uint8_t slot = RAM8(RAM_OBJECTS + (int16_t)ctx->d[0] + OBJ_ACTOR);
			set_b(&ctx->d[1], slot);
			cc_logic8(ctx, slot);
			if (slot & 0x80) {                             /* no actor */
				anim_wait_store(ctx);
				return;
			}
			set_w(&ctx->d[1], slot);
			ctx->a[1] = RAM_ACTOR_ANIM + 4;
			set_w(&ctx->d[2], slot);
			set_w(&ctx->d[2], cc_add(ctx, ctx->d[2], ctx->d[2], 16));
			RAM16(RAM_ACTOR_ANIM + 4 + (int16_t)ctx->d[2]) = anim;
			ctx->a[1] = RAM_ACTOR_TIMER2;
			RAM16(RAM_ACTOR_TIMER2 + (int16_t)ctx->d[2]) = 0xFFFF;
			set_w(&ctx->d[1], cc_add(ctx, ctx->d[1], 2, 16));
			bit_set(ctx, RAM_ACTOR_START, ctx->d[1]);
			hle_jump(ctx, 0x002BDC);
			return;
		}
		RAM16(RAM_ACTOR_ANIM + 2) = anim;                  /* Scooby */
		ctx->d[1] = 1;
		cc_logic32(ctx, 1);
	}
	bit_set(ctx, RAM_ACTOR_START, ctx->d[1]);
	hle_jump(ctx, 0x002B8C);
}
HLE_RANGE(0x002B92, script_op_anim_wait_started, 40, 0x002B8C, 0x002BF6)
{
	if (bit_test(ctx, RAM_ACTOR_START, ctx->d[1])) {       /* not taken yet */
		hle_jump(ctx, 0x002B8C);
		return;
	}
	ctx->a[5] += 6;
	hle_jump(ctx, 0x002BF6);
}
HLE_RANGE(0x002BE2, script_op_anim_wait_looped, 40, 0x002BDC, 0x002BF6)
{
	if (!bit_test(ctx, RAM_ACTORS_LOOPED, ctx->d[1])) {
		hle_jump(ctx, 0x002BDC);
		return;
	}
	anim_wait_store(ctx);
}

/* ------------------------------------------------------------------ TALK menu */
/* $001C0C talk_menu (sub_001C0C, called by action_execute for TALK): when the object script
 * has talk_choice ops, the verb bar slides away and up to 5 choice texts are listed in its
 * place; Up/Down move the red bar ($FF06F0 = text line of the bar, $FF06F2 = its height in
 * lines << 8), B or A picks. Picked choices are marked in $FF0874 and not listed again;
 * a choice's body may hold nested talk_choice ops (a sub-menu), the menu depth is $FF0810
 * with the parent's A5/A6 in $FF0812/$FF0816. Ported here: the set-up and the menu loop up
 * to the pick ($001E72). Running the picked choice (reply text, pose, body, nesting) stays
 * 68k: it is all stack pushes around blocking calls.
 *   $001C10 talk_menu_begin   no choices -> rts; clear the text line, hide the pointer
 *   $001C4A talk_menu_save    save the bar's name table, start the slide-out
 *   $001C9E talk_menu_slide   wait for the slide; remember A5/A6, depth 0, nothing used
 *   $001CDC talk_menu_level   (after collecting a level's choices) none left -> $00204A
 *   $001D22 talk_menu_draw    list the unused choices, put the bar on the first one
 *   $001DB8 talk_menu_input   Up/Down edges move the bar over unused choices
 *   $001E5A talk_menu_pick    B or A picks, else next frame */
#define RAM_TALK_LINES    0xFF081A   /* 5 words: text lines of each listed choice */
#define RAM_TALK_USED     0xFF0874   /* word: bit n = choice n already picked */
#define RAM_TALK_DEPTH    0xFF0810
#define RAM_TALK_A5       0xFF0812
#define RAM_TALK_A6       0xFF0816
#define RAM_MENU_BAR_Y    0xFF06F0
#define RAM_MENU_BAR_H    0xFF06F2
#define RAM_POINTER_HIDE  0xFF06EE
#define TEXT_LINE_CLEAR   0x00A054   /* sub_00A054: draw string A0 in the top text line */
#define VRAM_READ_WORD    0x009996
#define BAR_PLANE_FILL    0x0022F2   /* sub_0022F2: blank the bar rows at D4 */
#define CHOICE_DRAW       0x009FA2   /* sub_009FA2: draw choice text A0 at line D0, column D1 */

HLE_RANGE(0x001C10, talk_menu_begin, 120, 0x001C1A, 0x001C40)
{
	uint16_t n = RAM16(RAM_TALK_COUNT);
	cc_logic16(ctx, n);
	if (!n) {
		hle_jump(ctx, 0x001C1A);
		return;
	}
	RAM8(RAM_TEXT_BUF) = 0;
	ctx->a[0] = RAM_TEXT_BUF;
	hle_call68k(ctx, TEXT_LINE_CLEAR);
	RAM16(RAM_POINTER_HIDE) = 0xFFFF;
	cc_logic16(ctx, 0xFFFF);
	bit_set(ctx, RAM_VERB_STATE, 2);
	hle_jump(ctx, 0x001C40);                               /* spin until the bar is idle */
}

HLE_RANGE(0x001C4A, talk_menu_save, 9000, 0x001C98)
{
	bit_clr(ctx, VBLANK_FLAGS, 3);
	ctx->d[0] = 0;
	if (!bit_test(ctx, VBLANK_FLAGS, 4))
		set_w(&ctx->d[0], 0x40);
	set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 0xAA80, 16)); /* bar rows in the plane at $A000 */
	set_w(&ctx->d[4], (uint16_t)ctx->d[0]);
	ctx->a[0] = 0xFF2C00;
	set_w(&ctx->d[3], 5);
	for (;;) {
		set_w(&ctx->d[2], 0x1F);
		for (;;) {
			hle_call68k(ctx, VRAM_READ_WORD);
			RAM16(ctx->a[0]) = (uint16_t)ctx->d[1];
			ctx->a[0] += 2;
			set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 2, 16));
			uint16_t c = (uint16_t)(ctx->d[2] - 1);
			set_w(&ctx->d[2], c);
			if (c == 0xFFFF)
				break;
		}
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 0x40, 16));
		uint16_t r = (uint16_t)(ctx->d[3] - 1);
		set_w(&ctx->d[3], r);
		if (r == 0xFFFF)
			break;
	}
	hle_call68k(ctx, BAR_PLANE_FILL);
	RAM8(RAM_BAR_SLIDE) = 0x10;
	cc_logic8(ctx, 0x10);
	hle_jump(ctx, 0x001C98);
}

HLE_RANGE(0x001C9E, talk_menu_slide, 60, 0x001C98, 0x001CC4)
{
	uint8_t s = RAM8(RAM_BAR_SLIDE);
	cc_logic8(ctx, s);
	if (!(s & 0x80)) {
		hle_jump(ctx, 0x001C98);
		return;
	}
	RAM32_W(RAM_TALK_A5, ctx->a[5]);
	RAM32_W(RAM_TALK_A6, ctx->a[6]);
	RAM16(RAM_TALK_DEPTH) = 0;
	RAM16(RAM_TALK_USED) = 0;
	cc_logic16(ctx, 0);
	hle_jump(ctx, 0x001CC4);
}

HLE_RANGE(0x001CDC, talk_menu_level, 200, 0x00204A, 0x001CEA)
{
	uint16_t n = RAM16(RAM_TALK_COUNT);
	cc_logic16(ctx, n);
	if (!n) {
		hle_jump(ctx, 0x00204A);
		return;
	}
	hle_call68k(ctx, BAR_PLANE_FILL);
	hle_jump(ctx, 0x001CEA);
}

HLE_RANGE(0x001D22, talk_menu_draw, 3000, 0x001DB2)
{
	ctx->d[2] = 0;
	set_w(&ctx->d[6], 0);
	ctx->a[2] = RAM_TALK_TEXT;
	ctx->a[3] = RAM_TALK_LINES;
	set_w(&ctx->d[3], RAM16(RAM_TALK_COUNT));
	for (;;) {
		ctx->a[0] = bus_r32(ctx->a[2]);
		ctx->a[2] += 4;
		set_w(&ctx->d[0], (uint16_t)ctx->d[2]);
		ctx->d[1] = 0;
		if (bit_test(ctx, VBLANK_FLAGS, 4))
			set_w(&ctx->d[1], 0x20);                       /* inventory view: other half */
		if (!bit_test(ctx, RAM_TALK_USED, ctx->d[6])) {
			hle_call68k(ctx, CHOICE_DRAW);                 /* D0 = lines drawn */
			RAM16(ctx->a[3]) = (uint16_t)ctx->d[0];
			set_w(&ctx->d[2], cc_add(ctx, ctx->d[2], ctx->d[0], 16));
		}
		cc_logic16(ctx, RAM16(ctx->a[3]));
		ctx->a[3] += 2;
		set_w(&ctx->d[6], cc_add(ctx, ctx->d[6], 1, 16));
		uint16_t left = (uint16_t)cc_sub(ctx, ctx->d[3], 1, 16, true);
		set_w(&ctx->d[3], left);
		bool n = ctx->sr & CC_N, v = ctx->sr & CC_V;
		if ((ctx->sr & CC_Z) || n != v)                    /* ble */
			break;
		if (cmpw(ctx, ctx->d[2], 5) >= 0)                  /* 5 lines full */
			break;
	}
	ctx->d[2] = 0;
	ctx->a[0] = RAM_TALK_LINES;
	set_w(&ctx->d[0], RAM16(RAM_TALK_LINES));
	set_w(&ctx->d[0], cc_sub(ctx, ctx->d[0], 1, 16, true));
	set_w(&ctx->d[0], lslw(ctx, lslw(ctx, ctx->d[0], 4), 4));
	RAM16(RAM_MENU_BAR_H) = (uint16_t)ctx->d[0];
	RAM16(RAM_MENU_BAR_Y) = 0;
	RAM16(RAM_POINTER_HIDE) = 0;
	cc_logic16(ctx, 0);
	/* D3 = first unused choice, D6 = number of unused ones (scanned from 4 down) */
	ctx->d[0] = 4;
	ctx->d[6] = 0;
	cc_logic32(ctx, 0);
	for (;;) {
		if (!bit_test(ctx, RAM_TALK_USED, ctx->d[0])) {
			set_w(&ctx->d[3], (uint16_t)ctx->d[0]);
			cc_logic16(ctx, ctx->d[0]);
			set_w(&ctx->d[6], cc_add(ctx, ctx->d[6], 1, 16));
		}
		uint16_t c = (uint16_t)(ctx->d[0] - 1);
		set_w(&ctx->d[0], c);
		if (c == 0xFFFF)
			break;
	}
	bit_set(ctx, VBLANK_FLAGS, 0);                         /* $001DAA */
	hle_jump(ctx, 0x001DB2);
}

/* the bar position from D3 (current choice) and D2 (its first line) */
static void talk_menu_bar(hle_ctx *ctx)
{
	RAM16(RAM_MENU_BAR_Y) = (uint16_t)ctx->d[2];
	set_w(&ctx->d[0], (uint16_t)ctx->d[3]);
	set_w(&ctx->d[0], lslw(ctx, ctx->d[0], 1));
	set_w(&ctx->d[0], RAM16(ctx->a[0] + (int16_t)ctx->d[0]));
	set_w(&ctx->d[0], cc_sub(ctx, ctx->d[0], 1, 16, true));
	set_w(&ctx->d[0], lslw(ctx, lslw(ctx, ctx->d[0], 4), 4));
	RAM16(RAM_MENU_BAR_H) = (uint16_t)ctx->d[0];
	cc_logic16(ctx, ctx->d[0]);
	hle_jump(ctx, 0x001E4E);                               /* spin until VBlank */
}

HLE_RANGE(0x001DB8, talk_menu_input, 200, 0x001E4E)
{
	/* Up edge (pad active low: pressed now, not last frame): previous unused choice */
	if (!bit_test(ctx, PAD1_CUR, 0) && bit_test(ctx, PAD1_PREV, 0)) {
		set_w(&ctx->d[0], (uint16_t)ctx->d[3]);
		cc_logic16(ctx, ctx->d[0]);
		if ((uint16_t)ctx->d[0] != 0) {
			set_w(&ctx->d[0], cc_sub(ctx, ctx->d[0], 1, 16, true));
			for (;;) {
				if (!bit_test(ctx, RAM_TALK_USED, ctx->d[0])) {
					set_w(&ctx->d[3], (uint16_t)ctx->d[0]);
					cc_logic16(ctx, ctx->d[0]);
					set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], ctx->d[0], 16));
					set_w(&ctx->d[2], cc_sub(ctx, ctx->d[2],
					      RAM16(ctx->a[0] + (int16_t)ctx->d[0]), 16, true));
					talk_menu_bar(ctx);
					return;
				}
				set_w(&ctx->d[0], cc_sub(ctx, ctx->d[0], 1, 16, true));
				if (ctx->d[0] & 0x8000)
					break;
			}
		}
	}
	/* Down edge: next unused choice */
	if (!bit_test(ctx, PAD1_CUR, 1) && bit_test(ctx, PAD1_PREV, 1)) {
		set_w(&ctx->d[0], (uint16_t)ctx->d[3]);
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 1, 16));
		if (cmpw(ctx, ctx->d[0], RAM16(RAM_TALK_COUNT)) < 0 && cmpw(ctx, ctx->d[0], 5) < 0) {
			for (;;) {
				if (!bit_test(ctx, RAM_TALK_USED, ctx->d[0])) {
					set_w(&ctx->d[3], cc_add(ctx, ctx->d[3], ctx->d[3], 16));
					set_w(&ctx->d[2], cc_add(ctx, ctx->d[2],
					      RAM16(ctx->a[0] + (int16_t)ctx->d[3]), 16));
					set_w(&ctx->d[3], (uint16_t)ctx->d[0]);
					cc_logic16(ctx, ctx->d[0]);
					break;
				}
				set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 1, 16));
				if (cmpw(ctx, ctx->d[0], 5) == 0)
					break;
			}
		}
	}
	talk_menu_bar(ctx);
}

HLE_RANGE(0x001E5A, talk_menu_pick, 60, 0x001DB2, 0x001E7A)
{
	if (bit_test(ctx, PAD1_CUR, 4) && bit_test(ctx, PAD1_CUR, 6)) {   /* neither B nor A */
		bit_set(ctx, VBLANK_FLAGS, 0);                     /* next frame: $001DAA */
		hle_jump(ctx, 0x001DB2);
		return;
	}
	RAM16(RAM_POINTER_HIDE) = 0xFFFF;
	cc_logic16(ctx, 0xFFFF);
	hle_jump(ctx, 0x001E7A);
}
