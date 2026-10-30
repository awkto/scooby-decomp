/* The long script sub-ops that end or interrupt a cutscene (GitLab #19, third pass; byte code
 * docs/script.md, mechanics docs/subsystems/minigames.md, status docs/subsystems/script.md):
 *
 *   sub $1E  $0041B6  ending_credits      credits scroll, then soft reset
 *   sub $05/$0C $0045A4 fullscreen_picture  save the screen, show a picture with a palette shimmer
 *   sub $27  $003F58  minigame_throw      the Crane Game's claw (Game Tent)
 *   sub $32  $003142  minigame_tug        the Bumper Car duel (Bumper Cars)
 *
 * All four wait for VBlank in loops, so they are chains of HLE_RANGEs that stop in front of
 * every blocking call (`bsr wait_vblank`, `bsr actor_animate`, `bsr room_fade_in` ...) and
 * start again behind it (docs/porting.md, "Waiting for VBlank"). Whatever sits between two
 * waits is written as a plain C function; the thin range wrappers carry the register and
 * flag contract of the 68k code. Callees that do not wait (sound_play, vdp_copy_to_vram,
 * lzss_decompress, the room drawing) go through hle_call68k.
 *
 * Registers that are dead at a range exit (the duel's setup and per-frame body: the loop
 * reloads everything from RAM) are not reproduced; difftest runs those ranges with
 * --ignore-regs d0-d7,a0-a4 --ignore-flags (tools/py/script_dt_minigames.sh). A5/A6 (the
 * script pointers the dispatcher goes on with) and SP are always compared.
 */
#include "game.h"
#include "cc68k.h"

#define D(n) ctx->d[n]
#define A(n) ctx->a[n]
#define W(a) RAM16(a)
#define B(a) RAM8(a)

/* ---- addresses of the 68k routines called */
#define WAIT_VBLANK       0x00974C
#define WAIT_VBLANKS      0x009742
#define ACTOR_ANIMATE     0x00B0B8
#define SOUND_PLAY        0x009762
#define SOUND_STOP        0x009776
#define VDP_COPY_TO_VRAM  0x00992C
#define VDP_DMA_VRAM      0x0099F6
#define VDP_DMA_CRAM      0x009B10
#define VDP_CLEAR_CRAM    0x009C5E
#define VDP_WRITE_WORD    0x009964
#define LZSS_DECOMPRESS   0x0097AA
#define PLANE_A_CLEAR     0x009716
#define PLANE_B_CLEAR     0x009722
#define ROOM_DRAW_VIEW    0x007950
#define STATUS_LINE_UPDATE 0x00A828
#define LINE_PROBE        0x007476   /* zone_probe_point on (line_probe_x, line_probe_y); Z = free */
#define SOUND_CMD_PAUSE   0x1F6F2A
#define SOUND_CMD_UNPAUSE 0x1F6F3E

/* ---- RAM */
#define VBLANK_FLAGS_R    0xFF09DC
#define VERB_STATE        0xFF09DE   /* bit 1 status line dirty, bit 6 close-up/mini-game screen */
#define SCRIPT_FLAGS_09E8 0xFF09E8   /* bit 2: camera locked to the screen (mini-games) */
#define SCRIPT_FLAGS_09E9 0xFF09E9   /* bit 2: claw game active */
#define SCRIPT_FLAGS_09EB 0xFF09EB
#define DUEL_STATE        0xFF09EA   /* duel flags, see below */
#define DUEL_HIT          0xFF09EC   /* bit 0 Shaggy's car / bit 1 the Clown's car hit a wall this frame */
#define PAD1              0xFF09E0
#define PAD_LAST          0xFF09E2
#define GAME_STATE        0xFF2A00   /* bit 1: fullscreen picture shown once */
#define GAME_STATE_SAVE   0xFF2C00   /* VRAM backup of the fullscreen picture (game_state + 512) */
#define TEXT_BUF          0xFF08BC
#define INV_NAMETABLE     0xFF0328
#define PAL_TARGET        0xFF0778
#define PAL_CYCLE         0xFF090C
#define CAMERA_X          0xFF06CA
#define CAMERA_Y          0xFF06CE
#define VSCROLL           0xFF07FC
#define UI_TILE_BASE_A    0xFF068A
#define UI_TILE_BASE_F    0xFF08B8
#define VERB_BAR_WINDOW   0xFF0AD1
#define TILES_READY       0xFF0AB6
#define ACTORS_ANIM_NEW   0xFF0AB5
#define ACTORS_DEPTH_ON   0xFF0AB8
#define ACTORS_ANIM_DONE  0xFF0ABC
#define ACTORS_ANIM_LOOP  0xFF0ABD
#define ACTORS_FROZEN     0xFF0ABF
#define ACTORS_ON_PATH    0xFF0AC0
#define FRAME_COUNTDOWN_R 0xFF08AE
#define ACTOR_ANIMSET     0xFF0470   /* l[6] */
#define ACTOR_ANIM        0xFF0488   /* w[6] */
#define ACTOR_TIMER       0xFF0618   /* w[6] */
#define ACTOR_X           0xFF04DC   /* 16.16 [6]: the word at +0 is the pixel position */
#define ACTOR_Y           0xFF04F4
#define ACTOR_PATH_POS    0xFF060C   /* w[6] */
#define SHAGGY_FACING     0xFF05B8
#define CLOWN_FACING      0xFF05BC   /* duel: push direction of the Clown's car (actor 2) */
#define LINE_PROBE_X      0xFF064A
#define LINE_PROBE_Y      0xFF064E
#define OBJ0_RESULT       0xFF120C   /* object 0 state word: the mini-game's result for the script */
#define RNG_STATE         0xFF0428

static inline bool bit_test(hle_ctx *c, uint32_t a, int bit)
{
	cc_btst(c, RAM8(a), bit);
	return (RAM8(a) >> bit) & 1;
}
static inline void bit_set(hle_ctx *c, uint32_t a, int bit) { bit_test(c, a, bit); RAM8(a) |= (uint8_t)(1u << bit); }
static inline void bit_clr(hle_ctx *c, uint32_t a, int bit) { bit_test(c, a, bit); RAM8(a) &= (uint8_t)~(1u << bit); }

/* total 68k cost of the original minus what its nested calls already accounted for (docs/porting.md) */
static int charge_total(int total)
{
	int c = total - hle_nested_cycles();
	return c > 0 ? c : 0;
}

/* move.l #id,d0 / bsr sound_play (or sound_stop) */
static void sound_call(hle_ctx *ctx, uint32_t fn, uint32_t id)
{
	D(0) = id;
	cc_logic32(ctx, id);
	hle_call68k(ctx, fn);
}

/* ================================================================ ending credits
 * sub $1E: wait half a second, unpack two LZSS pictures (the credits' tiles to the UI tile
 * base, then the tile map: every word offset by the tile base), then scroll the tall picture
 * up one pixel per two frames (the vertical scroll register $FF07FC counts pixels); every 8
 * pixels the next map row goes into the 32-row plane A ring (row (d2 & 31) at $C000 + 128 per
 * row). When the map is used up, hold for 300 frames, fade out and reset the game.
 *   D2 ring row, D3 scroll (pixels), D5 toggles every frame (the scroll steps every 2nd),
 *   A0 map read pointer, A3 map end. */
#define CRED_WAIT 0x0041BA   /* bsr wait_vblanks */
#define CRED_ANIM 0x004222   /* bsr actor_animate (frame loop; the bsr wait_vblank follows) */
#define CRED_HOLD 0x004258   /* bsr actor_animate (300-frame hold) */
#define CRED_FADE 0x00426C   /* bsr palette_read_cram (fade out), then bra soft_reset */

HLE_RANGE(0x0041B6, script_sys_credits_wait, 8, CRED_WAIT)
{
	set_w(&D(0), 0x1E);
	cc_logic16(ctx, 0x1E);
	hle_jump(ctx, CRED_WAIT);
}

