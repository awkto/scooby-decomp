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
#include "cc68k.h"

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

/* ================================================================== main loop
 *
 * Reset ends in the game's main loop at $000D38 (see docs/subsystems/mainloop.md):
 *
 *   $D38  jsr sub_00AFDA          object event scripts              [68k: may block]
 *   $D3E  main_loop_pass          C: decide what this pass does; ends at one of:
 *     $D4A  bsr sub_0019B0        a room script is pending (VBF_SCRIPT)
 *     $D88  jsr palette_read_cram Start pressed: pause -> title menu -> $D8E
 *     $E86  bsr sub_0056B0        random ambient event spawned -> $E8A
 *     $116C/$1174/$11A8/$11B0     inventory scroll arrows (blocking redraws)
 *     $1228 jsr wait_vblank       after a verb-bar click
 *     $1392 bsr sub_0019B0        Shaggy walked into an object zone -> $1396
 *     $13B0 jsr sub_00B0B8        actor animation -> bra $D38
 *
 * Every exit is the original jsr/bsr of a function that can wait for VBlank (directly
 * or through script opcodes): interrupts cannot fire inside C, so the C code stops in
 * front of the call, the 68k instruction makes it, and the code after it is the next
 * HLE_RANGE ($D4E, $D5A, $D96, $E8A, $1178, $11B4, $122E, $1396).
 *
 * The 68k code leaves all its working values in registers, and difftest compares
 * registers at the exits, so the C below works directly on ctx->d[]/a[] where the
 * original does and reproduces every condition-code update on the way (the final
 * flags are compared too). */

#define CAMERA_X          0xFF06CA   /* word: world x of the screen's left edge */
#define CAMERA_Y          0xFF06CE
#define POINTER_X          0xFF06DC   /* word: pointer position on screen (pointer mode) */
#define POINTER_Y          0xFF06DE
#define HOVER_TARGET      0xFF06AE   /* word: target under the pointer / Shaggy: 0 none,
                                      * 2 Scooby, 10 verb-bar default, 3+n object n */
#define HOVER_PREV        0xFF06B4   /* word: HOVER_TARGET of the previous pass */
#define SECOND_TARGET     0xFF06B6   /* word: second target of a two-object verb */
#define SECOND_PREV       0xFF06BC
#define CUR_VERB          0xFF06C0   /* word: selected verb id (0 = none) */
#define CUR_VERB_PREV     0xFF06B8
#define VERB_TO_DRAW      0xFF06C2   /* word: verb id argument of draw_verb_button */
#define DEFAULT_VERB      0xFF06C4   /* word: default verb of the hovered target (highlighted) */
#define VERB_SLOT         0xFF06EA   /* word: verb-bar slot under the pointer (0 = none) */
#define INV_PAGE          0xFF06EC   /* word: inventory scroll position (rows of 4) */
#define INVENTORY         0xFF0A1E   /* words: carried object numbers, terminated by a negative */
#define OBJ_LAST          0xFF06F4   /* word: number of object records - 1 */
#define VERB_STATE        0xFF09DE   /* byte: bit0 waiting for the 2nd target, bit7 no ambient events */
#define VERB_STATE_PREV   0xFF09DF
#define ROOM_NAME_PREV    0xFF06D6   /* long: ROOM_NAME_PTR of the previous pass */
#define AMBIENT_TIMER     0xFF0688   /* word: ambient events are rolled when < 0 (then = 100) */
#define AMBIENT_COUNT     0xFF06A0   /* word: entries in the room's ambient event table */
#define AMBIENT_KIND      0xFF0640   /* word: event parameters for sub_0056B0 */
#define AMBIENT_ARG       0xFF063E
#define ZONE_TRIGGER      0xFF06BE   /* word: why sub_0019B0 runs a script (11 = walked in) */
#define SCRIPT_FLAGS      0xFF0AC9   /* byte: bit7 = zone script of HOVER_TARGET already ran */
#define ACTORS_ON         0xFF0AB4   /* byte: bit n = actor n present (0 Shaggy, 1 Scooby) */
#define ACTOR_X           0xFF04DC   /* 6 longs, 16.16: actor n x at +4n (Shaggy = actor 0) */
#define ACTOR_Y           0xFF04F4   /* 6 longs, 16.16 */
#define OBJ_REC_SIZE      0x1A       /* object record at OBJECTS + 26 * n (see the doc) */

/* objects: +2 hot-spot number (1-based, 0 = none), +6 room (1 = carried), +$16 default
 * verb (byte), +$18 flags (bit1 = hot spot follows actor +$19 + 2), +$19 actor index */
