/* Waits and interrupt entry as C blocks (GitLab #24, docs/native.md "Waits", docs/remaining68k.md 5.1/5.4).
 *
 * The game has no frame-locked main loop: it waits for the VBlank interrupt in spin loops on
 * flags and countdowns that the VBlank handler changes (docs/subsystems/mainloop.md). Each spin
 * is a C block here: if the condition still holds it idles for the rest of the CPU's time slice
 * (hle_idle) and continues at its own address, so it runs again in the next slice, after the
 * frame loop has delivered the interrupt; otherwise it falls through like the original. The
 * flags at the exit are those of the last test, as in the original.
 *
 * The interrupt vectors are blocks too: the trampoline saves d0-a6, calls the RAM hook (C) at top
 * level and returns to its own tail block, which restores the registers and does the rte.
 */
#include "game.h"
#include "cc68k.h"
#include "glue.h"

#define RAM_FRAME_COUNTDOWN 0xFF08AE   /* w: VBlank decrements it; menus pace themselves with it */
#define RAM_ARROW_UP_TIMER  0xFF0AB1   /* b: inventory arrow redraw countdowns (VBlank, sub_00A704) */
#define RAM_ARROW_DN_TIMER  0xFF0AB2
#define RAM_INV_BUSY        0xFF0AB6   /* b: bit 0 = the inventory animation is running */
#define RAM_BAR_SLIDE       0xFF0ACB   /* b: verb-bar slide state; >= 0 while it moves */

/* ------------------------------------------------------------------ interrupt vectors */
/* $0008BC VBlank (level 6) and $0008AA HBlank (level 4):
 *     movem.l d0-d7/a0-a6,-(sp) / movea.l (hook),a0 / jsr (a0)     -> the hook, then
 *     movem.l (sp)+,d0-d7/a0-a6 / rte                               (the tail, $8C8 / $8B6)
 * The exception frame (SR, PC) was pushed by the interrupt itself. */
static void vector_enter(hle_ctx *ctx, uint32_t hook_var, uint32_t tail)
{
	movem_push_l(ctx, MOVEM_ALL);
	ctx->a[0] = RAM32(hook_var);
	glue_call(ctx, ctx->a[0], tail);
}

static void vector_leave(hle_ctx *ctx)
{
	movem_pop_l(ctx, MOVEM_ALL);
	ctx->sr = pop16(ctx);                        /* rte: SR, then PC */
	hle_jump(ctx, pop32(ctx));
}

HLE_FUNC(0x0008BC, vblank_vector, 164) { vector_enter(ctx, 0xFF0000, 0x0008C8); }
HLE_FUNC(0x0008C8, vblank_vector_rte, 152) { vector_leave(ctx); }
HLE_FUNC(0x0008AA, hblank_vector, 164) { vector_enter(ctx, 0xFF0004, 0x0008B6); }
HLE_FUNC(0x0008B6, hblank_vector_rte, 152) { vector_leave(ctx); }

/* ------------------------------------------------------------------ wait_vblank(s) */
/* $00974C wait_vblank: bset #0,(VBLANK_FLAGS); spin until VBlank clears it; rts */
HLE_FUNC(0x00974C, wait_vblank, 24)
{
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 0);
	RAM8(VBLANK_FLAGS) |= VBF_WAITING;
	hle_jump(ctx, 0x009754);
}

/* $009754: btst #0,(VBLANK_FLAGS) / bne $9754 / rts */
HLE_WAIT_FUNC(0x009754, wait_vblank_spin, 24)
{
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 0);
	if (RAM8(VBLANK_FLAGS) & VBF_WAITING) {
		hle_idle(ctx);
		hle_jump(ctx, 0x009754);
	}
}

/* $00975C bne $9754 / $009760 rts: the second half of the spin. No C block jumps here, but a savestate
 * made by the 68k (--hle none) at the end of a frame usually stops in this loop; with these the native
 * runner can resume such a state (mousetest's start states). */