HLE_RANGE(0x0041BE, script_sys_credits_setup, 0, CRED_ANIM)
{
	bit_set(ctx, 0xFF09EB, 1);
	A(0) = 0x0296FA;                                   /* credits tiles */
	A(1) = 0xFF3000;
	A(2) = 0xFF7000;
	hle_call68k(ctx, LZSS_DECOMPRESS);
	A(0) = A(2);
	set_w(&D(0), W(UI_TILE_BASE_A));
	cc_logic16(ctx, D(0));
	set_w(&D(0), lslw(ctx, D(0), 5));                  /* tile base -> VRAM address */
	D(1) = RAM32(A(1));                                /* unpacked size in bytes */
	cc_logic32(ctx, D(1));
	set_w(&D(1), lsrw(ctx, D(1), 1));
	hle_call68k(ctx, VDP_COPY_TO_VRAM);
	A(0) = 0x029C72;                                   /* credits tile map */
	hle_call68k(ctx, LZSS_DECOMPRESS);
	D(0) = RAM32(A(1));
	cc_logic32(ctx, D(0));
	A(3) = A(2) + D(0);                                /* end of the map */
	A(0) = A(2);
	set_w(&D(0), lsrw(ctx, D(0), 1));
	set_w(&D(0), cc_sub(ctx, D(0), 1, 16, true));
	set_w(&D(1), W(UI_TILE_BASE_A));
	cc_logic16(ctx, D(1));
	do {                                               /* map words += tile base */
		RAM16(A(2)) = (uint16_t)cc_add(ctx, RAM16(A(2)), D(1), 16);
		A(2) += 2;
		set_w(&D(0), D(0) - 1);
	} while ((uint16_t)D(0) != 0xFFFF);
	set_w(&D(2), 0x1C);
	cc_logic16(ctx, 0x1C);
	D(3) = 0;
	cc_logic32(ctx, 0);
	set_w(&D(1), 0x20);
	cc_logic16(ctx, 0x20);
	D(5) = 0;
	cc_logic32(ctx, 0);
	W(VSCROLL) = (uint16_t)D(3);                       /* $00421C */
	cc_logic16(ctx, D(3));
	hle_jump(ctx, CRED_ANIM);
	ctx->cycles = charge_total(3173421);
}

/* after the frame's wait_vblank: every second frame (D5) scroll one pixel; every 8th pixel
 * copy the next map row (32 words) to the plane */
HLE_RANGE(0x00422A, script_sys_credits_step, 182, CRED_ANIM, CRED_HOLD)
{
	set_w(&D(5), D(5) ^ 1);
	cc_logic16(ctx, D(5));
	bool again = (uint16_t)D(5) != 0;
	if (!again) {
		set_w(&D(3), cc_add(ctx, D(3), 1, 16));
		set_w(&D(4), D(3));
		cc_logic16(ctx, D(4));
		set_w(&D(4), D(4) & 7);
		cc_logic16(ctx, D(4));
		again = (uint16_t)D(4) != 0;
		if (!again) {
			set_w(&D(2), cc_add(ctx, D(2), 1, 16));
			set_w(&D(2), D(2) & 0x1F);
			cc_logic16(ctx, D(2));
			set_w(&D(0), D(2));
			cc_logic16(ctx, D(0));
			set_w(&D(0), lslw(ctx, D(0), 7));
			set_w(&D(0), cc_add(ctx, D(0), 0xC000, 16));
			hle_call68k(ctx, VDP_COPY_TO_VRAM);
			cc_sub(ctx, A(3), A(0), 32, false);        /* cmpa.l a0,a3 */
			again = (int32_t)A(3) > (int32_t)A(0);
			if (!again) {
				W(FRAME_COUNTDOWN_R) = 0x12C;          /* hold the last screen 5 s */
				cc_logic16(ctx, 0x12C);
				hle_jump(ctx, CRED_HOLD);
				return;
			}
		}
	}
	W(VSCROLL) = (uint16_t)D(3);
	cc_logic16(ctx, D(3));
	hle_jump(ctx, CRED_ANIM);
}

HLE_RANGE(0x00425C, script_sys_credits_hold, 26, CRED_HOLD, CRED_FADE)
{
	cc_logic16(ctx, W(FRAME_COUNTDOWN_R));
	if (!(W(FRAME_COUNTDOWN_R) & 0x8000)) {
		hle_jump(ctx, CRED_HOLD);
		return;
	}
	bit_clr(ctx, VBLANK_FLAGS_R, 6);
	hle_jump(ctx, CRED_FADE);
}

/* ================================================================ fullscreen picture
 * sub $05 / $0C ($0045A4; $0C only runs the bar_hide at the end, "bar_hide_once"): show a
 * full-screen picture once (game_state bit 1 = already shown): wait for the actor tile
 * DMA, fade to black, save VRAM $0000-$43FF (8704 words) at $FF2C00, unpack the picture
 * tiles ($02B33A, 7712 words at VRAM 0; map $02EF7A to plane A $C080 (1216 words), $02F97A
 * rows to plane B) and fade in the picture's palette ($02F8FA, 32 longs). Then the viewer: up
 * to 201 frames, ended by A or B; every 4th frame toggles the camera X low bit (a 1 px
 * shimmer), and every frame the 8 palette words at $FF090C are rotated by one (a colour
 * cycle) and sent to CRAM. Afterwards the VRAM is restored from the backup, the room view
 * redrawn and the room palette faded in again.
 * Stack: movem.l d0-a6 (60) while the picture is up, camera Y/X (4), the frame counter (2). */
#define FP_SPIN   0x0045C8   /* tst.b ($FF0AB6) / bne: wait for the tile DMA */
#define FP_RTS    0x0045B0
#define FP_BAR    0x0047A6   /* bsr bar_hide */
#define FP_WAIT1  0x0045E0   /* bsr wait_vblank (fade to black) */
#define FP_LOOPW  0x0046F4   /* bsr wait_vblank (viewer frame) */
#define FP_ANIM   0x00476C   /* bsr actor_animate (after the restore) */
#define FP_WAITS  0x00477E   /* bsr wait_vblanks */
#define FP_WAIT2  0x004796   /* bsr wait_vblank (room palette) */
#define FP_PUSHED 60

HLE_RANGE(0x0045A4, script_sys_fullscreen_picture, 106, FP_RTS, FP_BAR, HLE_EXIT_SP(FP_SPIN, -FP_PUSHED))
{
	if (bit_test(ctx, GAME_STATE, 1)) {
		hle_jump(ctx, FP_RTS);                         /* shown already */
		return;
	}
	bit_set(ctx, GAME_STATE, 1);
	cc_sub(ctx, bus_r16(A(5) + 2), 0x0C, 16, false);   /* sub-op $0C: only the bar hide */
	if (bus_r16(A(5) + 2) == 0x0C) {
		hle_jump(ctx, FP_BAR);
		return;
	}
	for (int i = 6; i >= 0; i--)
		push32(ctx, A(i));                             /* movem.l d0-d7/a0-a6,-(sp) */
	for (int i = 7; i >= 0; i--)
		push32(ctx, D(i));
	hle_jump(ctx, FP_SPIN);
}

/* tile DMA done: clear the palette (the fade starts from black), back the VRAM up */
HLE_RANGE(0x0045D2, script_sys_fullscreen_picture_begin, 36, FP_WAIT1)
{
	bit_clr(ctx, VBLANK_FLAGS_R, 6);
	D(0) = 0;
	cc_logic32(ctx, 0);
	set_w(&D(1), 0x80);
	cc_logic16(ctx, 0x80);
	hle_jump(ctx, FP_WAIT1);
}


/* Viewer loop head ($0046BE-$0046F2): A or B ends the viewer; every 4th frame the camera X
 * high byte bit 0 is flipped (the picture has two animation frames side by side, 256 px
 * apart); then the frame counter D0 is pushed, the palette buffer is queued for CRAM and
 * the frame's wait_vblank runs. */
static void fp_viewer_head(hle_ctx *ctx)
{
	if (!bit_test(ctx, PAD1, 6) || !bit_test(ctx, PAD1, 4)) {   /* A or B pressed (active low) */
		hle_jump(ctx, 0x004716);
		return;
	}
	set_w(&D(2), cc_sub(ctx, D(2), 1, 16, true));      /* subq.w #1,d2 */
	if (ctx->sr & CC_N) {                              /* every 4th frame */
		D(2) = 3;
		cc_logic32(ctx, 3);
		bit_test(ctx, CAMERA_X, 0);                    /* bchg.b #0,(ram_camera_x) */
		B(CAMERA_X) ^= 1;
	}
	push16(ctx, (uint16_t)D(0));
	A(0) = TEXT_BUF;
	D(0) = 0;
	cc_logic32(ctx, 0);
	set_w(&D(1), 0x40);
	cc_logic16(ctx, 0x40);
	hle_jump(ctx, FP_LOOPW);
}