#define OBJECTS           0xFF1200
#define ROOM_INDEX        0xFF06AC   /* word: current room number */
#define MYSTERY_HDR       0xFF068E   /* long: header of the current mystery (ROM) */
#define ROOM_NAME_PTR     0xFF06D2   /* long: name string of the current room */

static void mainloop_exit(hle_ctx *ctx, uint32_t addr) { hle_jump(ctx, addr); }

/* $001222: clr.w (VERB_SLOT) then jsr wait_vblank at $1228 */
static void clear_slot_then_wait(hle_ctx *ctx)
{
	RAM16(VERB_SLOT) = 0;
	cc_logic16(ctx, 0);
	mainloop_exit(ctx, 0x001228);
}

/* $001280..$0013B0: if no verb is selected, highlight the default verb of the hovered
 * target (draw_verb_button state 2), then exit to the actor update at $13B0. */
static void highlight_default_verb(hle_ctx *ctx)
{
	cc_logic16(ctx, RAM16(CUR_VERB));
	if (RAM16(CUR_VERB) == 0) {
		set_w(&ctx->d[0], RAM16(HOVER_TARGET));
		cmpw(ctx, ctx->d[0], 3);
		bool have = true;
		if ((int16_t)ctx->d[0] < 3) {
			cc_logic16(ctx, ctx->d[0]);
			if ((ctx->d[0] & 0xFFFF) == 0)
				have = false;
			else
				set_w(&ctx->d[1], 0xA);                  /* Scooby: verb 10 */
		} else {
			set_w(&ctx->d[0], cc_sub(ctx, ctx->d[0], 3, 16, true));
			if ((int16_t)ctx->d[0] < 0)
				have = false;
			else {
				/* mulu.w #26,d0 / move.b $16(a2,d0.w),d1 / ext.w d1 */
				ctx->d[0] = (ctx->d[0] & 0xFFFF) * OBJ_REC_SIZE;
				cc_logic32(ctx, ctx->d[0]);
				set_b(&ctx->d[1], RAM8(ctx->a[2] + 0x16 + (int16_t)ctx->d[0]));
				set_w(&ctx->d[1], (uint32_t)(int16_t)(int8_t)ctx->d[1]);
				cc_logic16(ctx, ctx->d[1]);
			}
		}
		if (have) {
			RAM16(DEFAULT_VERB) = (uint16_t)ctx->d[1];
			cc_logic16(ctx, ctx->d[1]);
			if (ctx->d[1] & 0xFFFF) {
				RAM16(VERB_TO_DRAW) = (uint16_t)ctx->d[1];
				ctx->d[0] = 2;
				cc_logic32(ctx, 2);
				hle_call68k(ctx, 0x005BDC);            /* draw_verb_button(highlighted) */
			}
		}
	}
	mainloop_exit(ctx, 0x0013B0);
}

/* $00122E: the hovered target changed -> un-highlight the default verb of the previous
 * one (draw_verb_button state 0), then highlight_default_verb. */
static void update_verb_highlight(hle_ctx *ctx)
{
	ctx->a[2] = OBJECTS;
	set_w(&ctx->d[0], RAM16(HOVER_PREV));
	cmpw(ctx, ctx->d[0], RAM16(HOVER_TARGET));
	if ((ctx->d[0] & 0xFFFF) != RAM16(HOVER_TARGET)) {
		bool have = true;
		cmpw(ctx, ctx->d[0], 3);
		if ((int16_t)ctx->d[0] < 3) {
			cc_logic16(ctx, ctx->d[0]);
			if ((ctx->d[0] & 0xFFFF) == 0)
				have = false;
			else {
				set_w(&ctx->d[0], 0xA);
				cc_logic16(ctx, 0xA);
			}
		} else {
			ctx->a[2] = OBJECTS;
			set_w(&ctx->d[0], cc_sub(ctx, ctx->d[0], 3, 16, true));
			if ((int16_t)ctx->d[0] < 0)
				have = false;
			else {
				ctx->d[0] = (ctx->d[0] & 0xFFFF) * OBJ_REC_SIZE;
				cc_logic32(ctx, ctx->d[0]);
				set_b(&ctx->d[0], RAM8(ctx->a[2] + 0x16 + (int16_t)ctx->d[0]));
				set_w(&ctx->d[0], (uint32_t)(int16_t)(int8_t)ctx->d[0]);
				cc_logic16(ctx, ctx->d[0]);
			}
		}
		if (have) {
			cmpw(ctx, ctx->d[0], RAM16(CUR_VERB));
			if ((ctx->d[0] & 0xFFFF) != RAM16(CUR_VERB)) {
				RAM16(VERB_TO_DRAW) = (uint16_t)ctx->d[0];
				cc_logic16(ctx, ctx->d[0]);
				ctx->d[0] = 0;
				cc_logic32(ctx, 0);
				hle_call68k(ctx, 0x005BDC);            /* draw_verb_button(normal) */
			}
		}
	}
	highlight_default_verb(ctx);
}

