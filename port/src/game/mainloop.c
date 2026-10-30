/* Main loop, interrupt handlers and frame timing (GitLab #12, docs/subsystems/mainloop.md).
 *
 * Frame structure in one paragraph: the VBlank vector ($0008BC) saves all registers and
 * calls the code pointer in VBLANK_HOOK ($FF0000); the HBlank vector ($0008AA) does the
 * same with HBLANK_HOOK ($FF0004). In gameplay VBLANK_HOOK = vblank_main, which pushes the
 * display state built by the main loop to the VDP (scroll, sprites, palette, HUD) and runs
 * the per-frame timers; on the title screen it is vblank_title_fade, during the mystery
 * intro vblank_light. The main loop (inside Reset, from $000D38) is NOT locked to the frame:
 * it runs as fast as it can (one pass takes ~1-4 frames in the Lobby, dominated by actor
 * frame decompression) and paces itself only through VBlank-decremented timers and the
 * wait_vblank handshake (bit 0 of VBLANK_FLAGS).
 */
#include "game.h"

/* ------------------------------------------------------------------ flag helpers */
/* SUB/SUBQ/SUBI/CMP (cmp: X unchanged) of `s` from `d` at `bits` width; returns d - s */
static uint32_t cc_sub(hle_ctx *c, uint32_t d, uint32_t s, int bits, bool set_x)
{
	uint32_t m = bits == 8 ? 0xFFu : bits == 16 ? 0xFFFFu : 0xFFFFFFFFu, sign = (m >> 1) + 1;
	d &= m;
	s &= m;
	uint32_t r = (d - s) & m;
	unsigned f = 0;
	if (r & sign) f |= CC_N;
	if (!r) f |= CC_Z;
	if ((d ^ s) & (d ^ r) & sign) f |= CC_V;
	if (s > d) f |= CC_C | (set_x ? CC_X : 0);
	cc_set(c, CC_N | CC_Z | CC_V | CC_C | (set_x ? CC_X : 0), f);
	return r;
}

/* ADD/ADDQ/ADDI at `bits` width; returns d + s */
static uint32_t cc_add(hle_ctx *c, uint32_t d, uint32_t s, int bits)
{
	uint32_t m = bits == 8 ? 0xFFu : bits == 16 ? 0xFFFFu : 0xFFFFFFFFu, sign = (m >> 1) + 1;
	d &= m;
	s &= m;
	uint32_t r = (d + s) & m;
	unsigned f = 0;
	if (r & sign) f |= CC_N;
	if (!r) f |= CC_Z;
	if (~(d ^ s) & (d ^ r) & sign) f |= CC_V;
	if (r < d) f |= CC_C | CC_X;
	cc_set(c, CC_N | CC_Z | CC_V | CC_C | CC_X, f);
	return r;
}

/* BTST/BSET/BCLR: Z = !(old bit) */
static void cc_btst(hle_ctx *c, uint32_t v, int bit) { cc_set(c, CC_Z, (v >> bit) & 1 ? 0 : CC_Z); }

/* "tst.b (counter) / bmi.s skip / subi.b #1,(counter)": decrement a byte countdown that
 * stops at -1. Same for words. */
static void countdown8(hle_ctx *c, uint32_t addr)
{
	uint8_t v = RAM8(addr);
	cc_logic8(c, v);
	if (!(v & 0x80))
		RAM8(addr) = (uint8_t)cc_sub(c, v, 1, 8, true);
}

static void countdown16(hle_ctx *c, uint32_t addr)
{
	uint16_t v = RAM16(addr);
	cc_logic16(c, v);
	if (!(v & 0x8000))
		RAM16(addr) = (uint16_t)cc_sub(c, v, 1, 16, true);
}

/* ------------------------------------------------------------------ VBlank handlers */

/* $00A4A4 vblank_light: the per-frame minimum every VBlank handler ends with
 * (vblank_main has an identical copy at $00A6C0, vblank_title_fade falls into this one).
 *   bsr pad_read
 *   btst #7,(PAD1_CUR) / bne + / bset #0,(SCRIPT_FLAGS2)   Start held -> "skip" request
 *   + tst.b (VBL_COUNTDOWN_B) / bmi + / subi.b #1,(...)
 *   + bclr #0,(VBLANK_FLAGS)                                 release wait_vblank
 *   tst.w (FRAME_COUNTDOWN) / bmi + / subq.w #1,(...)
 *   + rts */