/* the backup, the picture, its palette; the first viewer frame */
HLE_RANGE(0x0045E4, script_sys_fullscreen_picture_draw, 0,
          HLE_EXIT_SP(FP_LOOPW, -6), HLE_EXIT_SP(0x004716, -4))
{
	hle_call68k(ctx, VDP_CLEAR_CRAM);
	A(0) = GAME_STATE_SAVE;                            /* save VRAM $0000-$43FF */
	set_w(&D(1), 0x21FF);
	ctx->sr = 0x2700;                                  /* the data port read must not be interrupted */
	vdp_ctrl_w32(0);
	do {
		RAM16(A(0)) = vdp_data_r();
		A(0) += 2;
		set_w(&D(1), D(1) - 1);
	} while ((uint16_t)D(1) != 0xFFFF);
	ctx->sr = 0x2400;
	vdp_ctrl_w(0x9200);                                /* reg $12: window off */
	cc_logic16(ctx, 0x9200);
	B(VERB_BAR_WINDOW) = 0;
	cc_logic8(ctx, 0);
	A(0) = 0x02B33A;                                   /* picture tiles */
	D(0) = 0;
	cc_logic32(ctx, 0);
	set_w(&D(1), 0x1E1F);
	cc_logic16(ctx, 0x1E1F);
	hle_call68k(ctx, VDP_COPY_TO_VRAM);
	push16(ctx, W(CAMERA_X));
	push16(ctx, W(CAMERA_Y));
	W(CAMERA_X) = 0;
	W(CAMERA_Y) = 0;
	hle_call68k(ctx, PLANE_A_CLEAR);
	hle_call68k(ctx, PLANE_B_CLEAR);
	A(0) = 0x02EF7A;                                   /* plane A map */
	set_w(&D(0), 0xC080);
	set_w(&D(1), 0x04C0);
	hle_call68k(ctx, VDP_COPY_TO_VRAM);
	A(0) = 0x02F97A;                                   /* plane B: 19 times the same 32 words, 64 bytes apart */
	set_w(&D(0), 0xE080);
	set_w(&D(1), 0x20);
	set_w(&D(2), 0x12);
	do {
		hle_call68k(ctx, VDP_COPY_TO_VRAM);
		set_w(&D(0), cc_add(ctx, D(0), 0x40, 16));
		A(0) -= 0x40;
		hle_call68k(ctx, VDP_COPY_TO_VRAM);
		set_w(&D(0), cc_add(ctx, D(0), 0x40, 16));
		set_w(&D(2), D(2) - 1);
	} while ((uint16_t)D(2) != 0xFFFF);
	A(0) = 0x02F8FA;                                   /* the picture's palette -> text buffer */
	A(1) = TEXT_BUF;
	set_w(&D(0), 0x1F);
	do {
		RAM32_W(A(1), bus_r32(A(0)));
		A(0) += 4;
		A(1) += 4;
		set_w(&D(0), D(0) - 1);
	} while ((uint16_t)D(0) != 0xFFFF);
	vdp_ctrl_w32(0x58000003);                          /* VSRAM/CRAM address write, then a zero long */
	bus_w32(VDP_DATA, 0);
	set_w(&D(0), 0xC8);                                /* 201 viewer frames at most */
	cc_logic16(ctx, 0xC8);
	D(2) = 3;
	cc_logic32(ctx, 3);
	fp_viewer_head(ctx);
	ctx->cycles = charge_total(696714);
}

/* after the frame's wait_vblank: palette to CRAM, rotate the 8 colours at $FF090C */
HLE_RANGE(0x0046F8, script_sys_fullscreen_picture_frame, 1366,
          HLE_EXIT_SP(FP_LOOPW, 0), HLE_EXIT_SP(0x004716, 2))
{
	hle_call68k(ctx, VDP_DMA_CRAM);
	A(0) = PAL_CYCLE;
	push16(ctx, RAM16(A(0)));
	D(1) = 6;
	cc_logic32(ctx, 6);
	do {
		cc_logic16(ctx, RAM16(A(0) + 2));
		RAM16(A(0)) = RAM16(A(0) + 2);
		A(0) += 2;
		set_w(&D(1), D(1) - 1);
	} while ((uint16_t)D(1) != 0xFFFF);
	{
		uint16_t v = pop16(ctx);
		cc_logic16(ctx, v);
		RAM16(A(0)) = v;
	}
	{
		uint16_t v = pop16(ctx);
		cc_logic16(ctx, v);
		set_w(&D(0), v);
	}
	set_w(&D(0), D(0) - 1);                            /* dbra d0 */
	if ((uint16_t)D(0) != 0xFFFF)
		fp_viewer_head(ctx);
	else
		hle_jump(ctx, 0x004716);
}

/* viewer over: black palette, restore VRAM and the camera, redraw the room, pop the registers */
HLE_RANGE(0x004716, script_sys_fullscreen_picture_restore, 0, HLE_EXIT_SP(FP_ANIM, 64))
{
	D(0) = 0;
	cc_logic32(ctx, 0);
	set_w(&D(1), 0x40);
	cc_logic16(ctx, 0x40);
	hle_call68k(ctx, VDP_CLEAR_CRAM);
	ctx->sr = 0x2700;
	{
		uint16_t v = pop16(ctx);
		W(CAMERA_Y) = v;
		cc_logic16(ctx, v);
		v = pop16(ctx);
		W(CAMERA_X) = v;
		cc_logic16(ctx, v);
	}
	vdp_ctrl_w(0x9202);                                /* window back on */
	cc_logic16(ctx, 0x9202);
	B(VERB_BAR_WINDOW) = 2;
	cc_logic8(ctx, 2);
	A(0) = GAME_STATE_SAVE;
	D(0) = 0;
	cc_logic32(ctx, 0);
	set_w(&D(1), 0x2200);
	cc_logic16(ctx, 0x2200);
	hle_call68k(ctx, VDP_COPY_TO_VRAM);
	hle_call68k(ctx, ROOM_DRAW_VIEW);
	ctx->sr = 0x2000;
	for (int i = 0; i < 8; i++)
		D(i) = pop32(ctx);                             /* movem.l (sp)+,d0-d7/a0-a6 */
	for (int i = 0; i < 7; i++)
		A(i) = pop32(ctx);
	bit_set(ctx, VBLANK_FLAGS_R, 6);
	bit_set(ctx, VBLANK_FLAGS_R, 6);
	hle_jump(ctx, FP_ANIM);
	ctx->cycles = charge_total(645461);
}

HLE_RANGE(0x004770, script_sys_fullscreen_picture_status, 0, FP_WAITS)
{
	bit_set(ctx, VERB_STATE, 1);
	hle_call68k(ctx, STATUS_LINE_UPDATE);
	D(0) = 5;
	cc_logic32(ctx, 5);
	hle_jump(ctx, FP_WAITS);
	ctx->cycles = charge_total(284852);
}

HLE_RANGE(0x004782, script_sys_fullscreen_picture_palette, 48, FP_WAIT2)
{
	bit_clr(ctx, VBLANK_FLAGS_R, 6);
	A(0) = PAL_TARGET;
	set_w(&D(1), 0x40);
	D(0) = 0;
	cc_logic32(ctx, 0);
	hle_jump(ctx, FP_WAIT2);
}

HLE_RANGE(0x00479A, script_sys_fullscreen_picture_done, 0, 0x0047A6)
{
	hle_call68k(ctx, VDP_DMA_CRAM);
	bit_set(ctx, VBLANK_FLAGS_R, 6);
	hle_jump(ctx, 0x0047A6);                           /* bsr bar_hide / rts */
	ctx->cycles = charge_total(1030);
}