/* Is (d0,d1) - a cell position - inside the rectangle of hot spot `obj+2`?
 * $001018 / $00130C:  rect = hdr[$1C] + hdr[$24][hotspot-1] -> x, y, w, h (cells).
 * Leaves d4/d5/d6/a2 like the original. Returns false early with the flags of the
 * failing test. */
static bool hotspot_hit(hle_ctx *ctx)
{
	set_w(&ctx->d[4], RAM16(ctx->a[0] + 2));
	cc_logic16(ctx, ctx->d[4]);
	if (!(ctx->d[4] & 0xFFFF))
		return false;
	set_w(&ctx->d[4], cc_sub(ctx, ctx->d[4], 1, 16, true));
	set_w(&ctx->d[4], lslw(ctx, ctx->d[4], 2));
	ctx->d[4] = bus_r32(ctx->a[3] + (int16_t)ctx->d[4]);
	cc_logic32(ctx, ctx->d[4]);
	ctx->a[2] = ctx->a[1] + ctx->d[4];
	set_w(&ctx->d[5], bus_r16(ctx->a[2]));
	cc_logic16(ctx, ctx->d[5]);
	set_w(&ctx->d[6], bus_r16(ctx->a[2] + 2));
	cc_logic16(ctx, ctx->d[6]);
	if (cmpw(ctx, ctx->d[0], ctx->d[5]) < 0)
		return false;
	if (cmpw(ctx, ctx->d[1], ctx->d[6]) < 0)
		return false;
	set_w(&ctx->d[5], cc_add(ctx, ctx->d[5], bus_r16(ctx->a[2] + 4), 16));
	set_w(&ctx->d[6], cc_add(ctx, ctx->d[6], bus_r16(ctx->a[2] + 6), 16));
	if (cmpw(ctx, ctx->d[0], ctx->d[5]) >= 0)
		return false;
	if (cmpw(ctx, ctx->d[1], ctx->d[6]) >= 0)
		return false;
	return true;
}

/* $000F72-$001064 / $0012FA-$001358: the object loop shared by pointer and walk mode.
 * For every object in the current room (d3) whose hot spot contains the cell (d0,d1),
 * d7 = object number + 3 (the last hit wins). In pointer mode, objects that follow an
 * actor are tested against a box around that actor instead (box extents per hot spot in
 * the ROM tables $032550 left, $032598 right, $0324C0 up, $032508 down); in walk mode
 * they are skipped. */
static void find_target_object(hle_ctx *ctx, bool pointer_mode)
{
	do {
		uint32_t obj = ctx->a[0];
		bool hit = false;
		if (cmpw(ctx, ctx->d[3], RAM16(obj + 6)) == 0) {           /* in this room */
			cc_btst(ctx, RAM8(obj + 0x18), 1);
			bool follows_actor = RAM8(obj + 0x18) & 2;
			if (pointer_mode && follows_actor) {
				uint32_t saved_d2 = ctx->d[2];
				cc_logic16(ctx, ctx->d[2]);                       /* move.w d2,-(sp) */
				ctx->a[2] = 0x032670;
				ctx->a[5] = ACTOR_X + 8;                          /* actor (+$19) + 2 */
				set_b(&ctx->d[4], RAM8(obj + 0x19));
				set_w(&ctx->d[4], ctx->d[4] & 0xFF);
				set_w(&ctx->d[4], lslw(ctx, ctx->d[4], 2));
				set_w(&ctx->d[2], RAM16(obj + 2));
				set_w(&ctx->d[2], cc_sub(ctx, ctx->d[2], 1, 16, true));
				set_w(&ctx->d[2], cc_add(ctx, ctx->d[2], ctx->d[2], 16));
				int16_t ai = (int16_t)ctx->d[4], hi = (int16_t)ctx->d[2];
				uint16_t ax = RAM16(ctx->a[5] + ai), ay = RAM16(ctx->a[5] + 0x18 + ai);
				hit = true;
				ctx->a[4] = 0x032550;
				set_w(&ctx->d[5], cc_sub(ctx, ax, bus_r16(ctx->a[4] + hi), 16, true));
				set_w(&ctx->d[6], cc_add(ctx, RAM16(POINTER_X), RAM16(CAMERA_X), 16));
				if (cmpw(ctx, ctx->d[5], ctx->d[6]) > 0)
					hit = false;
				if (hit) {
					ctx->a[4] = 0x032598;
					set_w(&ctx->d[5], cc_add(ctx, ax, bus_r16(ctx->a[4] + hi), 16));
					if (cmpw(ctx, ctx->d[5], ctx->d[6]) < 0)
						hit = false;
				}
				if (hit) {
					ctx->a[4] = 0x0324C0;
					set_w(&ctx->d[5], cc_sub(ctx, ay, bus_r16(ctx->a[4] + hi), 16, true));
					set_w(&ctx->d[6], cc_add(ctx, RAM16(CAMERA_Y), RAM16(POINTER_Y), 16));
					if (cmpw(ctx, ctx->d[5], ctx->d[6]) > 0)
						hit = false;
				}
				if (hit) {
					ctx->a[4] = 0x032508;
					set_w(&ctx->d[5], cc_add(ctx, ay, bus_r16(ctx->a[4] + hi), 16));
					if (cmpw(ctx, ctx->d[5], ctx->d[6]) < 0)
						hit = false;
				}
				set_w(&ctx->d[2], saved_d2);                      /* move.w (sp)+,d2 */
				cc_logic16(ctx, ctx->d[2]);
			} else if (!follows_actor) {
				hit = hotspot_hit(ctx);
			}
		}
		if (hit) {
			set_w(&ctx->d[7], ctx->d[2]);
			cc_logic16(ctx, ctx->d[7]);
			set_w(&ctx->d[7], cc_add(ctx, ctx->d[7], 3, 16));
		}
		ctx->a[0] += OBJ_REC_SIZE;
		set_w(&ctx->d[2], cc_add(ctx, ctx->d[2], 1, 16));
	} while (cmpw(ctx, ctx->d[2], RAM16(OBJ_LAST)) <= 0);
}

