/* Text and dialogue rendering (GitLab #20; documented in docs/subsystems/text.md).
 *
 * The game never draws text with sprites. Strings are plain ASCII (NUL terminated); a glyph is
 * the tile  char + ram_font_base  (the 94-tile font $030F12 is loaded after the room tiles by
 * font_tiles_load, room.c). The glyph writers are in vdp.c (#13): draw_status_line $00A054
 * draws the WINDOW plane ($D000, 32 cells wide: status line, room name, every dialogue box;
 * wraps at 30 characters, centres, sets the window height), draw_text_wrapped $009FA2 the TALK
 * menu lines, draw_text_plane_a $009F72 and friends the plane-A menus.
 *
 * A line is "shown" by text_show ($00A130 = print, wait, clear). The wait ($00A18C text_wait)
 * is a timer proportional to the text length that A or B (or Start in a skippable cutscene)
 * cuts short; it keeps the actors animating (sub_00B0B8) and runs the object events, so it
 * waits for VBlank and is built from HLE_RANGEs (docs/porting.md, "Waiting for VBlank").
 * Script op $10 `say` is in the second half of this file.
 *
 * Every function keeps the exact register and flag contract of the 68k routine (difftest
 * compares them), with the logic written out in readable steps. All callers that stay in 68k
 * (title screens, TALK menu, action_execute, the main loop) keep working unchanged.
 */
#include "game.h"
#include "cc68k.h"

/* ---- RAM ---------------------------------------------------------------------------- */
#define RAM_FONT_BASE      0xFF068C   /* word: tile base added to a character (palette bits 13-14, priority bit 15) */
#define RAM_MYSTERY_HDR    0xFF068E   /* long: active mystery header */
#define RAM_BLANK_TILE     0xFF08B8   /* word: name-table word of the empty window cell (UI tile base + blank) */
#define RAM_TEXT_BUF       0xFF08BC   /* status line / failure text buffer (NUL terminated, max ~60 bytes) */
#define RAM_TEXT_TIMER     0xFF080A   /* word: frames*2 left of the current text (see text_wait) */
#define RAM_TEXT_SPEED     0xFF080C   /* word: byte offset into the pacing table $00A182 (always 4) */
#define RAM_WINDOW_ROWS    0xFF0AD1   /* byte: VDP reg $12 (window height) as the HBlank hook sets it */
#define RAM_VBLANK_FLAGS   0xFF09DC
#define RAM_VERB_STATE     0xFF09DE   /* byte: bit1 = redraw the status line, bit4 = cutscene text (actors keep animating) */
#define RAM_PAD1           0xFF09E0   /* active low: bit6 = A, bit4 = B */
#define RAM_09EB           0xFF09EB
#define RAM_SCRIPT_FLAGS2  0xFF0ACA   /* bit0 = Start held, bit1 = cutscene skippable */
#define RAM_ACTORS_ANIMDONE 0xFF0ABC
#define RAM_ROOM_NAME_PTR  0xFF06D2   /* long */
#define RAM_ROOM_NAME_PREV 0xFF06D6   /* long: name shown by the previous pass */
#define RAM_VBLANK_FLAGS_PREV 0xFF09DD
#define RAM_TARGET_OBJ     0xFF06AE
#define RAM_HOVER_PREV     0xFF06B4
#define RAM_VERB_CUR       0xFF06C0
#define RAM_CUR_VERB_PREV  0xFF06B8
#define RAM_TARGET2_OBJ    0xFF06B6
#define RAM_SECOND_PREV    0xFF06BC
#define RAM_09E8           0xFF09E8   /* bit0: "give" verb (second object is introduced by "to") */

/* ---- ROM ---------------------------------------------------------------------------- */
#define ROM_VERB_NAMES     0x02FE5E   /* NUL-separated "Pull ", "Take ", ... indexed by verb id (0 = "") */
#define ROM_STR_WITH       0x02FFE8   /* " with " */
#define ROM_STR_TO         0x02FFEF   /* " to " */
#define ROM_BLANK_LINE     0x02FFF4   /* 34 spaces: clears a text line of plane A */
#define ROM_PACING_TABLE   0x00A182   /* 5 words 10,5,2,1,0: timer decrement per frame, by text speed */

