/* Room entry, the room fade-in and small helpers that were still 68k (GitLab #24).
 *
 *   $002138 room_enter      load a room with interrupts off (room_load $758A), then let the
 *                           actors settle and run the room's load script
 *   $002196 room_change     room_enter, then falls into room_fade_in (glue.c: bsr $2138)
 *   $00219A room_fade_in    status line, fade in, remember the main loop's "previous" state,
 *                           run the arrival script
 *   $001BDA verb_labels_refresh  repaint the selected and the default verb label
 *   $0022D6 rng_next        the random generator
 *   $0047AC bar_save        save the verb bar's plane rows and blank them (before a picture)
 *   $00A248 game_init       boot: mystery 0, seed the generator
 *
 * room_enter and room_fade_in wait (actor animation, wait_vblank, palette_fade, scripts), so
 * they are ranges cut at those calls (docs/porting.md "Waiting for VBlank").
 */
#include "game.h"
#include "cc68k.h"
#include "glue.h"

#define RAM_RNG_STATE     0xFF0428
#define RAM_INV_BUSY      0xFF0AB6   /* b: the inventory animation is running */
#define RAM_STORY_FLAGS   0xFF2A00   /* b: bit 1 = the verb bar is hidden */
#define RAM_VERB_STATE    0xFF09DE
#define RAM_MENU_FLAGS    0xFF09ED
#define RAM_PAD1          0xFF09E0

/* ------------------------------------------------------------------ room_enter */
/* $002138: movem.l d0-d7/a0-a6,-(sp); load the room with interrupts off; release the pad state;
 * re-enable the object timers and the VBlank frame work; interrupts on; then, unless the verb
 * bar is hidden, redraw the inventory arrows (inventory_arrows_update waits) */
HLE_RANGE(0x002138, room_enter, 120, HLE_EXIT_SP(0x005B92, -64), HLE_EXIT_SP(0x002178, -60))
{
	movem_push_l(ctx, MOVEM_ALL);
	ctx->sr = 0x2700;
	RAM8(RAM_PAD1) = 0xFF;                             /* no button held */
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 3);
	RAM8(VBLANK_FLAGS) &= (uint8_t)~VBF_POINTER;
	hle_call68k(ctx, 0x00758A);                        /* room_load */
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 2);
	RAM8(VBLANK_FLAGS) |= VBF_OBJ_TIMERS;
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 6);
	RAM8(VBLANK_FLAGS) |= VBF_FRAME_WORK;
	ctx->sr = 0x2000;
	cc_btst(ctx, RAM8(RAM_STORY_FLAGS), 1);
	if (RAM8(RAM_STORY_FLAGS) & 2) {
		hle_jump(ctx, 0x002178);
		return;
	}
	glue_call(ctx, 0x005B92, 0x002178);
}

GLUE_CALL(0x002178, 6, 0x00B0B8)                       /* jsr actor_animate */
GLUE_CALL(0x00217E, 4, 0x00974C)                       /* bsr wait_vblank */

/* $002182: tst.b (RAM_INV_BUSY) / bne $217E: one more frame while the inventory animates */
HLE_RANGE(0x002182, room_enter_wait_inventory, 20, 0x00217E, 0x00218C)
{
	cc_logic8(ctx, RAM8(RAM_INV_BUSY));
	hle_jump(ctx, RAM8(RAM_INV_BUSY) ? 0x00217E : 0x00218C);
}

GLUE_CALL(0x00218C, 4, 0x00221A)                       /* bsr run_load_script */

/* $002190: movem.l (sp)+,d0-d7/a0-a6 / rts */
HLE_FUNC(0x002190, room_enter_end, 140) { movem_pop_l(ctx, MOVEM_ALL); }

GLUE_CALL(0x002196, 4, 0x002138)                       /* room_change: bsr room_enter, then */
                                                       /* falls into room_fade_in */