/* Registers of the object loop: a0 = objects, d2 = 0, d3 = room, a3 = the mystery's
 * hot-spot offset table (header +$24), a1 = its rectangle base (header +$1C), d7 = 0. */
static void object_loop_setup(hle_ctx *ctx)
{
	ctx->a[0] = OBJECTS;
	ctx->d[2] = 0;
	cc_logic32(ctx, 0);
	set_w(&ctx->d[3], RAM16(ROOM_INDEX));
	cc_logic16(ctx, ctx->d[3]);
	ctx->a[1] = RAM32(MYSTERY_HDR);
	ctx->a[3] = bus_r32(ctx->a[1] + 0x24);
	ctx->a[1] = bus_r32(ctx->a[1] + 0x1C);
	ctx->d[7] = 0;
	cc_logic32(ctx, 0);
}

/* Store the found target (in register d<reg>): normally as the hovered target; while a
 * two-object verb waits for its second target, as the second target (0 if it is the
 * first target again). */
static void set_target(hle_ctx *ctx, int reg)
{
	cc_btst(ctx, RAM8(VERB_STATE), 0);
	if (!(RAM8(VERB_STATE) & 1)) {
		RAM16(HOVER_TARGET) = (uint16_t)ctx->d[reg];
		cc_logic16(ctx, ctx->d[reg]);
		return;
	}
	if (cmpw(ctx, ctx->d[reg], RAM16(HOVER_TARGET)) == 0) {
		ctx->d[reg] = 0;
		cc_logic32(ctx, 0);
	}
	RAM16(SECOND_TARGET) = (uint16_t)ctx->d[reg];
	cc_logic16(ctx, ctx->d[reg]);
}

/* count the words of INVENTORY before the negative terminator (a2 ends after it) */
static void count_inventory(hle_ctx *ctx, int reg)
{
	ctx->a[2] = INVENTORY;
	for (;;) {
		uint16_t v = RAM16(ctx->a[2]);
		ctx->a[2] += 2;
		cc_logic16(ctx, v);
		if (v & 0x8000)
			break;
		set_w(&ctx->d[reg], cc_add(ctx, ctx->d[reg], 1, 16));
	}
}

/* $001092: the pointer is on the verb bar (screen y >= 168). VERB_SLOT is the slot under
 * it (set elsewhere). Verb mode: B selects the slot's verb. Inventory mode: slots 5 and 10
 * scroll the inventory (blocking redraws -> exits), the others hover inventory items. */