static void vblank_tail(hle_ctx *ctx)
{
	pad_read(ctx);
	cc_btst(ctx, RAM8(PAD1_CUR), 7);
	if (!(RAM8(PAD1_CUR) & 0x80)) {                  /* Start pressed (active low) */
		cc_btst(ctx, RAM8(SCRIPT_FLAGS2), 0);
		RAM8(SCRIPT_FLAGS2) |= SF2_START_PRESSED;
	}
	countdown8(ctx, VBL_COUNTDOWN_B);
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 0);
	RAM8(VBLANK_FLAGS) &= (uint8_t)~VBF_WAITING;      /* wakes up wait_vblank */
	countdown16(ctx, FRAME_COUNTDOWN);
}

HLE_FUNC(0x00A4A4, vblank_light, 330)
{
	vblank_tail(ctx);
}

/* $00A65C vblank_main: in-game VBlank.
 *   VDP reg 2 = $30 (plane A at $C000), reg 4 = $07 (plane B at $E000), reg 5 = $6C (SAT
 *   at $D800): undo the verb-bar split that hblank_verb_bar made at line 167.
 *   bsr sub_00FA78                     scroll registers / camera
 *   if VBF_FRAME_WORK (bit 6) is set: clear it (re-entrancy guard; also how the main loop
 *   pauses this work while it rebuilds the screen), then
 *     bsr sub_00A7CE  palette cycling       bsr sub_005C8E  HUD/inventory DMA on request
 *     bsr sub_00AF24  actor tiles + sprite table DMA, actor timers
 *     move.w #$2300,sr                     let the HBlank interrupt in (see below)
 *     bsr sub_00A704  status meters        if VBF_OBJ_TIMERS: bsr sub_007C04 object timers
 *     bsr sub_005E4A  scripted Shaggy path
 *     set bit 6 again; move.w #$2600,sr
 *   then the common tail (vblank_tail).
 * The 68k lowers the mask to 2 so that HBlank can preempt this long handler. In C the work
 * takes no emulated time, so the mask is left at 6 (lowering it here would let a pending
 * HBlank run inside the nested 68k calls, where hblank_verb_bar's raster wait never ends);
 * only the final SR value of the original (#$2600, i.e. CCR = 0) is reproduced. */
HLE_FUNC(0x00A65C, vblank_main, 900)
{
	vdp_ctrl_w(0x8230);
	vdp_ctrl_w(0x8407);
	vdp_ctrl_w(0x856C);
	hle_call68k(ctx, 0x00FA78);
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 6);
	if (RAM8(VBLANK_FLAGS) & VBF_FRAME_WORK) {
		RAM8(VBLANK_FLAGS) &= (uint8_t)~VBF_FRAME_WORK;
		hle_call68k(ctx, 0x00A7CE);
		hle_call68k(ctx, 0x005C8E);
		hle_call68k(ctx, 0x00AF24);
		ctx->sr = (uint16_t)((ctx->sr & 0xFF00) | 0x00);   /* move.w #$2300,sr: CCR = 0 (mask kept, see above) */
		hle_call68k(ctx, 0x00A704);
		cc_btst(ctx, RAM8(VBLANK_FLAGS), 2);
		if (RAM8(VBLANK_FLAGS) & VBF_OBJ_TIMERS)
			hle_call68k(ctx, 0x007C04);
		hle_call68k(ctx, 0x005E4A);
		cc_btst(ctx, RAM8(VBLANK_FLAGS), 6);
		RAM8(VBLANK_FLAGS) |= VBF_FRAME_WORK;
		ctx->sr = 0x2600;                                  /* move.w #$2600,sr */
	}
	vblank_tail(ctx);
}

/* $00879E title_anim: title-screen animation, called from vblank_title_fade.
 *   - copies TITLE_SCROLL to $FF0627 while $FF09ED bit 4 is set,
 *   - ping-pongs TITLE_SCROLL between 0 and $40 in steps of 2 (direction TITLE_SCROLL_DIR),
 *   - every 4th frame scrolls the 64-word strip at $FF8C00 one word left and appends the
 *     next word of the 39-word loop at ROM $008828 (index TITLE_STRIP_INDEX). */