HLE_FUNC(0x00975C, wait_vblank_spin_branch, 10)
{
	if (!(ctx->sr & CC_Z))
		hle_jump(ctx, 0x009754);
}
GLUE_RTS(0x009760)

/* $009742 wait_vblanks: d0+1 frames.  bsr wait_vblank / dbra d0,$9742 / rts */
GLUE_CALL(0x009742, 4, 0x00974C)
HLE_FUNC(0x009746, wait_vblanks_next, 10)
{
	set_w(&ctx->d[0], ctx->d[0] - 1);
	if ((ctx->d[0] & 0xFFFF) != 0xFFFF)
		hle_jump(ctx, 0x009742);
}

/* ------------------------------------------------------------------ countdown spins */
/* tst.w (RAM_FRAME_COUNTDOWN) / bpl <self>: the menus' "wait until the countdown has run out"
 * (move.w #4,(RAM_FRAME_COUNTDOWN) before it = 5 frames). Falls through at NEXT. */
static void frame_countdown_spin(hle_ctx *ctx, uint32_t self, uint32_t next)
{
	cc_logic16(ctx, RAM16(RAM_FRAME_COUNTDOWN));
	if (!(RAM16(RAM_FRAME_COUNTDOWN) & 0x8000)) {
		hle_idle(ctx);
		hle_jump(ctx, self);
	} else {
		hle_jump(ctx, next);
	}
}

HLE_WAIT_RANGE(0x008588, menu_open_wait, 20, 0x008588, 0x008590) { frame_countdown_spin(ctx, 0x008588, 0x008590); }
HLE_WAIT_RANGE(0x008D42, menu_choose_wait, 20, 0x008D42, 0x008D4A) { frame_countdown_spin(ctx, 0x008D42, 0x008D4A); }
HLE_WAIT_RANGE(0x008EDC, menu_select_wait, 20, 0x008EDC, 0x008EE6) { frame_countdown_spin(ctx, 0x008EDC, 0x008EE6); }
HLE_WAIT_RANGE(0x009008, mystery_menu_wait, 20, 0x009008, 0x009010) { frame_countdown_spin(ctx, 0x009008, 0x009010); }
HLE_WAIT_RANGE(0x009444, sound_test_wait, 20, 0x009444, 0x00944E) { frame_countdown_spin(ctx, 0x009444, 0x00944E); }
HLE_WAIT_RANGE(0x00963E, sound_test_wait2, 20, 0x00963E, 0x009648) { frame_countdown_spin(ctx, 0x00963E, 0x009648); }

/* $005C5A inventory_arrow_up_wait: tst.b (RAM_ARROW_UP_TIMER) / bpl <self>, then (C, $5C64)
 * restart the countdown and set the meter. $005C74: the same for the down arrow. */
static void byte_countdown_spin(hle_ctx *ctx, uint32_t var, uint32_t self, uint32_t next)
{
	cc_logic8(ctx, RAM8(var));
	if (!(RAM8(var) & 0x80)) {
		hle_idle(ctx);
		hle_jump(ctx, self);
	} else {
		hle_jump(ctx, next);
	}
}

HLE_WAIT_RANGE(0x005C5A, inventory_arrow_up_spin, 20, 0x005C5A, 0x005C64)
{
	byte_countdown_spin(ctx, RAM_ARROW_UP_TIMER, 0x005C5A, 0x005C64);
}
HLE_WAIT_RANGE(0x005C74, inventory_arrow_down_spin, 20, 0x005C74, 0x005C7E)
{
	byte_countdown_spin(ctx, RAM_ARROW_DN_TIMER, 0x005C74, 0x005C7E);
}

/* tst.b (RAM_BAR_SLIDE) / bpl <self>: wait until the verb-bar slide has finished
 * ($001C40 talk dialog, $004836 script_subop_1D_bar_hide) */