static void verb_bar(hle_ctx *ctx)
{
	set_w(&ctx->d[1], RAM16(VERB_SLOT));
	cc_logic16(ctx, ctx->d[1]);
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 4);
	if (!(RAM8(VBLANK_FLAGS) & VBF_INVENTORY)) {
		cc_logic16(ctx, ctx->d[1]);
		if (!(ctx->d[1] & 0xFFFF)) {
			update_verb_highlight(ctx);
			return;
		}
		cc_btst(ctx, RAM8(PAD1_CUR), 4);
		if (RAM8(PAD1_CUR) & 0x10) {                              /* B not pressed */
			clear_slot_then_wait(ctx);
			return;
		}
		ctx->a[0] = 0x02FE3A;                                     /* slot -> verb id table */
		set_w(&ctx->d[1], lslw(ctx, ctx->d[1], 1));
		set_w(&ctx->d[3], ROM16(0x02FE3A + (int16_t)ctx->d[1]));
		cc_logic16(ctx, ctx->d[3]);
		cc_logic16(ctx, RAM16(CUR_VERB));
		if (!RAM16(CUR_VERB)) {
			/* no verb selected yet: drop the default-verb highlight */
			RAM16(VERB_TO_DRAW) = RAM16(DEFAULT_VERB);
			cc_logic16(ctx, RAM16(VERB_TO_DRAW));
			ctx->d[0] = 0;
			cc_logic32(ctx, 0);
			hle_call68k(ctx, 0x005BDC);                           /* draw_verb_button */
		}
		if (cmpw(ctx, ctx->d[3], RAM16(CUR_VERB)) == 0) {
			clear_slot_then_wait(ctx);
			return;
		}
		ctx->d[0] = 0;
		cc_logic32(ctx, 0);
		RAM16(VERB_TO_DRAW) = RAM16(CUR_VERB);                    /* old verb back to normal */
		cc_logic16(ctx, RAM16(VERB_TO_DRAW));
		hle_call68k(ctx, 0x005BDC);
		RAM16(CUR_VERB) = (uint16_t)ctx->d[3];
		cc_logic16(ctx, ctx->d[3]);
		cc_btst(ctx, RAM8(VERB_STATE), 0);
		RAM8(VERB_STATE) &= (uint8_t)~1;
		RAM16(VERB_TO_DRAW) = (uint16_t)ctx->d[3];
		cc_logic16(ctx, ctx->d[3]);
		ctx->d[0] = 1;
		cc_logic32(ctx, 1);
		hle_call68k(ctx, 0x005BDC);                               /* new verb selected */
		highlight_default_verb(ctx);
		return;
	}

	ctx->a[2] = OBJECTS;
	uint16_t slot = (uint16_t)ctx->d[1];
	bool arrow = cmpw(ctx, slot, 5) == 0;
	if (!arrow)
		arrow = cmpw(ctx, slot, 10) == 0;
	if (arrow) {
		/* scroll arrow: hovering it clears the target; B scrolls one row */
		cc_btst(ctx, RAM8(VERB_STATE), 0);
		uint32_t t = (RAM8(VERB_STATE) & 1) ? SECOND_TARGET : HOVER_TARGET;
		RAM16(t) = 0;
		cc_logic16(ctx, 0);
		cc_btst(ctx, RAM8(PAD1_CUR), 4);
		if (RAM8(PAD1_CUR) & 0x10) {
			clear_slot_then_wait(ctx);
			return;
		}
		if (cmpw(ctx, slot, 10) != 0) {
			/* slot 5: scroll back */
			cc_logic16(ctx, RAM16(INV_PAGE));
			if (!RAM16(INV_PAGE)) {
				mainloop_exit(ctx, 0x001174);                     /* bsr sub_005B92 */
				return;
			}
			RAM16(INV_PAGE) = (uint16_t)cc_sub(ctx, RAM16(INV_PAGE), 1, 16, true);
			ctx->d[0] = 1;
			cc_logic32(ctx, 1);
			mainloop_exit(ctx, 0x00116C);                         /* bsr sub_005C5A */
			return;
		}
		/* slot 10: scroll forward if there are more rows */
		ctx->d[7] = 0;
		cc_logic32(ctx, 0);
		count_inventory(ctx, 7);
		cc_logic16(ctx, ctx->d[7]);
		if (ctx->d[7] & 0xFFFF) {
			set_w(&ctx->d[7], cc_sub(ctx, ctx->d[7], 1, 16, true));
			set_w(&ctx->d[7], lsrw(ctx, ctx->d[7], 2));
			if (cmpw(ctx, ctx->d[7], RAM16(INV_PAGE)) != 0) {
				RAM16(INV_PAGE) = (uint16_t)cc_add(ctx, RAM16(INV_PAGE), 1, 16);
				ctx->d[0] = 1;
				cc_logic32(ctx, 1);
				mainloop_exit(ctx, 0x0011A8);                     /* bsr sub_005C74 */
				return;
			}
		}
		mainloop_exit(ctx, 0x0011B0);                             /* bsr sub_005B92 */
		return;
	}

	/* $0011B8: inventory item under the pointer: slots 1-4 and 6-9 show the items
	 * INV_PAGE*4 + 1..8; target = item + 3, 0 past the end of the list */
	if (cmpw(ctx, ctx->d[1], 5) >= 0)
		set_w(&ctx->d[1], cc_sub(ctx, ctx->d[1], 1, 16, true));
	set_w(&ctx->d[0], RAM16(INV_PAGE));
	cc_logic16(ctx, ctx->d[0]);
	set_w(&ctx->d[0], lslw(ctx, ctx->d[0], 2));
	set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], ctx->d[1], 16));
	ctx->d[1] = 0;
	cc_logic32(ctx, 0);
	count_inventory(ctx, 1);
	if (cmpw(ctx, ctx->d[1], ctx->d[0]) < 0) {
		ctx->d[1] = 0;
		cc_logic32(ctx, 0);
	} else {
		ctx->a[2] = INVENTORY;
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], ctx->d[0], 16));
		set_w(&ctx->d[0], cc_sub(ctx, ctx->d[0], 2, 16, true));
		set_w(&ctx->d[1], RAM16(INVENTORY + (int16_t)ctx->d[0]));
		cc_logic16(ctx, ctx->d[1]);
		set_w(&ctx->d[1], cc_add(ctx, ctx->d[1], 3, 16));
	}
	set_target(ctx, 1);
	update_verb_highlight(ctx);
}