/* ------------------------------------------------------------------ room_fade_in */
/* $00219A: movem.l d0-d7/a0-a6,-(sp); the status line, then wait a frame */
HLE_RANGE(0x00219A, room_fade_in, 160, HLE_EXIT_SP(0x00974C, -64))
{
	movem_push_l(ctx, MOVEM_ALL);
	cc_btst(ctx, RAM8(RAM_VERB_STATE), 1);
	RAM8(RAM_VERB_STATE) |= 2;
	hle_call68k(ctx, 0x00A828);                        /* status_line_update */
	glue_call(ctx, 0x00974C, 0x0021B0);
}

/* $0021B0: fade the room's palette in with the VBlank frame work paused */
HLE_RANGE(0x0021B0, room_fade_in_fade, 30, HLE_EXIT_SP(0x009E7E, -4))
{
	ctx->a[0] = 0xFF0778;
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 6);
	RAM8(VBLANK_FLAGS) &= (uint8_t)~VBF_FRAME_WORK;
	glue_call(ctx, 0x009E7E, 0x0021C2);                /* palette_fade */
}

/* $0021C2: frame work back on; the main loop's change detection starts from this state; then
 * the arrival script (it may wait) */
HLE_RANGE(0x0021C2, room_fade_in_done, 140, HLE_EXIT_SP(0x002278, -4))
{
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 6);
	RAM8(VBLANK_FLAGS) |= VBF_FRAME_WORK;
	RAM16(0xFF06B4) = RAM16(0xFF06AE);                 /* HOVER_PREV = HOVER_TARGET */
	RAM8(0xFF09DD) = RAM8(VBLANK_FLAGS);               /* VBLANK_FLAGS_PREV */
	RAM8(0xFF09DF) = RAM8(RAM_VERB_STATE);             /* VERB_STATE_PREV */
	RAM32_W(0xFF06D6, RAM32(0xFF06D2));                /* ROOM_NAME_PREV */
	RAM16(0xFF06B4) = RAM16(0xFF06AE);
	RAM16(0xFF06B8) = RAM16(0xFF06C0);                 /* CUR_VERB_PREV */
	RAM16(0xFF06BC) = RAM16(0xFF06B6);                 /* SECOND_PREV */
	cc_logic16(ctx, RAM16(0xFF06BC));
	glue_call(ctx, 0x002278, 0x002214);                /* run_arrival_script */
}

/* $002214: movem.l (sp)+,d0-d7/a0-a6 / rts */
HLE_FUNC(0x002214, room_fade_in_end, 140) { movem_pop_l(ctx, MOVEM_ALL); }

/* ------------------------------------------------------------------ helpers */
/* $001BDA: repaint the label of the selected verb and of the default verb (verb_label_redraw
 * with ram_verb_to_draw = each), all registers saved */
HLE_FUNC(0x001BDA, verb_labels_refresh, 120)
{
	uint32_t sp = ctx->a[7];
	movem_push_l(ctx, MOVEM_ALL);
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 3);
	RAM8(VBLANK_FLAGS) &= (uint8_t)~VBF_POINTER;
	RAM16(0xFF06C2) = RAM16(0xFF06C0);                 /* VERB_TO_DRAW = CUR_VERB */
	ctx->d[0] = 0;
	hle_call68k(ctx, 0x005BDC);
	RAM16(0xFF06C2) = RAM16(0xFF06C4);                 /* VERB_TO_DRAW = DEFAULT_VERB */
	ctx->d[0] = 0;
	hle_call68k(ctx, 0x005BDC);
	movem_pop_l(ctx, MOVEM_ALL);
	ctx->a[7] = sp;
}

/* $0022D6 rng_next: state = ~rol(state, 1); d0 = (d0.w * state.w) >> 16, a number below d0 */
HLE_FUNC(0x0022D6, rng_next, 90)
{
	uint32_t s = RAM32(RAM_RNG_STATE);
	s = ~((s << 1) | (s >> 31));
	RAM32_W(RAM_RNG_STATE, s);
	ctx->d[1] = s;
	uint32_t p = (ctx->d[0] & 0xFFFF) * (s & 0xFFFF);
	ctx->d[0] = p >> 16;
	/* lsr.l #8 twice: X = C = bit 15 of the product, N = 0, Z, V = 0 */
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
	       ((p >> 15) & 1 ? CC_X | CC_C : 0) | (ctx->d[0] ? 0 : CC_Z));
}