HLE_FUNC(0x00879E, title_anim, 260)
{
	cc_btst(ctx, RAM8(0xFF09ED), 4);
	if (RAM8(0xFF09ED) & 0x10) {
		RAM8(0xFF0627) = RAM8(TITLE_SCROLL);
		cc_logic8(ctx, RAM8(0xFF0627));
	}
	cc_logic8(ctx, RAM8(TITLE_SCROLL_DIR));
	if (!RAM8(TITLE_SCROLL_DIR)) {
		RAM8(TITLE_SCROLL) = (uint8_t)cc_add(ctx, RAM8(TITLE_SCROLL), 2, 8);
		cc_sub(ctx, RAM8(TITLE_SCROLL), 0x40, 8, false);
		if (RAM8(TITLE_SCROLL) == 0x40) {
			RAM8(TITLE_SCROLL_DIR) = 1;
			cc_logic8(ctx, 1);
		}
	} else {
		RAM8(TITLE_SCROLL) = (uint8_t)cc_sub(ctx, RAM8(TITLE_SCROLL), 2, 8, true);
		if (!RAM8(TITLE_SCROLL)) {
			RAM8(TITLE_SCROLL_DIR) = 0;
			cc_logic8(ctx, 0);
		}
	}
	RAM16(TITLE_STRIP_TIMER) = (uint16_t)cc_sub(ctx, RAM16(TITLE_STRIP_TIMER), 1, 16, true);
	if (!(RAM16(TITLE_STRIP_TIMER) & 0x8000))
		return;
	RAM16(TITLE_STRIP_TIMER) = 3;
	/* lea ram_planea_tmp,a0 / move.w #$3E,d1 / - move.w 2(a0),(a0)+ / dbra d1,- */
	ctx->a[0] = 0xFF8C00;
	for (int i = 0; i < 0x3F; i++, ctx->a[0] += 2)
		RAM16(ctx->a[0]) = RAM16(ctx->a[0] + 2);
	set_w(&ctx->d[1], 0xFFFF);
	set_w(&ctx->d[0], RAM16(TITLE_STRIP_INDEX));
	RAM16(0xFF8C7E) = ROM16(0x008828 + (int16_t)ctx->d[0]);
	set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 2, 16));
	cc_sub(ctx, ctx->d[0], 0x4E, 16, false);
	if ((int16_t)ctx->d[0] >= 0x4E)
		ctx->d[0] = 0;                                     /* moveq #0,d0 */
	RAM16(TITLE_STRIP_INDEX) = (uint16_t)ctx->d[0];
	cc_logic16(ctx, ctx->d[0]);
}

/* $00A38C vblank_title_fade: VBlank of the title screen. While $FF09EB bit 7 is set it
 * plays the random "lightning" effect: after a random delay (TITLE_FADE_DELAY, rng 0..199
 * frames) it flashes palette 2 (CRAM $40) with the 16 colours at $00A558, copies one of
 * three random picture pieces (table $00A578: source, x, y, w, h) into plane B, then
 * fades the palette back through the 4 steps of table $00A4E8 (one step every 3 frames)
 * and finally restores the piece from the clean picture at $0165B8.
 * Then title_anim and the common VBlank tail. */