/* $000E94: what is under the pointer (pointer mode) or under Shaggy (walk mode). */
static void pointer_or_walk(hle_ctx *ctx)
{
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 3);
	if (RAM8(VBLANK_FLAGS) & VBF_POINTER) {
		set_w(&ctx->d[0], RAM16(POINTER_X));
		cc_logic16(ctx, ctx->d[0]);
		set_w(&ctx->d[1], RAM16(POINTER_Y));
		cc_logic16(ctx, ctx->d[1]);
		if (cmpw(ctx, ctx->d[1], 0xA8) >= 0) {
			verb_bar(ctx);
			return;
		}
		set_w(&ctx->d[1], cc_sub(ctx, ctx->d[1], 8, 16, true));
		if ((int16_t)ctx->d[1] < 0) {                             /* top 8 lines: nothing */
			update_verb_highlight(ctx);
			return;
		}
		/* pointer hot spot (8,8) -> world cell */
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 8, 16));
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], RAM16(CAMERA_X), 16));
		set_w(&ctx->d[1], cc_add(ctx, ctx->d[1], RAM16(CAMERA_Y), 16));
		set_w(&ctx->d[0], lsrw(ctx, ctx->d[0], 3));
		set_w(&ctx->d[1], lsrw(ctx, ctx->d[1], 3));
		object_loop_setup(ctx);
		/* Scooby (actor 1, present and not hidden): box around his position */
		cc_btst(ctx, RAM8(0xFF0ABF), 1);
		if (!(RAM8(0xFF0ABF) & 2)) {
			cc_btst(ctx, RAM8(ACTORS_ON), 1);
			if (RAM8(ACTORS_ON) & 2) {
				uint16_t sx = RAM16(ACTOR_X + 4), sy = RAM16(ACTOR_Y + 4);
				set_w(&ctx->d[4], cc_add(ctx, RAM16(POINTER_X), RAM16(CAMERA_X), 16));
				set_w(&ctx->d[5], cc_add(ctx, sx, ROM16(0x03259A), 16));
				bool in = cmpw(ctx, ctx->d[4], ctx->d[5]) <= 0;
				if (in) {
					set_w(&ctx->d[5], cc_sub(ctx, sx, ROM16(0x032552), 16, true));
					in = cmpw(ctx, ctx->d[4], ctx->d[5]) >= 0;
				}
				if (in) {
					set_w(&ctx->d[4], cc_add(ctx, RAM16(POINTER_Y), RAM16(CAMERA_Y), 16));
					set_w(&ctx->d[5], cc_add(ctx, sy, ROM16(0x03250A), 16));
					in = cmpw(ctx, ctx->d[4], ctx->d[5]) <= 0;
				}
				if (in) {
					set_w(&ctx->d[5], cc_sub(ctx, sy, ROM16(0x0324C2), 16, true));
					in = cmpw(ctx, ctx->d[4], ctx->d[5]) >= 0;
				}
				if (in) {
					cc_btst(ctx, RAM8(VERB_STATE), 0);
					set_w(&ctx->d[7], 2);
					cc_logic16(ctx, 2);
				}
			}
		}
		find_target_object(ctx, true);
		set_target(ctx, 7);
		update_verb_highlight(ctx);
		return;
	}

	/* $0012CC walk mode: the object zone Shaggy stands in (by his cell) */
	set_w(&ctx->d[0], RAM16(ACTOR_X));
	cc_logic16(ctx, ctx->d[0]);
	set_w(&ctx->d[1], RAM16(ACTOR_Y));
	cc_logic16(ctx, ctx->d[1]);
	set_w(&ctx->d[0], lsrw(ctx, ctx->d[0], 3));
	set_w(&ctx->d[1], lsrw(ctx, ctx->d[1], 3));
	object_loop_setup(ctx);
	find_target_object(ctx, false);
	RAM16(HOVER_TARGET) = (uint16_t)ctx->d[7];
	cc_logic16(ctx, ctx->d[7]);
	if (!(ctx->d[7] & 0xFFFF)) {
		mainloop_exit(ctx, 0x0013B0);
		return;
	}
	if (cmpw(ctx, ctx->d[7], RAM16(HOVER_PREV)) != 0) {
		cc_btst(ctx, RAM8(SCRIPT_FLAGS), 7);
		RAM8(SCRIPT_FLAGS) &= 0x7F;                               /* new zone: re-arm */
	}
	cc_btst(ctx, RAM8(SCRIPT_FLAGS), 7);
	if (RAM8(SCRIPT_FLAGS) & 0x80) {                              /* zone script already ran */
		mainloop_exit(ctx, 0x0013B0);
		return;
	}
	RAM16(ZONE_TRIGGER) = 11;
	cc_logic16(ctx, 11);
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 7);
	RAM8(VBLANK_FLAGS) |= VBF_SCRIPT;
	mainloop_exit(ctx, 0x001392);                                 /* bsr sub_0019B0: zone script */
}