/* ================================================================ Crane Game claw
 * sub $27 ($003F58, "minigame_throw"; the Crane Game in the Game Tent, room "crane"): the
 * claw is actor 2 (X $FF04E8, Y $FF0500); the pistol on the machine's belt is a path actor
 * whose path position $FF0610 advances by itself (the scripted path of actor 2... slot 2's
 * path pointer; position wraps at $140). Left/Right move the claw (-2/+2 px per frame,
 * 100..148); A or B drops it (2 px per frame down to Y 74) and it catches the pistol iff the
 * path position at that moment is within $5C..$78 (claw X = 148) or $F8..$114 (any other X):
 * claw animation 28 (grab) and result 1 in object 0's state word, else animation 27 and 0.
 * The script reads the result ($FF120C) afterwards.
 *
 * Pad notes: the pad byte is forced to $FF at the start (nothing pressed), and the 68k
 * waits for VBlank by spinning on $FF08AE (set to 0 and decremented by VBlank): one claw
 * step per frame. The loop at $003FE2 below the drop test (move while a direction is held)
 * is dead code: the branch to it from the drop test always has Z set. */
#define CR_FRAME   0x003F82   /* bsr actor_animate */
#define CR_SPIN    0x003FD2   /* tst.w ($FF08AE) / bpl: the frame wait */
#define CR_DROP    0x004014   /* bsr wait_vblank (claw falling) */
#define CR_ANIM    0x004086   /* bsr actor_animate (catch / miss animation) */
#define CR_SETTLE  0x0040AE   /* bsr wait_vblank (claw rising) */
#define CR_FADE    0x0040CA   /* bsr palette_read_cram */
#define CR_RTS     0x0040DE
#define CLAW_X     0xFF04E8
#define CLAW_X2    0xFF04EC   /* copy of the claw X for the second sprite */
#define CLAW_Y     0xFF0500
#define BELT_POS   0xFF0610   /* path position of the pistol belt */

HLE_RANGE(0x003F58, script_sys_minigame_throw, 98, CR_FRAME)
{
	bit_set(ctx, SCRIPT_FLAGS_09E9, 2);
	D(1) = 0;
	cc_logic32(ctx, 0);
	B(PAD1) = 0xFF;                                    /* nothing pressed */
	cc_logic8(ctx, 0xFF);
	if (!bit_test(ctx, ACTORS_ON_PATH, 2)) {           /* belt not running: start it at 0 */
		W(BELT_POS) = 0;
		cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);  /* clr.w */
	}
	W(FRAME_COUNTDOWN_R) = 0;                          /* $003F7A */
	cc_logic16(ctx, 0);
	hle_jump(ctx, CR_FRAME);
}

/* one claw frame, after the actor frame: steer; if A/B is down start the drop */
HLE_RANGE(0x003F86, script_sys_minigame_throw_frame, 200, CR_SPIN, CR_DROP)
{
	if (!bit_test(ctx, PAD1, 2)) {                     /* Left */
		D(1) = (uint32_t)-2;
		cc_logic32(ctx, D(1));
	}
	if (!bit_test(ctx, PAD1, 3)) {                     /* Right */
		D(1) = 2;
		cc_logic32(ctx, 2);
	}
	set_w(&D(0), D(1));
	cc_logic16(ctx, D(0));
	set_w(&D(0), cc_add(ctx, D(0), W(CLAW_X), 16));
	cc_sub(ctx, D(0), 100, 16, false);
	if (!((int16_t)D(0) < 100)) {
		cc_sub(ctx, D(0), 148, 16, false);
		if (!((int16_t)D(0) > 148)) {
			W(CLAW_X) = (uint16_t)D(0);
			cc_logic16(ctx, D(0));
			W(CLAW_X2) = (uint16_t)D(0);
			cc_logic16(ctx, D(0));
		}
	}
	if (bit_test(ctx, PAD1, 6) && bit_test(ctx, PAD1, 4)) {   /* neither A nor B: next frame */
		hle_jump(ctx, CR_SPIN);
		return;
	}
	/* drop: $004008 */
	set_w(&D(0), W(CLAW_Y));
	cc_logic16(ctx, D(0));
	W(CLAW_Y) = (uint16_t)D(0);
	cc_logic16(ctx, D(0));
	hle_jump(ctx, CR_DROP);
}

/* the spin ended: next frame ($003FDC bra $003F7A) */
HLE_RANGE(0x003FDC, script_sys_minigame_throw_next, 30, CR_FRAME)
{
	W(FRAME_COUNTDOWN_R) = 0;
	cc_logic16(ctx, 0);
	hle_jump(ctx, CR_FRAME);
}

/* the claw falls 2 px per frame to Y 74, then the catch test */
HLE_RANGE(0x004018, script_sys_minigame_throw_fall, 60, CR_DROP, CR_ANIM)
{
	set_w(&D(0), cc_add(ctx, D(0), 2, 16));
	cc_sub(ctx, D(0), 74, 16, false);
	if ((int16_t)D(0) < 74) {
		W(CLAW_Y) = (uint16_t)D(0);
		cc_logic16(ctx, D(0));
		hle_jump(ctx, CR_DROP);
		return;
	}
	W(CLAW_Y) = 74;
	cc_logic16(ctx, 74);
	set_w(&D(0), W(BELT_POS));
	cc_logic16(ctx, D(0));
	bool catch;
	cc_sub(ctx, W(CLAW_X), 148, 16, false);
	if (W(CLAW_X) == 148) {                            /* far right: a narrow window */
		cc_sub(ctx, D(0), 92, 16, false);
		catch = !((int16_t)D(0) < 92);
		if (catch) {
			cc_sub(ctx, D(0), 120, 16, false);
			catch = !((int16_t)D(0) > 120);
		}
	} else {
		cc_sub(ctx, D(0), 248, 16, false);
		catch = !((int16_t)D(0) < 248);
		if (catch) {
			cc_sub(ctx, D(0), 276, 16, false);
			catch = !((int16_t)D(0) > 276);
		}
	}
	if (catch) {
		sound_call(ctx, SOUND_PLAY, 0x63);
		W(ACTOR_ANIM + 6) = 28;                        /* claw closes on the prize */
		cc_logic16(ctx, 28);
		W(OBJ0_RESULT) = 1;
		cc_logic16(ctx, 1);
	} else {
		W(ACTOR_ANIM + 6) = 27;                        /* claw closes on air */
		cc_logic16(ctx, 27);
		W(OBJ0_RESULT) = 0;
		cc_logic16(ctx, 0);
	}
	bit_set(ctx, ACTORS_ANIM_NEW, 3);
	hle_jump(ctx, CR_ANIM);
}

/* wait for the claw animation, sound, freeze the prize on a catch */
HLE_RANGE(0x00408A, script_sys_minigame_throw_anim, 31, CR_ANIM, CR_SETTLE)
{
	if (!bit_test(ctx, ACTORS_ANIM_DONE, 3)) {
		hle_jump(ctx, CR_ANIM);
		return;
	}
	sound_call(ctx, SOUND_PLAY, 0x62);
	cc_logic16(ctx, W(OBJ0_RESULT));
	if (W(OBJ0_RESULT))
		bit_set(ctx, ACTORS_FROZEN, 2);
	hle_jump(ctx, CR_SETTLE);
}

/* the claw rises 1 px per frame back to Y 0 ... */
HLE_RANGE(0x0040B2, script_sys_minigame_throw_rise, 30, CR_SETTLE, CR_FADE)
{
	W(CLAW_Y) = (uint16_t)cc_sub(ctx, W(CLAW_Y), 1, 16, true);
	if (W(CLAW_Y)) {
		hle_jump(ctx, CR_SETTLE);
		return;
	}
	bit_clr(ctx, SCRIPT_FLAGS_09E9, 2);
	bit_clr(ctx, VBLANK_FLAGS_R, 6);
	hle_jump(ctx, CR_FADE);
}

/* ... then the palette is read back (the fade out runs in the 68k) and the game goes on */
HLE_RANGE(0x0040CE, script_sys_minigame_throw_done, 48, CR_RTS)
{
	bit_set(ctx, VBLANK_FLAGS_R, 6);
	bit_clr(ctx, ACTORS_FROZEN, 2);
	hle_jump(ctx, CR_RTS);
}