/* ---- VDP map ------------------------------------------------------------------------ */
#define VRAM_PLANE_A       0xC000     /* 32 cells wide in the menus that use draw_text_plane_a */
#define VRAM_WINDOW        0xD000     /* window plane: 32 x 32 cells, text at row 1, col 1 */
#define VRAM_WINDOW_TEXT   0xD042
#define VRAM_TALK_MENU     0xAA82     /* verb-bar plane ($A000, 64 wide) row of the first TALK choice */
#define TEXT_COLS          30         /* characters per line */
#define VDP_REG_WINDOW_V   0x12

extern void object_name_ptr(hle_ctx *ctx);   /* verbs.c, $005B5E */

static inline uint16_t lo16(uint32_t r) { return (uint16_t)r; }

/* ------------------------------------------------------------------ strlen $00A236 */
/* D0.l = length of the NUL-terminated string at A0 (A0 unchanged). CCR: Z=1 (the NUL was
 * just tested), N=V=C=0; X = 0 if the string is not empty (addq.w #1,d0 never carries), else
 * unchanged. */
HLE_FUNC(0x00A236, str_length, 0)
{
	uint32_t n = 0;
	while (bus_r8(ctx->a[0] + n))
		n++;
	ctx->d[0] = n;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C | (n ? CC_X : 0), CC_Z);
	ctx->cycles = 60 + 34 * (int)n;
}

/* The glyph writers live in vdp.c (#13, they go through the renderer hook): draw_text_plane_a
 * $009F72 (+ the plane-A menu helpers $009F30/$009F46/$009F58), draw_text_wrapped $009FA2 (TALK
 * menu line) and draw_status_line $00A054 (window plane: status line / dialogue box, word wrap
 * at 30 characters, centred, window height). text.c uses draw_status_line for every box. */

/* ------------------------------------------------------------------ text_clear $00A172 */
/* Empty the status/dialogue buffer and print it: clears the window plane (and resets the
 * window height to 1 text row + blank). A0 = RAM_TEXT_BUF afterwards. */
HLE_FUNC(0x00A172, text_clear, 0)
{
	ctx->a[0] = RAM_TEXT_BUF;
	RAM8(RAM_TEXT_BUF) = 0;
	draw_status_line(ctx);
}

/* ------------------------------------------------------------------ text_show $00A130 */
/* Show the string at A0 as a dialogue line and wait until it is done:
 *     pointer-mode bit saved, pointer mode off  (the bar must not react while text is up)
 *     draw_status_line(a0)            print the box
 *     text_wait()                   timer / button, actors keep moving     <- blocks (68k)
 *     text_clear()                  empty the box again
 *     window height reg $12 = 2 (the status line), pointer-mode bit restored,
 *     verb_state bit1 = "redraw the status line"
 * The pushes around the blocking call keep the 68k's stack, so it is four ranges that exit
 * in front of the push, the wait call, the pop and the final rts. */
HLE_RANGE(0x00A130, text_show, 24, 0x00A13A)
{
	uint8_t f = RAM8(RAM_VBLANK_FLAGS);
	uint8_t r = f & 8;
	set_b(&ctx->d[0], r);                              /* move.b / andi.b #8,d0 */
	cc_logic8(ctx, r);
	hle_jump(ctx, 0x00A13A);                           /* movew d0,-(sp) */
}
HLE_RANGE(0x00A13C, text_show_print, 60, 0x00A148)
{
	cc_btst(ctx, RAM8(RAM_VBLANK_FLAGS), 3);
	RAM8(RAM_VBLANK_FLAGS) &= ~8;                      /* bclr #3: pointer mode off */
	draw_status_line(ctx);
	ctx->cycles += 20;
	hle_jump(ctx, 0x00A148);                           /* bsr text_wait */
}
HLE_RANGE(0x00A14C, text_show_clear, 60, 0x00A160)
{
	text_clear(ctx);
	vdp_ctrl_w(0x9200 | 2);                            /* window height back to 2 rows */
	RAM8(RAM_WINDOW_ROWS) = 2;
	cc_logic8(ctx, 2);
	ctx->cycles += 40;
	hle_jump(ctx, 0x00A160);                           /* movew (sp)+,d0 */
}
HLE_RANGE(0x00A162, text_show_done, 44, 0x00A170)
{
	uint8_t v = RAM8(RAM_VBLANK_FLAGS) | (uint8_t)ctx->d[0];
	RAM8(RAM_VBLANK_FLAGS) = v;                        /* restore pointer mode */
	cc_logic8(ctx, v);
	cc_btst(ctx, RAM8(RAM_VERB_STATE), 1);
	RAM8(RAM_VERB_STATE) |= 2;                         /* status line needs redrawing */
	hle_jump(ctx, 0x00A170);                           /* rts */
}