/* $000D3E main_loop_pass: one pass of the main loop after the object event scripts. */
HLE_RANGE(0x000D3E, main_loop_pass, 1500,
          0x000D4A, 0x000D88, 0x000E86, 0x00116C, 0x001174, 0x0011A8, 0x0011B0, 0x001228, 0x001392, 0x0013B0)
{
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 7);
	if (RAM8(VBLANK_FLAGS) & VBF_SCRIPT) {
		mainloop_exit(ctx, 0x000D4A);                             /* bsr sub_0019B0: run it */
		return;
	}
	cc_btst(ctx, RAM8(PAD1_CUR), 7);
	if (!(RAM8(PAD1_CUR) & 0x80)) {
		cc_btst(ctx, RAM8(0xFF09E9), 4);
		if (!(RAM8(0xFF09E9) & 0x10)) {
			/* Start: pause. Stop the VBlank game work and go to the title menu */
			cc_btst(ctx, RAM8(VBLANK_FLAGS), 6);
			RAM8(VBLANK_FLAGS) &= (uint8_t)~VBF_FRAME_WORK;
			mainloop_exit(ctx, 0x000D88);                         /* jsr palette_read_cram */
			return;
		}
	}

	hle_call68k(ctx, 0x00A828);                                   /* status line text */
	/* remember this pass's state for change detection in the next one */
	RAM16(HOVER_PREV) = RAM16(HOVER_TARGET);
	RAM8(VBLANK_FLAGS_PREV) = RAM8(VBLANK_FLAGS);
	RAM8(VERB_STATE_PREV) = RAM8(VERB_STATE);
	RAM32_W(ROOM_NAME_PREV, RAM32(ROOM_NAME_PTR));
	RAM16(HOVER_PREV) = RAM16(HOVER_TARGET);
	RAM16(CUR_VERB_PREV) = RAM16(CUR_VERB);
	RAM16(SECOND_PREV) = RAM16(SECOND_TARGET);
	cc_logic16(ctx, RAM16(SECOND_PREV));

	/* $000DF4 ambient events: when AMBIENT_TIMER has run out, pick a random entry of the
	 * room's event table (ram_room_ptr_69a, AMBIENT_COUNT entries of 2 words) */
	cc_btst(ctx, RAM8(VERB_STATE), 7);
	bool roll = !(RAM8(VERB_STATE) & 0x80);
	if (roll) { cc_btst(ctx, RAM8(0xFF0AB7), 1); roll = !(RAM8(0xFF0AB7) & 2); }
	if (roll) { cc_btst(ctx, RAM8(0xFF09EB), 2); roll = !(RAM8(0xFF09EB) & 4); }
	if (roll) { cc_logic16(ctx, RAM16(AMBIENT_TIMER)); roll = RAM16(AMBIENT_TIMER) & 0x8000; }
	if (roll) { cc_btst(ctx, RAM8(0xFF0ABC), 1); roll = RAM8(0xFF0ABC) & 2; }
	if (roll) {
		RAM16(AMBIENT_TIMER) = 100;
		cc_logic16(ctx, 100);
		set_w(&ctx->d[0], RAM16(AMBIENT_COUNT));
		cc_logic16(ctx, ctx->d[0]);
		if (ctx->d[0] & 0xFFFF) {
			hle_call68k(ctx, 0x0022D6);                           /* rng_next: 0..count-1 */
			set_w(&ctx->d[0], lslw(ctx, ctx->d[0], 2));
			ctx->a[0] = RAM32(0xFF069A) + (int16_t)ctx->d[0];
			/* entry word 0: kkpp xxxx xxxx xxxx (k: index into the 4 words at $000E8C,
			 * p: AMBIENT_KIND, x: cell x), word 1: cell y */
			uint16_t w0 = bus_r16(ctx->a[0]);
			uint16_t r = (uint16_t)((w0 << 4) | (w0 >> 12));      /* rol.w #4 (X kept) */
			cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, (r & 0x8000 ? CC_N : 0) | (r ? 0 : CC_Z) | (r & 1 ? CC_C : 0));
			set_w(&ctx->d[0], r);
			set_w(&ctx->d[1], r & 3);
			cc_logic16(ctx, ctx->d[1]);
			set_w(&ctx->d[0], r & 0xC);
			cc_logic16(ctx, ctx->d[0]);
			set_w(&ctx->d[0], lsrw(ctx, ctx->d[0], 2));
			set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], ctx->d[0], 16));
			set_w(&ctx->d[0], ROM16(0x000E8C + (int16_t)ctx->d[0]));
			RAM16(AMBIENT_KIND) = (uint16_t)ctx->d[1];
			RAM16(AMBIENT_ARG) = (uint16_t)ctx->d[0];
			set_w(&ctx->d[0], w0 & 0xFFF);
			set_w(&ctx->d[1], bus_r16(ctx->a[0] + 2));
			set_w(&ctx->d[0], lslw(ctx, ctx->d[0], 3));                /* cells -> pixels */
			set_w(&ctx->d[1], lslw(ctx, ctx->d[1], 3));
			set_w(&ctx->d[7], 0);
			ctx->d[2] = 1;
			cc_logic32(ctx, 1);
			mainloop_exit(ctx, 0x000E86);                         /* bsr sub_0056B0: spawn */
			return;
		}
	}
	pointer_or_walk(ctx);
}