/* ================================================================ Bumper Car duel
 * sub $32 ($003142, "minigame_tug"; Bumper Cars, room 7): a top-down duel between Shaggy's
 * car (actor 0, steered with the pad) and the Clown's (actor 2, computer-driven). Both are
 * ordinary actors; the duel only sets their animation numbers (= headings 0..7, clockwise
 * from north), moves their X/Y by the vector tables and tests collisions.
 *
 * State (all words unless noted):
 *   $FF0A90 / $FF0A92  speed 0..7 of Shaggy's / the Clown's car (A or B = gas, else it slows;
 *                      the Clown is always on the gas)
 *   $FF0A94 / $FF0A96  recoil: speed of the push the car got from a wall or the other car
 *   $FF0A98 / $FF0A9A  hit points, 63 at the start, shown as two 64-pixel meters;
 *                      below 0 = destroyed (A98 < 0: lost, result 0; A9A < 0: won, result 1)
 *   $FF0A9C            heading the Clown last aimed at; $FF0A9E its turn direction (+-1)
 *   $FF0AA0-$FF0AA4    byte timers (frames): Shaggy's gas ramp, the Clown's gas ramp,
 *                      Shaggy's recoil step, the Clown's recoil step, the Clown's steering
 *   $FF0A84-$FF0A8E    last frame's positions/headings (undo of a blocked or colliding move)
 *   $FF05B8 / $FF05BC  push direction (heading) of the recoil of Shaggy's / the Clown's car
 *   $FF09EA            bit 1/2: Shaggy's recoil turns him -1/+1 heading per step; bit 3/4: the
 *                      Clown's; bit 6/7: damage from the other car is still due (Shaggy / Clown)
 *   $FF09EC            bit 0 / bit 1: Shaggy's / the Clown's car hit a wall this frame
 * Movement per frame = vector[heading][speed] + vector[push heading][recoil], tables at
 * $003D96 (16 bytes of X words then 16 of Y words per heading, 8 speeds); a car that ends
 * inside a blocked cell (zone probe) goes back to last frame's position and starts a recoil;
 * two cars whose hit boxes ($0F88DC / $0FA77A) overlap bump: both stay put, both are pushed
 * back, and the damage is the other car's speed (halved when not facing it).
 * Pause: Start freezes the game and sound until the next Start. */
#define DUEL_SPEED0   0xFF0A90
#define DUEL_SPEED1   0xFF0A92
#define DUEL_RECOIL0  0xFF0A94
#define DUEL_RECOIL1  0xFF0A96
#define DUEL_HP0      0xFF0A98
#define DUEL_HP1      0xFF0A9A
#define DUEL_AIM      0xFF0A9C
#define DUEL_TURN     0xFF0A9E
#define DUEL_TMR_GAS0 0xFF0AA0
#define DUEL_TMR_GAS1 0xFF0AA1
#define DUEL_TMR_REC0 0xFF0AA2
#define DUEL_TMR_REC1 0xFF0AA3
#define DUEL_TMR_STEER 0xFF0AA4
#define DUEL_PREV     0xFF0A84   /* x0 y0 x2 y2 anim0 anim2 */
#define DUEL_HEADING_OPP 0x003D86  /* w[8]: opposite heading */
#define DUEL_VECTORS  0x003D96
#define ACTOR2_X      0xFF04E4   /* actor 2 = the Clown's car; actor words are 16.16, the pixel is the high word */
#define ACTOR2_Y      0xFF04FC
#define ANIM0         ACTOR_ANIM
#define ANIM2         (ACTOR_ANIM + 4)
#define TUG_SETUP_WAIT 0x00331A   /* bsr actor_animate (intro animation) */
#define TUG_FADE_IN    0x00334A   /* bsr room_fade_in (script_subop_02) */
#define TUG_WAIT50     0x00335C
#define TUG_WAIT100    0x00336E
#define TUG_FRAME_WAIT 0x003372   /* bsr wait_vblank: top of the duel loop */
#define TUG_PAUSE_W1   0x0033B2   /* pause: wait for Start to be released */
#define TUG_PAUSE_W2   0x0033C0   /*        wait for Start again */
#define TUG_END_ANIM   0x0038DC   /* bsr actor_animate (explosion) */
#define TUG_FADE_OUT   0x0038E8   /* bsr script_subop_03 fade_out */
#define TUG_RTS        0x003938

static inline int16_t s16(uint32_t v) { return (int16_t)(uint16_t)v; }
static inline uint16_t duel_vec(unsigned off) { return ROM16(DUEL_VECTORS + off); }

/* the duel's coin flip (inline in the 68k code at four places): rng = rol(~rng, 1) with the
 * halves swapped; the bit rotated out of the low word picks the recoil's turn direction */
static bool duel_coin(void)
{
	uint32_t s = ~RAM32(RNG_STATE);
	s = (s << 1) | (s >> 31);
	s = (s << 16) | (s >> 16);
	RAM32_W(RNG_STATE, s);
	return (s >> 15) & 1;
}

/* $003A7A: heading 0..7 (clockwise from north) from Shaggy's car to the Clown's, from the
 * signs of the offsets and the octant of their ratio (the 68k code works it out bit by bit) */
static unsigned duel_bearing(void)
{
	int16_t dx = (int16_t)(W(ACTOR2_X) - W(ACTOR_X));
	int16_t dy = (int16_t)(W(ACTOR2_Y) - W(ACTOR_Y));
	int16_t ax = dx, ay = dy;
	uint16_t q = 0;
	if (dx < 0) { ax = (int16_t)-dx; q = 2; }
	if (dy < 0) { ay = (int16_t)-dy; q++; }
	if (!(q & 2)) q ^= 1;
	q = (uint16_t)(q << 1);
	int16_t m = ax, n = ay;                            /* d6, d7 */
	int16_t a = ax, b = ay;                            /* d2, d3 */
	if (q & 2) { int16_t t = a; a = b; b = t; }
	if (!(a < b)) q++;
	uint16_t t5 = (uint16_t)((q + 1) & 2);
	q = (uint16_t)(q << 1);
	if (t5) n = (int16_t)(n * 2); else m = (int16_t)(m * 2);
	if (q & 4) { int16_t t = m; m = n; n = t; }
	if (!(m < n)) q++;
	q++;
	return (q & 0xF) >> 1;
}

/* $003A68: the heading opposite to the bearing (the Clown turns towards Shaggy) */
static unsigned duel_aim(void) { return ROM16(DUEL_HEADING_OPP + 2 * duel_bearing()); }

/* $00393A: draw the two 64-pixel hit point meters as tiles (Shaggy's in text_buf: colour A,
 * the Clown's in inv_nametable: colour 9); one pixel column per point */
static void duel_meter(uint32_t buf, int16_t hp, uint32_t colour)
{
	for (int tile = 0; tile < 8; tile++) {
		uint32_t row = 0, bit = colour << 28;
		for (int px = 0; px < 8; px++) {
			if (hp >= 0) {
				row |= bit;
				bit >>= 4;
				hp--;
			}
		}
		for (int i = 0; i < 8; i++)
			RAM32_W(buf + 32 * tile + 4 * i, (i >= 2 && i < 6) ? row : 0);
	}
}
static void duel_meters(void)
{
	duel_meter(TEXT_BUF, s16(W(DUEL_HP0)), 0xA);
	duel_meter(INV_NAMETABLE, s16(W(DUEL_HP1)), 0x9);
}

/* $003C70: Shaggy's car hit a wall: sound $60, the pending car damage is taken, the push is
 * the old speed backwards, a random turn direction */
static void duel_wall0(hle_ctx *ctx)
{
	sound_call(ctx, SOUND_PLAY, 0x60);
	if (B(DUEL_STATE) & 0x40) {
		W(DUEL_HP0) = (uint16_t)(W(DUEL_HP0) - W(DUEL_RECOIL0));
		B(DUEL_STATE) &= ~0x40;
	}
	W(DUEL_RECOIL0) = W(DUEL_SPEED0);
	W(DUEL_SPEED0) = 0;
	B(DUEL_TMR_GAS0) = 8;
	B(DUEL_TMR_REC0) = 8;
	B(DUEL_STATE) |= duel_coin() ? 4 : 2;
	W(SHAGGY_FACING) = ROM16(DUEL_HEADING_OPP + 2 * (W(ANIM0) & 0xFFFF));
}
/* $003CFE: the same for the Clown's car */
static void duel_wall1(hle_ctx *ctx)
{
	sound_call(ctx, SOUND_PLAY, 0x60);
	if (B(DUEL_STATE) & 0x80) {
		W(DUEL_HP1) = (uint16_t)(W(DUEL_HP1) - W(DUEL_RECOIL1));
		B(DUEL_STATE) &= ~0x80;
	}
	W(DUEL_RECOIL1) = W(DUEL_SPEED1);
	W(DUEL_SPEED1) = 0;
	B(DUEL_TMR_GAS1) = 8;
	B(DUEL_TMR_REC1) = 8;
	B(DUEL_STATE) |= duel_coin() ? 0x10 : 8;
	W(CLOWN_FACING) = ROM16(DUEL_HEADING_OPP + 2 * (W(ANIM2) & 0xFFFF));
}