/* ------------------------------------------------------------------ text_wait $00A18C */
/* The "line is on screen" wait. RAM_TEXT_TIMER (= length * 16, set by draw_status_line) is
 * halved, then every frame reduced by the pacing table entry (10,5,2,1,0 selected by
 * RAM_TEXT_SPEED, always 4 -> 2): a 30-character line stays 30*8/2 = 120 frames. Nothing is
 * typed out: the whole text appears at once. The box goes away when
 *   - the timer runs out (unsigned borrow), or
 *   - A or B is pressed (after the previous press was released first), or
 *   - Start is held in a skippable cutscene (script_flags2 bits 0 and 1).
 * Each frame the actors animate (sub_00B0B8) and object events run; in a cutscene text
 * (verb_state bit4) sub_0020DA also runs once the actors finish a move.
 *
 *   $A18C  lsr.w timer ; spin while A or B held          -> exit A192 (68k spin on the pad)
 *   $A1AA  bsr sub_00B0B8 ; bsr run_object_events         (68k calls)
 *   $A1B2  range: Start-skip test, cutscene call          -> exits A234 / A1E2 / A1E8
 *   $A1E8  bsr wait_vblank                                (68k)
 *   $A1EC  range: timer -= table, button tests            -> exits A234, A1AA, A20C, A228 */
HLE_RANGE(0x00A18C, text_wait, 21, 0x00A192)
{
	uint16_t v = RAM16(RAM_TEXT_TIMER);
	RAM16(RAM_TEXT_TIMER) = (uint16_t)lsr16(ctx, v, 1);
	hle_jump(ctx, 0x00A192);
}
HLE_RANGE(0x00A1B2, text_wait_frame, 93, 0x00A234, 0x00A1E2, 0x00A1E8)
{
	uint8_t sf = RAM8(RAM_SCRIPT_FLAGS2);
	cc_btst(ctx, sf, 1);
	if (sf & 2) {
		cc_btst(ctx, sf, 0);
		if (sf & 1) {                                  /* Start held while skippable: leave */
			hle_jump(ctx, 0x00A234);
			return;
		}
	}
	uint8_t vs = RAM8(RAM_VERB_STATE);
	cc_btst(ctx, vs, 4);
	if (vs & 0x10) {
		uint8_t ad = RAM8(RAM_ACTORS_ANIMDONE);
		cc_btst(ctx, ad, 0);
		if (ad & 1) {
			hle_jump(ctx, 0x00A1E2);                   /* jsr sub_0020DA (cutscene step) */
			return;
		}
	}
	hle_jump(ctx, 0x00A1E8);                           /* bsr wait_vblank */
}
HLE_RANGE(0x00A1EC, text_wait_tick, 121, 0x00A234, 0x00A1AA, 0x00A20C, 0x00A228)
{
	set_w(&ctx->d[0], RAM16(RAM_TEXT_SPEED));
	uint16_t step = ROM16(ROM_PACING_TABLE + (uint16_t)ctx->d[0]);
	set_w(&ctx->d[0], step);
	uint16_t t = RAM16(RAM_TEXT_TIMER);
	RAM16(RAM_TEXT_TIMER) = (uint16_t)cc_sub(ctx, t, step, 16, true);
	if (step > t) {                                    /* borrow: time is up */
		hle_jump(ctx, 0x00A234);
		return;
	}
	uint8_t pad = RAM8(RAM_PAD1);
	cc_btst(ctx, pad, 6);
	if (!(pad & 0x40)) {                               /* A pressed: wait for release, leave */
		hle_jump(ctx, 0x00A20C);
		return;
	}
	cc_btst(ctx, pad, 4);
	if (pad & 0x10)                                    /* B not pressed: next frame */
		hle_jump(ctx, 0x00A1AA);
	else
		hle_jump(ctx, 0x00A228);                       /* B pressed: wait for release, leave */
}

/* ------------------------------------------------------------------ status_line_update $00A828 */
/* The line at the top of the screen (window plane), built once per change:
 *   walking mode  (pointer mode off): the room name, redrawn when the room changed or the
 *                 previous pass was in pointer mode or verb_state bit1 asked for it
 *   pointer mode: "<Verb> <object>" or "<Verb> <object> with|to <object2>" into RAM_TEXT_BUF
 *                 (verb word from verb_names, object names from object_name_ptr, second
 *                 object only while verb_state bit0 = waiting for it; "to" for Give), or an
 *                 empty line when nothing is under the pointer. Only rebuilt when the
 *                 hovered object, verb, second object or bit0 changed since the last pass.
 * Registers/flags out are those of the original paths (each exit documented inline). */