HLE_WAIT_RANGE(0x001C40, talk_bar_slide_wait, 20, 0x001C40, 0x001C4A)
{
	byte_countdown_spin(ctx, RAM_BAR_SLIDE, 0x001C40, 0x001C4A);
}
HLE_WAIT_RANGE(0x004836, bar_hide_slide_wait, 20, 0x004836, 0x004840)
{
	byte_countdown_spin(ctx, RAM_BAR_SLIDE, 0x004836, 0x004840);
}

/* ------------------------------------------------------------------ flag spins */
/* btst #BIT,(VAR) / bne <self>: wait until the VBlank side clears the flag */
static void flag_clear_spin(hle_ctx *ctx, uint32_t var, int bit, uint32_t self, uint32_t next)
{
	cc_btst(ctx, RAM8(var), bit);
	if (RAM8(var) & (1u << bit)) {
		hle_idle(ctx);
		hle_jump(ctx, self);
	} else {
		hle_jump(ctx, next);
	}
}

/* $001E4E (talk dialog): an inline wait_vblank (bset #0 at $1DAA, then this spin) */
HLE_WAIT_RANGE(0x001E4E, talk_wait_vblank, 24, 0x001E4E, 0x001E5A)
{
	flag_clear_spin(ctx, VBLANK_FLAGS, 0, 0x001E4E, 0x001E5A);
}

/* $005CF6 inventory_refresh: wait until the inventory animation has stopped (RAM_INV_BUSY bit 0) */
HLE_WAIT_RANGE(0x005CF6, inventory_refresh_wait_anim, 24, 0x005CF6, 0x005D02)
{
	flag_clear_spin(ctx, RAM_INV_BUSY, 0, 0x005CF6, 0x005D02);
}

/* $005D0E: wait until VBlank's hud_dma has uploaded the inventory (VBF_HUD_DMA, bit 5); rts */
HLE_WAIT_FUNC(0x005D0E, inventory_refresh_wait_dma, 24)
{
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 5);
	if (RAM8(VBLANK_FLAGS) & VBF_HUD_DMA) {
		hle_idle(ctx);
		hle_jump(ctx, 0x005D0E);
	}
}

/* $0045C8 (fullscreen picture): tst.b (RAM_INV_BUSY) / bne <self> */
HLE_WAIT_RANGE(0x0045C8, picture_wait_inventory, 20, 0x0045C8, 0x0045D2)
{
	cc_logic8(ctx, RAM8(RAM_INV_BUSY));
	if (RAM8(RAM_INV_BUSY)) {
		hle_idle(ctx);
		hle_jump(ctx, 0x0045C8);
	} else {
		hle_jump(ctx, 0x0045D2);
	}
}

/* $002E6E script_op_0B_set_state: wait until object d0's animation flag (+$18 bit 0, a0 = the
 * object table) is cleared by the VBlank-side object code, then
 *     lea 6(a5),a5 / rts                                                     ($2E78 / $2E7C) */
HLE_WAIT_FUNC(0x002E6E, set_state_wait_object, 24)
{
	uint32_t a = ctx->a[0] + (int16_t)ctx->d[0] + 0x18;
	cc_btst(ctx, bus_r8(a), 0);
	if (bus_r8(a) & 1) {
		hle_idle(ctx);
		hle_jump(ctx, 0x002E6E);
		return;
	}
	ctx->a[5] += 6;
}

/* ------------------------------------------------------------------ pad spins */
/* Pad 1 is read by VBlank (pad_read, active low). btst #6 / beq <self> / btst #4 / beq <self>:
 * wait until both A ($40) and B ($10) are released. */
static bool a_and_b_released(hle_ctx *ctx)
{
	cc_btst(ctx, RAM8(PAD1_CUR), 6);
	if (!(RAM8(PAD1_CUR) & 0x40))
		return false;
	cc_btst(ctx, RAM8(PAD1_CUR), 4);
	return RAM8(PAD1_CUR) & 0x10;
}