/* $0047AC bar_save: copy the 6 x 32 cells of the verb bar (plane at $AA80, $AAC0 in the
 * inventory view) to $FF2C00 and fill them with the blank tile of the bar font */
HLE_FUNC(0x0047AC, bar_save, 9000)
{
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 3);
	RAM8(VBLANK_FLAGS) &= (uint8_t)~VBF_POINTER;
	ctx->d[0] = 0;
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 4);
	if (!(RAM8(VBLANK_FLAGS) & VBF_INVENTORY))
		set_w(&ctx->d[0], 0x40);
	set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 0xAA80, 16));
	set_w(&ctx->d[4], ctx->d[0]);
	cc_logic16(ctx, ctx->d[4]);
	ctx->a[0] = 0xFF2C00;
	set_w(&ctx->d[3], 5);
	do {
		set_w(&ctx->d[2], 0x1F);
		do {
			hle_call68k(ctx, 0x009996);                /* vram_read_word d0 -> d1 */
			RAM16(ctx->a[0]) = (uint16_t)ctx->d[1];
			cc_logic16(ctx, ctx->d[1]);
			ctx->a[0] += 2;
			set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 2, 16));
			set_w(&ctx->d[2], ctx->d[2] - 1);
		} while ((ctx->d[2] & 0xFFFF) != 0xFFFF);
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 0x40, 16));
		set_w(&ctx->d[3], ctx->d[3] - 1);
	} while ((ctx->d[3] & 0xFFFF) != 0xFFFF);
	set_w(&ctx->d[0], ctx->d[4]);
	cc_logic16(ctx, ctx->d[0]);
	set_w(&ctx->d[1], RAM16(0xFF068C) & 0x7FF);
	set_w(&ctx->d[1], cc_add(ctx, ctx->d[1], 0x20, 16));
	set_w(&ctx->d[3], 5);
	do {
		set_w(&ctx->d[2], 0x1F);
		do {
			hle_call68k(ctx, 0x009964);                /* vram_write_word d1 at d0 */
			set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 2, 16));
			set_w(&ctx->d[2], ctx->d[2] - 1);
		} while ((ctx->d[2] & 0xFFFF) != 0xFFFF);
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 0x40, 16));
		set_w(&ctx->d[3], ctx->d[3] - 1);
	} while ((ctx->d[3] & 0xFFFF) != 0xFFFF);
}

/* $00A248 game_init (boot, from Reset): wait until a pending VRAM fill has finished, then
 * mystery 0, the generator's seed and one step, the menu flags; falls into game_reset ($A27A).
 * The original reads the HV counter into d0 before that step (move.w (vdp_hv),d0 / bsr
 * rng_next). The step itself does not depend on d0 (the state is ~rol(state)); only the
 * returned number and the X flag do, and game_reset overwrites d0 (its palette loop) and the
 * flags before anything reads them. So the beam position never reaches game state, and the C
 * uses the constant 0 (docs/native.md, "Determinism"). */
#define GAME_INIT_HV 0x0000u

HLE_WAIT_RANGE(0x00A248, game_init, 80, 0x00A248, 0x00A27A)
{
	set_w(&ctx->d[0], vdp_status_r());
	cc_btst(ctx, ctx->d[0], 1);
	if (ctx->d[0] & 2) {                               /* DMA busy */
		hle_idle(ctx);
		hle_jump(ctx, 0x00A248);
		return;
	}
	RAM16(MYSTERY_IDX) = 0;
	RAM32_W(RAM_RNG_STATE, 0x45C3F2D1);
	set_w(&ctx->d[0], GAME_INIT_HV);
	hle_call68k(ctx, 0x0022D6);                        /* rng_next */
	RAM8(RAM_MENU_FLAGS) = 0;
	cc_logic8(ctx, 0);
	hle_jump(ctx, 0x00A27A);
}