/* $003AF0: the cars bump. Each car's damage to the other is its speed, halved when it does
 * not face the other, none when the other car faces it from behind (opposite heading);
 * a sound says who got the worse of it ($25 Shaggy's side, $28 the Clown's); then both are
 * pushed apart (recoil = speed, at least 4) and spin by a random sign. */
static void duel_bump(hle_ctx *ctx)
{
	B(DUEL_STATE) |= 0xC0;
	sound_call(ctx, SOUND_PLAY, 0x72);
	uint16_t d4 = (uint16_t)duel_bearing();            /* heading to the Clown */
	uint16_t d5 = ROM16(DUEL_HEADING_OPP + 2 * d4);    /* heading to Shaggy */
	uint16_t d0 = W(DUEL_SPEED0);                      /* Shaggy's hit on the Clown: his speed, */
	if (d4 != W(ANIM0)) d0 >>= 1;                      /* halved unless he faces the Clown; */
	if (d5 == W(ANIM2)) d0 = 0;                        /* none if the Clown faces him (bumper to bumper), */
	else if (d4 == W(ANIM2)) d0 >>= 1;                 /* half if the Clown faces away */
	W(DUEL_HP1) = (uint16_t)(W(DUEL_HP1) - d0);
	uint16_t dealt = d0;
	d0 = W(DUEL_SPEED1);                               /* the Clown's hit on Shaggy, the same way round */
	if (d5 != W(ANIM2)) d0 >>= 1;
	if (d4 == W(ANIM0)) d0 = 0;
	else if (d5 == W(ANIM0)) d0 >>= 1;
	W(DUEL_HP0) = (uint16_t)(W(DUEL_HP0) - d0);
	if (s16(dealt) != s16(d0))
		sound_call(ctx, SOUND_PLAY, s16(dealt) < s16(d0) ? 0x28 : 0x25);
	/* the Clown is pushed away from Shaggy: recoil = Shaggy's speed, at least 4 */
	W(DUEL_RECOIL1) = W(DUEL_SPEED0);
	if (s16(W(DUEL_RECOIL1)) < 4) W(DUEL_RECOIL1) = 4;
	W(DUEL_SPEED1) = 0;
	B(DUEL_TMR_GAS1) = 8;
	B(DUEL_TMR_REC1) = 8;
	B(DUEL_STATE) |= duel_coin() ? 0x10 : 8;
	W(CLOWN_FACING) = d4;
	d4 = ROM16(DUEL_HEADING_OPP + 2 * d4);
	/* Shaggy is pushed away from the Clown: recoil = the Clown's (now zero) speed -> 4 */
	W(DUEL_RECOIL0) = W(DUEL_SPEED1);
	if (s16(W(DUEL_RECOIL0)) < 4) W(DUEL_RECOIL0) = 4;
	W(DUEL_SPEED0) = 0;
	B(DUEL_TMR_GAS0) = 8;
	B(DUEL_TMR_REC0) = 8;
	B(DUEL_STATE) |= duel_coin() ? 4 : 2;
	W(SHAGGY_FACING) = d4;
}

/* $0039D4: do the two cars' hit boxes overlap? Each car's box is four offsets from its
 * position, picked by its heading from the animation set (Shaggy's $0F88DC, the Clown's
 * $0FA77A: offsets into the set header) */
static bool duel_overlap(void)
{
	/* Shaggy's box */
	uint32_t set = 0x0F88DC;
	uint32_t i = 2 * (uint32_t)(uint16_t)W(ANIM0) + ROM16(set + 4);
	uint32_t box = set + 0xE + (uint32_t)(int32_t)(int16_t)ROM16(set + (uint32_t)(int32_t)(int16_t)i);
	int16_t x = (int16_t)W(ACTOR_X), y = (int16_t)W(ACTOR_Y);
	int16_t x1 = (int16_t)(x - ROM16(box)), y1 = (int16_t)(y - ROM16(box + 2));
	int16_t x2 = (int16_t)(x - ROM16(box + 4)), y2 = (int16_t)(y - ROM16(box + 6));
	/* the Clown's box */
	set = 0x0FA77A;
	i = 2 * (uint32_t)(uint16_t)W(ANIM2) + ROM16(set + 4);
	box = set + 0xE + (uint32_t)(int32_t)(int16_t)ROM16(set + (uint32_t)(int32_t)(int16_t)i);
	x = (int16_t)W(ACTOR2_X);
	y = (int16_t)W(ACTOR2_Y);
	int16_t u1 = (int16_t)(x - ROM16(box)), v1 = (int16_t)(y - ROM16(box + 2));
	int16_t u2 = (int16_t)(x - ROM16(box + 4)), v2 = (int16_t)(y - ROM16(box + 6));
	/* x intervals [x1,x2] and [u1,u2] overlap (as the 68k compares them) */
	if (x1 > u1) {
		if (u2 < x1) return false;
	} else {
		if (!(x2 >= u1)) return false;
	}
	if (y1 > v1)
		return !(v2 < y1);
	return y2 >= v1;
}

/* ---- setup ($003142-$003318): HInt counter, the meter frame tiles, the arena's HUD, the two
 * meters, the cars' start animation; the 68k then runs the actor frames until it loops */
static void duel_vram_word(hle_ctx *ctx, uint32_t addr, uint32_t val)
{
	D(0) = addr;
	D(1) = val;
	hle_call68k(ctx, VDP_WRITE_WORD);
}
/* 5 rows of 4 words of the HUD map to plane A (stride 128 bytes) */
static void duel_hud_map(hle_ctx *ctx, uint32_t vram, uint32_t src)
{
	for (int row = 0; row < 5; row++) {
		for (int i = 0; i < 4; i++) {
			duel_vram_word(ctx, vram, (uint32_t)ROM16(src) + 0x49C);
			src += 2;
			vram += 2;
		}
		src += 8;
		vram += 0x78;
	}
}

HLE_RANGE(0x003142, script_sys_minigame_tug, 0, HLE_EXIT_SP(TUG_SETUP_WAIT, -2))
{
	vdp_ctrl_w(0x8A94);                                /* H interrupt counter: the verb bar's raster split */
	push16(ctx, W(SHAGGY_FACING));                     /* restored at the end */
	/* two tiles of frame graphics (VRAM $FF00) */
	uint32_t p = TEXT_BUF;
	RAM32_W(p, 0xFF); p += 4;
	for (int i = 0; i < 6; i++) { RAM32_W(p, 0xF0); p += 4; }
	RAM32_W(p, 0xFF); p += 4;
	RAM32_W(p, 0xFFFFFFFF); p += 4;
	for (int i = 0; i < 6; i++) { RAM32_W(p, 0); p += 4; }
	RAM32_W(p, 0xFFFFFFFF);
	A(0) = TEXT_BUF;
	D(0) = 0xFF00;
	set_w(&D(1), 0x20);
	hle_call68k(ctx, VDP_DMA_VRAM);
	/* the meters' frames: two rows of 12 cells (plane A rows, +$40 when the 64-cell... flag) */
	uint32_t vram = 0xAB0A + ((RAM8(VBLANK_FLAGS_R) & 0x10) ? 0x40 : 0);
	p = TEXT_BUF;
	for (int r = 0; r < 2; r++) {
		W(p) = 0x07F8; p += 2;
		for (int i = 0; i < 8; i++) { W(p) = 0x07F9; p += 2; }
		W(p) = 0x0FF8; p += 2;
		W(p) = W(UI_TILE_BASE_F); p += 2;
		W(p) = W(UI_TILE_BASE_F); p += 2;
	}
	A(0) = TEXT_BUF;
	D(0) = vram;
	set_w(&D(1), 0x18);
	hle_call68k(ctx, VDP_DMA_VRAM);
	A(0) = 0x01900C;                                   /* HUD tiles */
	set_w(&D(1), 0x1C0);
	D(0) = 0x9380;
	hle_call68k(ctx, VDP_DMA_VRAM);
	vram = 0xAB02 + ((RAM8(VBLANK_FLAGS_R) & 0x10) ? 0x40 : 0);
	duel_hud_map(ctx, vram, 0x01938C);                 /* the two portraits */
	duel_hud_map(ctx, vram + 0x36, 0x019394);
	B(DUEL_STATE) = 0;
	B(DUEL_STATE) |= 1;                                /* bit 0: duel running (cleared at the end) */
	B(SCRIPT_FLAGS_09E8) |= 4;                         /* camera locked */
	B(VERB_STATE) |= 0x40;
	W(SHAGGY_FACING) = 0;
	RAM32_W(DUEL_SPEED0, 0);                           /* speeds 0 */
	RAM32_W(DUEL_RECOIL0, 0);
	RAM32_W(DUEL_TMR_GAS0, 0);
	W(DUEL_HP0) = 63;
	W(DUEL_HP1) = 63;
	B(DUEL_TMR_STEER) = 0;
	W(DUEL_AIM) = 0xFFFF;
	duel_meters();
	A(0) = TEXT_BUF;
	D(0) = 0xA300;
	set_w(&D(1), 0x80);
	hle_call68k(ctx, VDP_COPY_TO_VRAM);
	A(0) = INV_NAMETABLE;
	D(0) = 0xA400;
	hle_call68k(ctx, VDP_COPY_TO_VRAM);
	RAM32_W(ACTOR_ANIMSET, 0x0F88DC);                  /* the cars' animation set */
	B(ACTORS_DEPTH_ON) &= ~1;
	W(ANIM0) = 2;                                      /* Shaggy's car, facing east */
	B(ACTORS_ANIM_NEW) |= 1;
	W(ACTOR_TIMER) = 0xFFFF;
	hle_jump(ctx, TUG_SETUP_WAIT);
	ctx->cycles = charge_total(42219);
}