/* $001D0A (talk dialog, before the choice) */
HLE_WAIT_RANGE(0x001D0A, talk_wait_release, 40, 0x001D0A, 0x001D22)
{
	if (a_and_b_released(ctx)) {
		hle_jump(ctx, 0x001D22);
		return;
	}
	hle_idle(ctx);
	hle_jump(ctx, 0x001D0A);
}

/* $00A192 text_wait: wait for A and B released, then animate a frame (bsr actor_animate,
 * bsr run_object_events from $A1AA, glue.c) */
HLE_WAIT_RANGE(0x00A192, text_wait_release, 40, 0x00A192, 0x00A1AA)
{
	if (a_and_b_released(ctx)) {
		hle_jump(ctx, 0x00A1AA);
		return;
	}
	hle_idle(ctx);
	hle_jump(ctx, 0x00A192);
}

/* $00A20C: A was down when the text ran out: wait for its release, then bra $A234 (rts) */
HLE_WAIT_RANGE(0x00A20C, text_wait_release_a, 20, 0x00A20C, 0x00A234)
{
	cc_btst(ctx, RAM8(PAD1_CUR), 6);
	if (RAM8(PAD1_CUR) & 0x40) {
		hle_jump(ctx, 0x00A234);
		return;
	}
	hle_idle(ctx);
	hle_jump(ctx, 0x00A20C);
}

/* $00A228: the same for B; falls through into the rts at $A234 */
HLE_WAIT_RANGE(0x00A228, text_wait_release_b, 20, 0x00A228, 0x00A234)
{
	cc_btst(ctx, RAM8(PAD1_CUR), 4);
	if (RAM8(PAD1_CUR) & 0x10) {
		hle_jump(ctx, 0x00A234);
		return;
	}
	hle_idle(ctx);
	hle_jump(ctx, 0x00A228);
}

/* ------------------------------------------------------------------ long routes */
/* $003026 script_subop_35_shaggy_pose_45 and $0044BA script_subop_07_closeup_picture: wait
 * until the inventory animation has stopped (RAM_INV_BUSY) */
HLE_WAIT_RANGE(0x003026, pose45_wait_inventory, 24, 0x003026, 0x003030)
{
	flag_clear_spin(ctx, RAM_INV_BUSY, 0, 0x003026, 0x003030);
}
HLE_WAIT_RANGE(0x0044BA, closeup_wait_inventory, 20, 0x0044BA, 0x0044C2)
{
	cc_logic8(ctx, RAM8(RAM_INV_BUSY));
	if (RAM8(RAM_INV_BUSY)) {
		hle_idle(ctx);
		hle_jump(ctx, 0x0044BA);
	} else {
		hle_jump(ctx, 0x0044C2);
	}
}

/* $003FD2 script_subop_27_minigame_throw: the frame pacing countdown */
HLE_WAIT_RANGE(0x003FD2, throw_wait, 20, 0x003FD2, 0x003FDC) { frame_countdown_spin(ctx, 0x003FD2, 0x003FDC); }

/* ------------------------------------------------------------------ the crash */
/* $000000: a jump through a null code pointer. Loading some saved games of the Ha Ha Carnival
 * (port/tools/slottest.sh, w1_full slots 4-8) runs an if/else body through ram_script_cb before
 * it is set, and the original does that too (--hle none). The 68000 then executes the vector table:
 * $00FF is an illegal instruction, its vector is not code, and the CPU ends up in the bus error
 * handler at $892, which cycles colour 0 forever: the game hangs on a flashing screen. The C does
 * the same, without the exception frames on the way. */
HLE_FUNC(0x000000, null_jump_crash, 34) { hle_jump(ctx, 0x000892); }

/* $000892 BusError: move.l #$C0000000,(vdp_ctrl) / move.w d0,(vdp_data) / addi.w #1,d0 / bra $892 */
HLE_FUNC(0x000892, bus_error_hang, 44)
{
	vdp_ctrl_w32(0xC0000000);
	vdp_data_w((uint16_t)ctx->d[0]);
	set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 1, 16));
	hle_jump(ctx, 0x000892);
}