/* $000E8A: back from the ambient event spawn (bra $E94) */
HLE_RANGE(0x000E8A, main_loop_after_spawn, 1200,
          0x00116C, 0x001174, 0x0011A8, 0x0011B0, 0x001228, 0x001392, 0x0013B0)
{
	pointer_or_walk(ctx);
}

/* $00122E: back from wait_vblank after a verb-bar click */
HLE_RANGE(0x00122E, main_loop_after_click, 300, 0x0013B0)
{
	update_verb_highlight(ctx);
}

/* $001178 / $0011B4: back from the inventory redraw (bra $1222) */
HLE_RANGE(0x001178, main_loop_after_inv_back, 40, 0x001228)
{
	clear_slot_then_wait(ctx);
}

HLE_RANGE(0x0011B4, main_loop_after_inv_fwd, 40, 0x001228)
{
	clear_slot_then_wait(ctx);
}

/* $000D4E: after the pending room script: clr.w (SECOND_TARGET), jsr wait_vblank */
HLE_RANGE(0x000D4E, main_loop_after_script, 30, 0x000D54)
{
	RAM16(SECOND_TARGET) = 0;
	cc_logic16(ctx, 0);
	mainloop_exit(ctx, 0x000D54);
}

/* $000D5A: bclr #7,(VBLANK_FLAGS), jsr wait_vblank (then bra $D38) */
HLE_RANGE(0x000D5A, main_loop_script_done, 30, 0x000D62)
{
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 7);
	RAM8(VBLANK_FLAGS) &= (uint8_t)~VBF_SCRIPT;
	mainloop_exit(ctx, 0x000D62);
}

/* $000D96: back from the pause menu (sub_008196) and room reload (sub_002196) */
HLE_RANGE(0x000D96, main_loop_after_pause, 30, 0x000D9E)
{
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 7);
	RAM8(VBLANK_FLAGS) &= (uint8_t)~VBF_SCRIPT;
	mainloop_exit(ctx, 0x000D9E);
}

/* $001396: back from a zone script: mark it as run for this zone, then wait_vblank */
HLE_RANGE(0x001396, main_loop_after_zone_script, 30, 0x0013A6)
{
	cc_btst(ctx, RAM8(SCRIPT_FLAGS), 7);
	RAM8(SCRIPT_FLAGS) |= 0x80;
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 7);
	RAM8(VBLANK_FLAGS) &= (uint8_t)~VBF_SCRIPT;
	mainloop_exit(ctx, 0x0013A6);
}