/* the start animation has looped: stop the room music, start the duel music, fade the
 * screen in and run the countdown jingles */
HLE_RANGE(0x00331E, script_sys_minigame_tug_ready, 1045, TUG_SETUP_WAIT, 0x003346)
{
	if (!bit_test(ctx, ACTORS_ANIM_LOOP, 0)) {
		hle_jump(ctx, TUG_SETUP_WAIT);
		return;
	}
	sound_call(ctx, SOUND_STOP, 0x58);
	sound_call(ctx, SOUND_STOP, 0x71);
	sound_call(ctx, SOUND_PLAY, 0x7B);
	hle_jump(ctx, 0x003346);                           /* bsr wait_vblank, bsr room_fade_in */
}
HLE_RANGE(0x00334E, script_sys_minigame_tug_jingle1, 668, TUG_WAIT50)
{
	sound_call(ctx, SOUND_PLAY, 0x79);
	set_w(&D(0), 50);
	cc_logic16(ctx, 50);
	hle_jump(ctx, TUG_WAIT50);
}
HLE_RANGE(0x003360, script_sys_minigame_tug_jingle2, 668, TUG_WAIT100)
{
	sound_call(ctx, SOUND_PLAY, 0x32);
	set_w(&D(0), 100);
	cc_logic16(ctx, 100);
	hle_jump(ctx, TUG_WAIT100);
}

/* ---- the duel loop. $003372: wait_vblank, actor_animate, then: */
/* is anyone destroyed? ($0038C2/$0038A4: explosion animation 8, the result for the script) */
HLE_RANGE(0x00337A, script_sys_minigame_tug_check, 56, 0x00338E, TUG_END_ANIM)
{
	cc_logic16(ctx, W(DUEL_HP0));
	if (W(DUEL_HP0) & 0x8000) {                        /* Shaggy's car is gone: lost */
		W(ANIM0) = 8;
		B(ACTORS_ANIM_NEW) |= 1;
		W(OBJ0_RESULT) = 0;
		D(0) = 0;
		cc_logic32(ctx, 0);
		hle_jump(ctx, TUG_END_ANIM);
		return;
	}
	cc_logic16(ctx, W(DUEL_HP1));
	if (W(DUEL_HP1) & 0x8000) {                        /* the Clown's car is gone: won */
		W(ANIM2) = 8;
		bit_set(ctx, ACTORS_ANIM_NEW, 2);
		W(OBJ0_RESULT) = 1;
		cc_logic16(ctx, 1);
		D(0) = 2;
		cc_logic32(ctx, 2);
		hle_jump(ctx, TUG_END_ANIM);
		return;
	}
	hle_jump(ctx, 0x00338E);
}

/* Start pressed this frame (and not the frame before): pause the sound and the game */
#define TUG_BODY 0x0033D8
HLE_RANGE(0x00338E, script_sys_minigame_tug_pause, 30, TUG_BODY, TUG_PAUSE_W1)
{
	if (bit_test(ctx, PAD1, 7)) { hle_jump(ctx, TUG_BODY); return; }
	if (!bit_test(ctx, PAD_LAST, 7)) { hle_jump(ctx, TUG_BODY); return; }
	hle_call68k(ctx, SOUND_CMD_PAUSE);
	sound_call(ctx, SOUND_PLAY, 0x59);
	hle_jump(ctx, TUG_PAUSE_W1);
}
HLE_RANGE(0x0033B6, script_sys_minigame_tug_pause_wait1, 30, TUG_PAUSE_W1, TUG_PAUSE_W2)
{
	hle_jump(ctx, bit_test(ctx, PAD1, 7) ? TUG_PAUSE_W2 : TUG_PAUSE_W1);
}
HLE_RANGE(0x0033C4, script_sys_minigame_tug_pause_wait2, 30, TUG_PAUSE_W2, TUG_BODY)
{
	if (bit_test(ctx, PAD1, 7)) {
		hle_jump(ctx, TUG_PAUSE_W2);
		return;
	}
	hle_call68k(ctx, SOUND_PLAY);                      /* D0 still the pause jingle: plays it again */
	hle_call68k(ctx, SOUND_CMD_UNPAUSE);
	hle_jump(ctx, TUG_BODY);
}

/* ---- one frame of the duel ($0033D8-$003896). Registers are dead at the exit (the loop head
 * reloads everything): difftest --ignore-regs d0-d7,a0-a4 --ignore-flags. */