HLE_FUNC(0x00A828, status_line_update, 150)
{
	uint32_t *d = ctx->d;
	bool rebuild;

	if (!(RAM8(RAM_VBLANK_FLAGS) & 8)) {
		/* ---- walking: room name */
		uint8_t s9 = RAM8(RAM_09EB);
		cc_btst(ctx, s9, 5);
		if (s9 & 0x20)                                 /* text/cutscene owns the window */
			return;
		uint8_t vs = RAM8(RAM_VERB_STATE);
		cc_btst(ctx, vs, 1);
		if (vs & 2) {
			RAM8(RAM_VERB_STATE) = vs & ~2;
			ctx->a[0] = RAM32(RAM_ROOM_NAME_PTR);
		} else {
			ctx->a[0] = RAM32(RAM_ROOM_NAME_PTR);
			uint8_t pf = RAM8(RAM_VBLANK_FLAGS_PREV);
			cc_btst(ctx, pf, 3);
			if (!(pf & 8)) {
				cc_sub(ctx, ctx->a[0], RAM32(RAM_ROOM_NAME_PREV), 32, false);   /* cmpa.l */
				if (ctx->a[0] == RAM32(RAM_ROOM_NAME_PREV))
					return;                            /* same room as last pass */
			}
		}
		draw_status_line(ctx);
		return;
	}

	/* ---- pointer mode: verb + object(s) */
	uint8_t vs = RAM8(RAM_VERB_STATE);
	cc_btst(ctx, vs, 1);
	if (vs & 2) {
		RAM8(RAM_VERB_STATE) = vs & ~2;
		rebuild = true;
	} else {
		uint8_t pf = RAM8(RAM_VBLANK_FLAGS_PREV);
		cc_btst(ctx, pf, 3);
		rebuild = true;
		if (pf & 8) {                                  /* previous pass was pointer mode too: unchanged? */
			rebuild = false;
			set_w(&d[0], RAM16(RAM_TARGET_OBJ));
			cc_sub(ctx, d[0], RAM16(RAM_HOVER_PREV), 16, false);
			if (lo16(d[0]) != RAM16(RAM_HOVER_PREV)) rebuild = true;
			else {
				set_w(&d[0], RAM16(RAM_VERB_CUR));
				cc_sub(ctx, d[0], RAM16(RAM_CUR_VERB_PREV), 16, false);
				if (lo16(d[0]) != RAM16(RAM_CUR_VERB_PREV)) rebuild = true;
				else {
					set_w(&d[0], RAM16(RAM_TARGET2_OBJ));
					cc_sub(ctx, d[0], RAM16(RAM_SECOND_PREV), 16, false);
					if (lo16(d[0]) != RAM16(RAM_SECOND_PREV)) rebuild = true;
				}
			}
			if (!rebuild) {
				/* bit0 of verb_state now vs before: eor.w d0,d1 */
				uint16_t b0 = RAM8(RAM_VERB_STATE) & 1, b1 = RAM8(RAM_VERB_STATE + 1) & 1;
				set_w(&d[0], b0);
				set_w(&d[1], b0);
				set_w(&d[1], b1);
				set_w(&d[1], b0 ^ b1);
				cc_logic16(ctx, b0 ^ b1);
				if (b0 ^ b1) rebuild = true;
				else return;
			}
		}
	}
	(void)rebuild;

	/* A8E4: build the text */
	cc_logic16(ctx, RAM16(RAM_TARGET_OBJ));
	if (RAM16(RAM_TARGET_OBJ) == 0) {
		RAM8(RAM_TEXT_BUF) = 0;                        /* nothing under the pointer: empty line */
	} else {
		ctx->a[1] = RAM_TEXT_BUF;
		ctx->a[0] = ROM_VERB_NAMES;
		set_w(&d[0], RAM16(RAM_VERB_CUR));
		if (lo16(d[0]) != 0) {                         /* skip to the verb_cur-th word */
			uint16_t n = lo16(d[0]) - 1;
			for (;; ) {
				while (bus_r8(ctx->a[0]++)) { }
				if (n-- == 0) break;
			}
			set_w(&d[0], 0xFFFF);
		}
		str_copy(ctx);                                 /* "Take " */
		set_w(&d[0], RAM16(RAM_TARGET_OBJ));
		ctx->a[1]--;
		ctx->a[2] = ctx->a[1];
		object_name_ptr(ctx);                          /* A0 = name of the hovered object */
		ctx->a[1] = ctx->a[2];
		str_copy(ctx);
		ctx->a[1]--;
		if (RAM8(RAM_VERB_STATE) & 1) {                /* waiting for the second object */
			ctx->a[0] = (RAM8(RAM_09E8) & 1) ? ROM_STR_TO : ROM_STR_WITH;
			str_copy(ctx);
			ctx->a[1]--;
			ctx->a[2] = ctx->a[1];
			set_w(&d[0], RAM16(RAM_TARGET2_OBJ));
			cc_logic16(ctx, d[0]);
			if (lo16(d[0]) != 0) {
				object_name_ptr(ctx);
				ctx->a[1] = ctx->a[2];
				str_copy(ctx);
			}
		}
	}
	ctx->a[0] = RAM_TEXT_BUF;
	draw_status_line(ctx);
}