HLE_FUNC(0x00A38C, vblank_title_fade, 600)
{
	cc_btst(ctx, RAM8(0xFF09EB), 7);
	if (RAM8(0xFF09EB) & 0x80) {
		int16_t delay = (int16_t)RAM16(TITLE_FADE_DELAY);
		cc_logic16(ctx, (uint16_t)delay);
		if (delay < 0) {
			/* fading back: one palette step every 3 frames */
			RAM16(TITLE_FADE_TICK) = (uint16_t)cc_sub(ctx, RAM16(TITLE_FADE_TICK), 1, 16, true);
			if ((int16_t)RAM16(TITLE_FADE_TICK) < 0) {
				RAM16(TITLE_FADE_TICK) = 2;
				set_w(&ctx->d[0], RAM16(TITLE_FADE_STEP));
				ctx->a[0] = ROM32(0x00A4E8 + (int16_t)ctx->d[0]);
				set_w(&ctx->d[1], 0x10);
				set_w(&ctx->d[0], 0x40);
				hle_call68k(ctx, 0x009B10);                /* vdp_dma_cram */
				RAM16(TITLE_FADE_STEP) = (uint16_t)cc_sub(ctx, RAM16(TITLE_FADE_STEP), 4, 16, true);
				if ((int16_t)RAM16(TITLE_FADE_STEP) < 0) {
					/* faded back: restore the flashed piece of plane B from the clean picture */
					ctx->a[0] = 0x0165B8;
					set_w(&ctx->d[0], RAM16(TITLE_PIECE_VRAM));
					ctx->a[0] += (int16_t)ctx->d[0];
					set_w(&ctx->d[0], ctx->d[0] + 0xE000);
					set_w(&ctx->d[1], RAM16(TITLE_PIECE_W));
					set_w(&ctx->d[2], RAM16(TITLE_PIECE_H));
					set_w(&ctx->d[3], 0x40 - 2 * ctx->d[1]);   /* source skip to the next row */
					do {
						hle_call68k(ctx, 0x0099F6);        /* vdp_dma_vram */
						set_w(&ctx->d[0], ctx->d[0] + 0x40);
						ctx->a[0] += (int16_t)ctx->d[3];
						set_w(&ctx->d[2], ctx->d[2] - 1);
					} while ((uint16_t)ctx->d[2] != 0xFFFF);
					set_w(&ctx->d[0], 0xC8);
					hle_call68k(ctx, 0x0022D6);            /* rng_next: 0..199 */
					RAM16(TITLE_FADE_DELAY) = (uint16_t)ctx->d[0];
				}
			}
		} else {
			RAM16(TITLE_FADE_DELAY) = (uint16_t)cc_sub(ctx, (uint16_t)delay, 1, 16, true);
			if ((int16_t)RAM16(TITLE_FADE_DELAY) < 0) {
				/* flash: bright palette 2, then draw a random piece */
				RAM16(TITLE_FADE_STEP) = 0xC;
				RAM16(TITLE_FADE_TICK) = 2;
				set_w(&ctx->d[0], 0x40);
				set_w(&ctx->d[1], 0x10);
				ctx->a[0] = 0x00A558;
				hle_call68k(ctx, 0x009B10);                /* vdp_dma_cram */
				ctx->d[0] = 3;
				hle_call68k(ctx, 0x0022D6);                /* rng_next: 0..2 */
				set_w(&ctx->d[3], (ctx->d[0] & 0xFFFF) << 2);
				int16_t k = (int16_t)ctx->d[3];
				ctx->a[1] = 0x00A578;
				ctx->a[0] = ROM32(0x00A578 + k);
				set_w(&ctx->d[0], ROM16(0x00A578 + 0xE + k));      /* y */
				set_w(&ctx->d[1], ROM16(0x00A578 + 0xC + k) * 2);  /* x * 2 */
				set_w(&ctx->d[0], (ctx->d[0] << 6) + ctx->d[1]);  /* plane offset */
				RAM16(TITLE_PIECE_VRAM) = (uint16_t)ctx->d[0];
				set_w(&ctx->d[0], ctx->d[0] + 0xE000);
				set_w(&ctx->d[1], ROM16(0x00A578 + 0x18 + k));     /* width in words */
				set_w(&ctx->d[2], ROM16(0x00A578 + 0x1A + k));     /* rows - 1 */
				RAM16(TITLE_PIECE_W) = (uint16_t)ctx->d[1];
				RAM16(TITLE_PIECE_H) = (uint16_t)ctx->d[2];
				do {
					hle_call68k(ctx, 0x0099F6);            /* vdp_dma_vram */
					set_w(&ctx->d[0], ctx->d[0] + 0x40);
					set_w(&ctx->d[2], ctx->d[2] - 1);
				} while ((uint16_t)ctx->d[2] != 0xFFFF);
			}
		}
	}
	title_anim(ctx);
	vblank_tail(ctx);
}

/* $01006A hblank_nop: HBlank hook outside gameplay (rts). */
HLE_FUNC(0x01006A, hblank_nop, 16)
{
}