static void duel_frame(hle_ctx *ctx)
{
	A(0) = TEXT_BUF;                                   /* meters to VRAM */
	D(0) = 0xA300;
	set_w(&D(1), 0x80);
	hle_call68k(ctx, VDP_COPY_TO_VRAM);
	A(0) = INV_NAMETABLE;
	D(0) = 0xA400;
	hle_call68k(ctx, VDP_COPY_TO_VRAM);

	for (uint32_t t = DUEL_TMR_GAS0; t <= DUEL_TMR_STEER; t++)
		if (B(t)) B(t)--;

	/* recoils die down one step per timer expiry; when over, the turn flags go */
	if (W(DUEL_RECOIL1)) {
		if (!B(DUEL_TMR_REC1)) {
			B(DUEL_TMR_REC1) = (uint8_t)(7 - W(DUEL_RECOIL1));
			W(DUEL_RECOIL1)--;
		}
	} else {
		B(DUEL_STATE) &= 0xE7;
		B(DUEL_STATE) &= 0x7F;
	}
	if (W(DUEL_RECOIL0)) {
		if (!B(DUEL_TMR_REC0)) {
			B(DUEL_TMR_REC0) = (uint8_t)(7 - W(DUEL_RECOIL0));
			W(DUEL_RECOIL0)--;
		}
	} else {
		B(DUEL_STATE) &= 0xF9;
		B(DUEL_STATE) &= 0xBF;
	}

	/* gas: the Clown always accelerates; Shaggy with A or B, else he slows down */
	if (!B(DUEL_TMR_GAS1) && W(DUEL_SPEED1) != 7) {
		W(DUEL_SPEED1)++;
		B(DUEL_TMR_GAS1) = (uint8_t)(W(DUEL_SPEED1) + 8);
	}
	if (!(RAM8(PAD1) & 0x40) || !(RAM8(PAD1) & 0x10)) {
		if (!B(DUEL_TMR_GAS0) && W(DUEL_SPEED0) != 7) {
			W(DUEL_SPEED0)++;
			B(DUEL_TMR_GAS0) = (uint8_t)(W(DUEL_SPEED0) + 8);
		}
	} else if (W(DUEL_SPEED0) && !B(DUEL_TMR_GAS0)) {
		B(DUEL_TMR_GAS0) = (uint8_t)(7 - W(DUEL_SPEED0));
		W(DUEL_SPEED0)--;
	}

	/* the Clown steers towards Shaggy: one heading step per turn, at most every 20 frames,
	 * and each turn costs a speed point */
	if (!(W(DUEL_RECOIL1) && W(DUEL_SPEED1)) && (B(ACTORS_ANIM_DONE) & 4)) {
		uint16_t h = W(ANIM2);
		if (B(DUEL_STATE) & 8) h++;                    /* recoil spin */
		if (B(DUEL_STATE) & 0x10) h--;
		h &= 7;
		uint16_t aim = (uint16_t)duel_aim();
		if (aim != h && !B(DUEL_TMR_STEER)) {
			B(DUEL_TMR_STEER) = 0x14;
			if (aim != W(DUEL_AIM)) {                  /* new target: pick the shorter way round */
				W(DUEL_AIM) = aim;
				uint16_t t = (uint16_t)((aim - h) & 7);
				if ((t > 4 && !(B(DUEL_STATE) & 8)) || (B(DUEL_STATE) & 0x10))
					W(DUEL_TURN) = 0xFFFF;
				else
					W(DUEL_TURN) = 1;
			}
			h = (uint16_t)((h + W(DUEL_TURN)) & 7);
		}
		if (h != W(ANIM2)) {
			W(DUEL_SPEED1)--;
			if (s16(W(DUEL_SPEED1)) < 0) W(DUEL_SPEED1) = 0;
			W(ANIM2) = h;
			B(ACTORS_ANIM_NEW) |= 4;
		}
	}

	/* Shaggy steers with Left/Right (and the recoil spin) once his last turn has been drawn */
	if ((W(DUEL_RECOIL0) || W(DUEL_SPEED0)) && (B(ACTORS_ANIM_DONE) & 1)) {
		uint16_t h = W(ANIM0);
		if (B(DUEL_STATE) & 2) h++;
		if (!(RAM8(PAD1) & 8)) h++;                    /* Right */
		if (B(DUEL_STATE) & 4) h--;
		if (!(RAM8(PAD1) & 4)) h--;                    /* Left */
		h &= 7;
		if (h != W(ANIM0)) {
			W(DUEL_SPEED0)--;
			if (s16(W(DUEL_SPEED0)) < 0) W(DUEL_SPEED0) = 0;
			W(ANIM0) = h;
			B(ACTORS_ANIM_NEW) |= 1;
		}
	}

	/* move the Clown's car: heading vector + recoil vector; a blocked cell = wall hit */
	B(DUEL_HIT) &= 0xFC;
	{
		unsigned v = (unsigned)((W(ANIM2) << 5) + W(DUEL_SPEED1) * 2) & 0xFFFF;
		unsigned r = (unsigned)((W(CLOWN_FACING) << 5) + W(DUEL_RECOIL1) * 2) & 0xFFFF;
		uint16_t x = (uint16_t)(W(ACTOR2_X) + duel_vec(v) + duel_vec(r));
		uint16_t y = (uint16_t)(W(ACTOR2_Y) + duel_vec(v + 16) + duel_vec(r + 16));
		W(ACTOR2_X) = x; W(ACTOR2_Y) = y;
		W(LINE_PROBE_X) = x; W(LINE_PROBE_Y) = y;
		hle_call68k(ctx, LINE_PROBE);
		if (!(ctx->sr & CC_Z)) {
			duel_wall1(ctx);
			B(DUEL_HIT) |= 2;
		}
	}
	/* ... and Shaggy's */
	{
		unsigned v = (unsigned)((W(ANIM0) << 5) + W(DUEL_SPEED0) * 2) & 0xFFFF;
		unsigned r = (unsigned)((W(SHAGGY_FACING) << 5) + W(DUEL_RECOIL0) * 2) & 0xFFFF;
		uint16_t x = (uint16_t)(W(ACTOR_X) + duel_vec(v) + duel_vec(r));
		uint16_t y = (uint16_t)(W(ACTOR_Y) + duel_vec(v + 16) + duel_vec(r + 16));
		W(ACTOR_X) = x; W(ACTOR_Y) = y;
		W(LINE_PROBE_X) = x; W(LINE_PROBE_Y) = y;
		hle_call68k(ctx, LINE_PROBE);
		if (!(ctx->sr & CC_Z)) {
			duel_wall0(ctx);
			B(DUEL_HIT) |= 1;
		}
	}

	if (duel_overlap()) {                              /* the cars bump: both go back */
		duel_bump(ctx);
		W(ACTOR_X) = W(DUEL_PREV);
		W(ACTOR_Y) = W(DUEL_PREV + 2);
		W(ACTOR2_X) = W(DUEL_PREV + 4);
		W(ACTOR2_Y) = W(DUEL_PREV + 6);
		W(ANIM0) = W(DUEL_PREV + 8);
		W(ANIM2) = W(DUEL_PREV + 10);
		B(ACTORS_ANIM_NEW) |= 5;
		W(ACTOR_TIMER) = 0xFFFF;
		W(ACTOR_TIMER + 4) = 0xFFFF;
	} else {
		if (B(DUEL_HIT) & 1) {                         /* Shaggy hit a wall: back, redraw */
			W(ACTOR_X) = W(DUEL_PREV);
			W(ACTOR_Y) = W(DUEL_PREV + 2);
			W(ANIM0) = W(DUEL_PREV + 8);
			W(ACTOR_TIMER) = 0xFFFF;
			B(ACTORS_ANIM_NEW) |= 1;
		}
		if (B(DUEL_HIT) & 2) {
			W(ACTOR2_X) = W(DUEL_PREV + 4);
			W(ACTOR2_Y) = W(DUEL_PREV + 6);
			W(ANIM2) = W(DUEL_PREV + 10);
			W(ACTOR_TIMER + 4) = 0xFFFF;
			B(ACTORS_ANIM_NEW) |= 4;
		}
	}
	W(DUEL_PREV + 10) = W(ANIM2);                      /* this frame is next frame's undo point */
	W(DUEL_PREV + 8) = W(ANIM0);
	W(DUEL_PREV) = W(ACTOR_X);
	W(DUEL_PREV + 2) = W(ACTOR_Y);
	W(DUEL_PREV + 4) = W(ACTOR2_X);
	W(DUEL_PREV + 6) = W(ACTOR2_Y);
	duel_meters();
}
HLE_RANGE(0x0033D8, script_sys_minigame_tug_frame, 0, TUG_FRAME_WAIT)
{
	duel_frame(ctx);
	hle_jump(ctx, TUG_FRAME_WAIT);
	ctx->cycles = charge_total(27157);
}

/* ---- the end: wait for the explosion, fade out, restore ($0038DC-$003938) */
HLE_RANGE(0x0038E0, script_sys_minigame_tug_end_wait, 26, TUG_END_ANIM, TUG_FADE_OUT)
{
	if (!bit_test(ctx, ACTORS_ANIM_DONE, D(0) & 7)) {
		hle_jump(ctx, TUG_END_ANIM);
		return;
	}
	hle_jump(ctx, TUG_FADE_OUT);                       /* bsr script_subop_03_fade_out */
}
HLE_RANGE(0x0038EC, script_sys_minigame_tug_end, 0, HLE_EXIT_SP(TUG_RTS, 2))
{
	vdp_ctrl_w(0x8AA3);                                /* H interrupt counter back */
	cc_logic16(ctx, 0x8AA3);
	sound_call(ctx, SOUND_STOP, 0x7B);                 /* duel music off, room music */
	sound_call(ctx, SOUND_PLAY, 0x71);
	{
		uint16_t v = pop16(ctx);
		W(SHAGGY_FACING) = v;
		cc_logic16(ctx, v);
	}
	bit_clr(ctx, VERB_STATE, 6);
	RAM32_W(ACTOR_ANIMSET, 0x032700);                  /* back to Shaggy's normal animation set */
	cc_logic32(ctx, 0x032700);
	bit_set(ctx, ACTORS_DEPTH_ON, 0);
	bit_clr(ctx, DUEL_STATE, 0);
	bit_clr(ctx, SCRIPT_FLAGS_09E8, 2);
	hle_jump(ctx, TUG_RTS);
	ctx->cycles = charge_total(1494);
}