/* ================================================================== script op $10: say */
/* `say` (execute mode; script.c's dispatcher calls $004D80 with A5 = op, scan mode skips it):
 *     10 | FLAGS | TEXT_OFF.l        8 bytes
 *   FLAGS bits 0-3  glyph colour (palette index of the font pixels)
 *         bits 4-5  palette line of the text (-> bits 13-14 of the font base word)
 *         bit 12    cutscene text: the actors keep animating while it is up
 *                   (anim-done bit 0 and verb_state bit 4 set for the duration)
 *         bit 13    Shaggy "talks": bits 14-15 = facing, a pose from $0FC740 plays before and
 *                   after the box and the old facing is restored
 *   TEXT_OFF.l      string = header+$20 text pool + offset
 * Steps: [pose], empty the box, recolour the font (font_tiles_load with the glyph colour and
 * the line's palette bits), text_show, bar state restore, font back to colour 15, [pose back].
 * The three waits for the pose animation and the text_show wait are 68k, so the op is a chain
 * of ranges between pushes/blocking calls. */
#define RAM_SHAGGY_FACING  0xFF05B8
#define RAM_CUR_INDEX      0xFF0488   /* animation selector of the actor being driven */
#define RAM_0AB5           0xFF0AB5   /* bit0 = actor 0 (Shaggy) animation request */
#define ROM_POSE_TABLE     0x0FC740   /* [new facing * 2 + old facing * 8] -> animation index */
#define SAY_CUTSCENE       0x1000
#define SAY_POSE           0x2000

static inline uint16_t say_word(hle_ctx *c) { return bus_r16(c->a[5] + 2); }

/* "move.w pose(facing...),cur_index ; bset #0,0ab5": request the pose animation. D0 = index into
 * the table (facing*2 + oldfacing*8). Leaves A0 = table. */
static void pose_request(hle_ctx *c, uint16_t idx)
{
	c->a[0] = ROM_POSE_TABLE;
	RAM16(RAM_CUR_INDEX) = bus_r16(ROM_POSE_TABLE + (int16_t)idx);
	cc_btst(c, RAM8(RAM_0AB5), 0);
	RAM8(RAM_0AB5) |= 1;
}

/* $004E12: clear the box before the text, keep the old font base for later (pushed by the 68k) */
static void say_clear_box(hle_ctx *c)
{
	text_clear(c);
	set_w(&c->d[2], RAM16(RAM_FONT_BASE));
	cc_logic16(c, RAM16(RAM_FONT_BASE));                 /* move.w sets N/Z */
	c->cycles += 40;
	hle_jump(c, 0x004E1C);                                 /* movew d2,-(sp) */
}

HLE_RANGE(0x004D80, script_op_say, 20, 0x004EDE, 0x004D84)
{
	if (ctx->sr & CC_Z)
		hle_jump(ctx, 0x004EDE);                       /* scan mode: skip the op */
	else
		hle_jump(ctx, 0x004D84);                       /* movew d7,-(sp) */
}
HLE_RANGE(0x004D86, script_op_say_begin, 60, 0x004DBE, 0x004E1C)
{
	uint16_t w = say_word(ctx);
	set_w(&ctx->d[0], w);
	cc_logic16(ctx, w);
	cc_btst(ctx, w, 12);
	if (w & SAY_CUTSCENE) {
		cc_btst(ctx, RAM8(RAM_ACTORS_ANIMDONE), 0);
		RAM8(RAM_ACTORS_ANIMDONE) |= 1;
		cc_btst(ctx, RAM8(RAM_VERB_STATE), 4);
		RAM8(RAM_VERB_STATE) |= 0x10;
	}
	cc_btst(ctx, w, 13);
	if (!(w & SAY_POSE)) {
		say_clear_box(ctx);
		return;
	}
	/* new facing = bits 14-15; old one goes on the 68k stack (exit in front of the push) */
	uint16_t f = (uint16_t)(((w << 2) | (w >> 14)) & 3);   /* rol.w #2 ; andi.w #3 */
	cc_logic16(ctx, f);
	set_w(&ctx->d[0], f);
	set_w(&ctx->d[1], RAM16(RAM_SHAGGY_FACING));
	RAM16(RAM_SHAGGY_FACING) = f;
	set_w(&ctx->d[0], (uint16_t)(f + f));              /* addw d0,d0 */
	hle_jump(ctx, 0x004DBE);                           /* movew d1,-(sp) */
}
HLE_RANGE(0x004DC0, script_op_say_pose_in, 40, 0x004DDA)
{
	uint16_t old = lo16(ctx->d[1]);
	set_w(&ctx->d[1], (uint16_t)(old << 3));
	uint16_t idx = (uint16_t)(lo16(ctx->d[0]) + lo16(ctx->d[1]));
	set_w(&ctx->d[0], idx);
	pose_request(ctx, idx);
	hle_jump(ctx, 0x004DDA);                           /* bsr sub_00B0B8 */
}
/* Not ported (no captured states, i.e. no route plays the Shaggy pose): the pose-wait loops
 * $004DDE/$004DEE and the pose-back sequence $004E82..$004EDA stay 68k.
 * $004E1E: set up the font for this line and print: A0 = text. Both font_tiles_load calls
 * cost ~1.02 M cycles each (charged in full, the layer slices it). */
HLE_RANGE(0x004E1E, script_op_say_text, 120, 0x004E50)
{
	uint16_t w = say_word(ctx);
	uint16_t pal = (uint16_t)(((w & 0x30) << 5) << 4);   /* line bits 4-5 -> bits 13-14 */
	uint16_t fb = (RAM16(RAM_FONT_BASE) & 0x9FFF) | pal;
	set_w(&ctx->d[0], w);
	set_w(&ctx->d[1], pal);
	set_w(&ctx->d[2], fb);
	RAM16(RAM_FONT_BASE) = fb;
	set_w(&ctx->d[0], w & 0xF);
	ctx->d[1] = 1;                                     /* moveq #1,d1 : background colour */
	font_tiles_load(ctx);
	ctx->a[0] = RAM32(RAM_MYSTERY_HDR);
	ctx->a[0] = bus_r32(ctx->a[0] + 0x20);
	ctx->a[0] += bus_r32(ctx->a[5] + 4);
	ctx->cycles += 120;
	hle_jump(ctx, 0x004E50);                           /* bsr text_show */
}
HLE_RANGE(0x004E54, script_op_say_after, 40, 0x004E5C)
{
	cc_btst(ctx, RAM8(RAM_VERB_STATE), 4);
	RAM8(RAM_VERB_STATE) &= ~0x10;                     /* cutscene text over */
	hle_jump(ctx, 0x004E5C);                           /* movew (sp)+,d2 */
}
HLE_RANGE(0x004E5E, script_op_say_font_back, 120, 0x004EDC, 0x004E80)
{
	RAM16(RAM_FONT_BASE) = lo16(ctx->d[2]);            /* the saved font base */
	set_w(&ctx->d[0], 0xF);
	ctx->d[1] = 1;
	font_tiles_load(ctx);                              /* glyph colour 15 again */
	uint16_t w = say_word(ctx);
	set_w(&ctx->d[0], w);
	cc_btst(ctx, w, 13);
	ctx->cycles += 120;
	if (!(w & SAY_POSE)) {
		hle_jump(ctx, 0x004EDC);                       /* movew (sp)+,d7 ; lea 8,a5 ; rts */
		return;
	}
	set_w(&ctx->d[1], RAM16(RAM_SHAGGY_FACING));
	hle_jump(ctx, 0x004E80);                           /* movew (sp)+,d0 : the old facing */
}
