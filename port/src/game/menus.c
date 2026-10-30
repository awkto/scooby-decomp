/* Title screen, main menu, pause menu, mystery choice and the cursor loop (GitLab #21).
 * docs/subsystems/menus.md has the map; menu_password.c, menu_debug.c and soundtest.c hold the
 * password screens, the hidden DEBUG editors and the sound test.
 *
 * Almost all of this code waits for VBlank (frame pacing through `wait_vblank` and a spin on
 * the frame countdown $FF08AE), so it is ported as HLE_RANGEs that end in front of each wait
 * (docs/porting.md "Waiting for VBlank").  The ranges stop at the original's wait instruction,
 * the 68k executes it, and the code after it is the next range.  Pushes and pops of the 68k stack
 * that straddle a wait (the menu table pointer and item count pushed by $00857A, popped by
 * $0085CC...) stay 68k: a range must leave with the SP it was entered with.
 *
 * Registers: the proofs for what difftest ignores are in the comment of each range ("live out");
 * the table is in port/tools/difftest.c ("#21 menus").
 */
#include "menus.h"

/* ROM data used by the title screen */
#define ROM_TITLE_TILES      0x01155A   /* LZSS: tile art, copied to VRAM $0000 */
#define ROM_TITLE_ART1       0x016D38   /* RLE, 4 tiles each ... */
#define ROM_TITLE_ART2       0x016E6A
#define ROM_TITLE_ART3       0x016F76
#define ROM_TITLE_PLANE_B    0x0165B8   /* 0x380 words of plane B name table ($E000) */
#define ROM_TITLE_RLE_TABLE  0x010136   /* streams for $FF3800 / $FF5000 */
#define ROM_TITLE_PALETTE    0x016CB8   /* 32 longs -> ram_pal_target */
#define ROM_TITLE_PALETTE2   0x032482   /* 15 longs -> ram_pal_target + 2 (text colours) */
#define ROM_BATS_FRAMES      0x017064   /* 28 pointers to RLE frames of the bat animation */
#define RAM_PAL_TARGET       0xFF0778
#define RAM_BUF_3800         0xFF3800
#define RAM_BUF_5000         0xFF5000
#define BATS_FRAMES          28

#define VBLANK_TITLE_FADE 0x00A38C
#define HBLANK_NOP        0x01006A

static void vdp_copy(hle_ctx *ctx, uint16_t vram, uint16_t words, uint32_t src)
{
	set_w(&ctx->d[0], vram);
	set_w(&ctx->d[1], words);
	ctx->a[0] = src;
	hle_call68k(ctx, A_VDP_COPY);
}

/* rle_decompress(a0 = stream, a2 = output) */
static void rle(hle_ctx *ctx, uint32_t stream, uint32_t out)
{
	ctx->a[0] = stream;
	ctx->a[2] = out;
	rle_call(ctx);
}

/* ================================================================== title screen */

/* $00819A: [68k: bsr palette_read_cram at $008196, which waits for VBlank]  Mask interrupts for
 * the set-up.  Split off so that the long set-up below is entered masked (the layer charges its
 * cost at once, as the original lost those VBlanks), not unmasked (sliced). */
HLE_RANGE(0x00819A, title_mask_irq, 16, 0x00819E)
{
	ctx->sr = 0x2700;
	hle_jump(ctx, 0x00819E);
}

/* $00819E title_setup: build the title screen: tiles, plane B, palette, the bat sprites; show
 * the encoded current password; point the interrupt hooks at the title fade.  Ends in front of
 * `jsr palette_fade` ($0083E2, a0 = the palette; it waits for VBlank).
 * Live out: a0 only (palette_fade saves everything else; the next range loads its own). */
#define TITLE_FADE_IN 0x0083E2
HLE_RANGE(0x00819E, title_setup, 58000, TITLE_FADE_IN)
{
	RAM8(MENU_FLAGS) &= (uint8_t)~(0x04 | MF_BATS_DONE);
	RAM8(SCREEN_FLAGS) &= (uint8_t)~0x80;
	vdp_ctrl_w(0x9100);                 /* window plane off */
	vdp_ctrl_w(0x9200);
	RAM16(TITLE_FADE_DELAY) = 0;        /* title lightning/fade state */
	RAM16(TITLE_SCROLL_DIR) = 0;        /* clears TITLE_SCROLL_DIR and TITLE_SCROLL together */
	RAM16(TITLE_FADE_TICK) = 0;
	RAM16(MENU_ROOM) = RAM16(ROOM_INDEX);
	RAM16(MENU_BAT_DEPTH_A) = 0xE0;     /* scale depths of the two bat sprite groups: shrink to 0 */
	RAM16(MENU_BAT_DEPTH_B) = 0xE0;
	set_w(&ctx->d[0], 0xDC00);          /* sprite table row at VRAM $DC00 cleared (two words) */
	set_w(&ctx->d[1], 0);
	hle_call68k(ctx, A_VDP_WRITE_WORD);
	set_w(&ctx->d[0], ctx->d[0] + 2);
	hle_call68k(ctx, A_VDP_WRITE_WORD);
	vdp_ctrl_w32(0x40000010);           /* VSRAM 0: vertical scroll = 0 */
	bus_w32(VDP_DATA, 0);
	RAM16(TITLE_STRIP_INDEX) = 0;
	RAM16(TITLE_STRIP_TIMER) = 3;
	RAM8(SCREEN_FLAGS) &= (uint8_t)~0x08;
	RAM8(SCREEN_FLAGS) |= 0x10;
	set_w(&ctx->d[0], 0xDC00);
	set_w(&ctx->d[1], 0x1C0);
	hle_call68k(ctx, A_VDP_CLEAR);
	set_w(&ctx->d[1], 0x8000);          /* all of VRAM */
	ctx->d[0] = 0;
	hle_call68k(ctx, A_VDP_CLEAR);
	RAM8(SCREEN_FLAGS) |= 0x02;
	vdp_ctrl_w32(0x7C000003);
	vdp_ctrl_w(0x857E);
	bus_w32(VDP_DATA, 0);
	RAM32_W(VBLANK_HOOK, VBLANK_TITLE_FADE);
	RAM32_W(HBLANK_HOOK, HBLANK_NOP);
	vdp_ctrl_w(0x9100);
	vdp_ctrl_w(0x9200);
	vdp_ctrl_w(0x9000);

	/* tile art: LZSS $01155A -> VRAM $0000; the size left in the ring buffer says where the
	 * next art goes (d1 = byte size, d2 = first free tile number) */
	ctx->a[0] = ROM_TITLE_TILES;
	ctx->a[1] = LZSS_RING;
	ctx->a[2] = DECOMP_BUF;
	lzss_call(ctx);
	ctx->d[0] = 0;
	ctx->d[1] = bus_r32(ctx->a[1]);
	set_w(&ctx->d[1], (uint16_t)ctx->d[1] >> 1);
	ctx->a[0] = ctx->a[2];
	hle_call68k(ctx, A_VDP_COPY);
	uint16_t words = (uint16_t)ctx->d[1];
	uint16_t vram = (uint16_t)(words * 2);                   /* VRAM byte address after the art */
	RAM16(MENU_CURSOR_TILE) = words >> 4;                    /* its first free tile number */
	RAM16(FONT_BASE) = 0x680;
	/* three 4-tile pieces (bat/lightning art) after it */
	rle(ctx, ROM_TITLE_ART1, LZSS_RING);
	vdp_copy(ctx, vram, 0x100, LZSS_RING);
	rle(ctx, ROM_TITLE_ART2, LZSS_RING);
	vdp_copy(ctx, (uint16_t)(vram + 0x200), 0x100, LZSS_RING);
	rle(ctx, ROM_TITLE_ART3, LZSS_RING);
	vdp_copy(ctx, (uint16_t)(vram + 0x400), 0x100, LZSS_RING);
	/* the 8x8 font: font_tiles_load(d0 = $F colour, d1 = 0, d2 = font base) */
	set_w(&ctx->d[0], 0xF);
	set_w(&ctx->d[1], 0);
	set_w(&ctx->d[2], RAM16(FONT_BASE));
	hle_call68k(ctx, 0x007A2A);
	set_w(&ctx->d[0], 0xFA00);          /* a blank tile row ($AAAA = colour 10) */
	set_w(&ctx->d[1], 0x40);
	set_w(&ctx->d[2], 0xAAAA);
	hle_call68k(ctx, A_VDP_FILL);
	vdp_copy(ctx, 0xE000, 0x380, ROM_TITLE_PLANE_B);
	/* two streams from the table at $010136 go to $FF3800 / $FF5000 (title plane A pieces) */
	uint32_t tbl = ROM_TITLE_RLE_TABLE;
	uint16_t base = ROM16(tbl + 4);
	uint16_t d1 = ROM16(tbl + base);
	rle(ctx, tbl + ROM32(tbl + 6 + d1), RAM_BUF_3800);
	d1 = ROM16(tbl + 2 + base);
	rle(ctx, tbl + ROM32(tbl + 6 + d1), RAM_BUF_5000);
	RAM32_W(LZSS_RING + 2044, 0);
	RAM32_W(RAM_BUF_5000 + 2048, 0);
	vdp_copy(ctx, 0xFC00, 0x34, ROM_TITLE_SAT);
	for (int i = 0; i < 32; i++)        /* palette: 32 longs, then 15 longs over the text colours */
		RAM32_W(RAM_PAL_TARGET + 4 * i, ROM32(ROM_TITLE_PALETTE + 4 * i));
	for (int i = 0; i < 15; i++)
		RAM32_W(RAM_PAL_TARGET + 2 + 4 * i, ROM32(ROM_TITLE_PALETTE2 + 4 * i));
	ctx->sr = 0x2000;                   /* interrupts on from here (the fade needs VBlank) */
	password_show(ctx);                 /* the current password goes to $FF08BC (the pre-fill) */
	ctx->a[0] = RAM_PAL_TARGET;
	hle_jump(ctx, TITLE_FADE_IN);
}

/* $0083E8 title_bats_start: the palette is faded in; arm the bat animation: 28 frames from
 * $017064 (a6 = next frame pointer, d6 = frames left - 1), scale depth $E0 (full shrink).
 * Ends in front of `bsr wait_vblank` ($00840A).  Live out: d6, a6. */
#define BATS_WAIT_A 0x00840A
HLE_RANGE(0x0083E8, title_bats_start, 84, BATS_WAIT_A)
{
	RAM8(SCREEN_FLAGS) |= 0x80;
	RAM16(0xFF0624) = 0xE0;             /* ram_scale_depth_x / _y */
	RAM16(0xFF0626) = 0xE0;
	ctx->a[6] = ROM_BATS_FRAMES;
	set_w(&ctx->d[6], BATS_FRAMES - 1);
	hle_jump(ctx, BATS_WAIT_A);
}

#define BATS_FRAME 0x008416
#define BATS_WAIT  0x0084AA

/* $00840E: after the first VBlank: pace the animation at 5 frames per picture */
HLE_RANGE(0x00840E, title_bats_first, 20, BATS_FRAME)
{
	RAM16(FRAME_COUNTDOWN) = 4;
	hle_jump(ctx, BATS_FRAME);
}

/* $008416 title_bats_frame: one picture of the bat animation.  The bats fly in as scaled
 * sprites: the frame ($017064 list) is unpacked to the ring buffer, then drawn through the
 * sprite subsystem ($E40C, $F272) at the current scale depth; the depth ($FF0010, then
 * $FF0012) shrinks by $20 per picture until it reaches 0.  Sound $0B (the flap) on pictures
 * 27 and 13 (counting d6 down).  Ends in front of `bsr wait_vblank` ($0084AA).
 * Live out: d6, a6 (d0-d5, a0-a5: the movem pair around the scaler restores them, the rest is
 * scratch that the polling code at $0084AE overwrites). */
HLE_RANGE(0x008416, title_bats_frame, 21000, BATS_WAIT)
{
	uint16_t d6 = (uint16_t)ctx->d[6];
	if (d6 == 27 || d6 == 13)
		menu_sound(ctx, 0x0B);
	ctx->a[0] = ROM32(ctx->a[6]);
	ctx->a[6] += 4;
	rle(ctx, ctx->a[0], LZSS_RING);
	hle_ctx save = *ctx;                /* movem.l d0-d7/a0-a6,-(sp) */
	if (!(RAM8(MENU_FLAGS) & MF_BATS_DONE)) {
		RAM16(0xFF0624) = RAM16(MENU_BAT_DEPTH_A);
		RAM16(0xFF0626) = RAM16(MENU_BAT_DEPTH_A);
		if (RAM16(MENU_BAT_DEPTH_A))
			RAM16(MENU_BAT_DEPTH_A) -= 0x20;
	}
	hle_call68k(ctx, A_SPRITES_E40C);
	if (RAM16(MENU_BAT_DEPTH_A) == 0) {
		if (!(RAM8(MENU_FLAGS) & MF_BATS_DONE)) {
			RAM16(0xFF0624) = RAM16(MENU_BAT_DEPTH_B);
			RAM16(0xFF0626) = RAM16(MENU_BAT_DEPTH_B);
			if (RAM16(MENU_BAT_DEPTH_B) == 0)
				RAM8(MENU_FLAGS) |= MF_BATS_DONE;
			else
				RAM16(MENU_BAT_DEPTH_B) -= 0x20;
		}
		hle_call68k(ctx, A_SPRITES_F272);
	}
	*ctx = save;                        /* movem.l (sp)+,d0-d7/a0-a6 */
	hle_jump(ctx, BATS_WAIT);
}

/* $0084AE: after a VBlank during the animation: any of Start/A/B/C ends it (-> menu $00851E);
 * else when the 5-frame countdown has run out, show the picture (planes and scrolled title
 * sprites to VRAM) and go for the next one; after the 28th the menu follows.
 * Live out: d6 and a6 on the loop exits ($008416, $0084AA); nothing at $00851E. */
#define MENU_OPEN 0x00851E
HLE_RANGE(0x0084AE, title_bats_wait, 300, BATS_WAIT, BATS_FRAME, MENU_OPEN)
{
	if ((RAM8(PAD1_CUR) & 0xF0) != 0xF0) {      /* S, A, C or B pressed */
		hle_jump(ctx, MENU_OPEN);
		return;
	}
	if (!(RAM16(FRAME_COUNTDOWN) & 0x8000)) {   /* tst.w countdown / bpl: not yet */
		hle_jump(ctx, BATS_WAIT);
		return;
	}
	menu_dma(ctx, 0xF000, 0x4B0, ZONE_MAP);     /* the scrolling title strips */
	if (RAM16(MENU_BAT_DEPTH_A) == 0)
		menu_dma(ctx, 0xC800, 0x200, PLANEA_NAMES);
	menu_title_sprites(ctx);
	menu_dma(ctx, 0xC000, 0x380, LZSS_RING);    /* the bat picture */
	RAM16(FRAME_COUNTDOWN) = 4;
	uint16_t d6 = (uint16_t)ctx->d[6] - 1;
	set_w(&ctx->d[6], d6);
	hle_jump(ctx, d6 != 0xFFFF ? BATS_FRAME : MENU_OPEN);
}

/* ================================================================== menu builder ($00851E) */

/* $00851E menu_open: build the menu: the title menu (PLAY BLAKE'S HOTEL / PLAY HA HA CARNIVAL /
 * SOUND TEST, 3 items) or, once a game was started (MF_GAME_STARTED), the pause menu
 * (CONTINUE / QUIT MYSTERY, 2 items; 4 with DEBUG ROOMS / OBJECTS after the cheat code).
 * Picks the item's screen (RLE) and the action table; [68k from $00857A: push table and count,
 * unpack the screen, spin on the frame countdown].
 * Live out: a0 = screen stream, a1 = action table, d0 = item count. */
#define MENU_PUSH 0x00857A
#define ROM_SCREEN_PAUSE_DEBUG 0x018CBE
#define ROM_SCREEN_PAUSE       0x018BFA
#define ROM_SCREEN_TITLE_MENU  0x01896C
HLE_RANGE(0x00851E, menu_open, 400, MENU_PUSH)
{
	RAM16(0xFF0624) = 0;
	RAM16(0xFF0626) = 0;
	RAM8(MENU_FLAGS) |= MF_BATS_DONE;
	hle_call68k(ctx, A_SPRITES_E40C);
	hle_call68k(ctx, A_SPRITES_F272);
	if (RAM8(MENU_FLAGS) & MF_GAME_STARTED) {
		if (RAM8(MENU_FLAGS) & MF_DEBUG) {
			ctx->a[0] = ROM_SCREEN_PAUSE_DEBUG;
			ctx->d[0] = 4;
		} else {
			ctx->a[0] = ROM_SCREEN_PAUSE;
			ctx->d[0] = 2;
		}
		ctx->a[1] = ROM_MENU_SCREENS_P;
	} else {
		ctx->a[0] = ROM_SCREEN_TITLE_MENU;
		ctx->a[1] = ROM_MENU_SCREENS;
		ctx->d[0] = 3;
	}
	hle_jump(ctx, MENU_PUSH);
}

/* $008590 menu_show: the frame countdown ran out: upload the planes and the menu screen;
 * in the pause menu draw the current password (2 text rows) above the items.
 * Ends in front of `move.w (sp)+,d0` ($0085CC: [68k pops the item count, bsr menu_select, then
 * the dispatch through the action table: lsl.w #2,d0 / movea.l (sp)+,a0 / jmp ([a0+d0])]).
 * Live out: nothing. */
#define MENU_POP 0x0085CC
HLE_RANGE(0x008590, menu_show, 300, MENU_POP)
{
	RAM16(FRAME_COUNTDOWN) = 4;
	menu_refresh_planes(ctx);
	menu_dma(ctx, 0xC000, 0x380, LZSS_RING);
	if (RAM8(MENU_FLAGS) & MF_GAME_STARTED) {
		ctx->a[0] = TEXT_BUF;
		ctx->d[2] = 7;
		ctx->d[1] = 1;
		hle_call68k(ctx, A_TEXT_9F72);
		set_w(&ctx->d[2], ctx->d[2] + 1);
		hle_call68k(ctx, A_TEXT_9F72);
	}
	hle_jump(ctx, MENU_POP);
}

/* ================================================================== menu_choose ($008D36) */

/* $008D4A: [68k: push d0 (item count), unpack the screen $FF3000, spin until the frame
 * countdown runs out]  Show the screen (planes + name table), then leave in front of the
 * item-count pop ($008D68) so that menu_select ($008D6A) follows with d0 = count.
 * Live out: nothing. */
#define MENU_CHOOSE_POP 0x008D68
HLE_RANGE(0x008D4A, menu_choose_show, 100, MENU_CHOOSE_POP)
{
	RAM16(FRAME_COUNTDOWN) = 4;
	menu_refresh_planes(ctx);
	menu_dma(ctx, 0xC000, 0x380, LZSS_RING);
	hle_jump(ctx, MENU_CHOOSE_POP);
}

/* ================================================================== menu_select ($008D6A) */
/* The item cursor loop shared by all menus.  d0 = item count on entry, chosen item (0-based) in
 * d0 on return.  D-pad up/down moves the cursor by $18 pixels, Start chooses.  The cursor is a
 * sprite at VRAM $FC60 that blinks through 4 tile offsets ($008F14).  Hidden: the cheat code
 * A B A C A B Left Right A B Left ($010118) typed on any of these menus sets MF_DEBUG.
 * Registers carried through the loop: d4 = blink phase (0,2,4,6 = byte offset into the table),
 * d5 = cursor y offset from the first item (multiples of $18), d6 = blink timer, d7 = largest
 * y offset ($18 * (count - 1)). */
#define MS_DRAW     0x008D96
#define MS_POLL     0x008DA6
#define MS_FRAME    0x008D8E
#define MS_WAIT     0x008E74
#define MS_CHOSEN   0x008EDC
#define MS_RTS      0x008F12

/* the cheat code matcher ($008DA6..$008E12), run once per pass while MF_DEBUG is clear.
 * Table entries are active-high masks; the pad is active low, so the expected pad value is the
 * complement.  Matching the next key advances; still holding the previous key is allowed;
 * releasing everything keeps the progress; any other press resets it. */
static void cheat_step(hle_ctx *ctx, uint8_t pad)
{
	uint16_t n = RAM16(CHEAT_PROGRESS);
	uint8_t want = (uint8_t)(ROM8(ROM_PAD_CHEAT + n) ^ 0xFF);
	if (want == pad) {
		RAM16(CHEAT_PROGRESS) = ++n;
		if (n == 11) {
			RAM8(MENU_FLAGS) |= MF_DEBUG;
			menu_sound(ctx, 3);
		}
		return;
	}
	if (n != 0 && (uint8_t)(ROM8(ROM_PAD_CHEAT + n - 1) ^ 0xFF) == pad)
		return;                                 /* still holding the last key */
	if (pad != 0xFF)
		RAM16(CHEAT_PROGRESS) = 0;
}

/* $008D6A: start: cheat progress 0, cursor on the first item, y limit from the item count */
HLE_RANGE(0x008D6A, menu_select, 148, MS_DRAW)
{
	RAM16(CHEAT_PROGRESS) = 0;
	uint16_t count = (uint16_t)ctx->d[0];
	ctx->d[4] = 0;
	ctx->d[5] = 0;
	ctx->d[6] = 1;
	uint16_t limit = 0;
	do {                                        /* subq.w #1,d0 / beq: limit = $18 * (count - 1) */
		count--;
		if (!count)
			break;
		limit += 0x18;
	} while (1);
	ctx->d[7] = limit;
	set_w(&ctx->d[0], 0);
	RAM8(MENU_PAD_PREV) = RAM8(PAD1_PREV);      /* pad 1 as of the last frame */
	hle_jump(ctx, MS_DRAW);
}

/* $008D8E: re-arm the pacing countdown, then redraw the sprites */
HLE_RANGE(0x008D8E, menu_select_frame, 20, MS_DRAW)
{
	RAM16(FRAME_COUNTDOWN) = 4;
	hle_jump(ctx, MS_DRAW);
}

/* The pad pass: cheat code, cursor moves, Start.  Returns the exit address. */
static uint32_t menu_select_poll(hle_ctx *ctx)
{
	uint8_t pad = RAM8(PAD1_CUR), prev = RAM8(MENU_PAD_PREV);
	uint16_t d5 = (uint16_t)ctx->d[5], d7 = (uint16_t)ctx->d[7];
	if (!(RAM8(MENU_FLAGS) & MF_DEBUG))
		cheat_step(ctx, pad);
	if (menu_pressed(pad, prev, PAD_U) && d5)
		d5 -= 0x18;
	if (menu_pressed(pad, prev, PAD_D) && d5 != d7)
		d5 += 0x18;
	set_w(&ctx->d[5], d5);
	if (menu_pressed(pad, prev, PAD_S))
		return MS_CHOSEN;
	RAM8(MENU_PAD_PREV) = pad;
	return MS_WAIT;
}

/* $008D96: sprites, then the pad pass (stops in front of bsr wait_vblank, or of the spin at
 * $008EDC when Start was just pressed).  Live out: d4-d7. */
HLE_RANGE(0x008D96, menu_select_draw, 400, MS_WAIT, MS_CHOSEN)
{
	menu_sprites_refresh(ctx);
	hle_jump(ctx, menu_select_poll(ctx));
}

/* $008DA6: the pad pass alone (countdown not run out yet) */
HLE_RANGE(0x008DA6, menu_select_pad, 200, MS_WAIT, MS_CHOSEN)
{
	hle_jump(ctx, menu_select_poll(ctx));
}

/* $008E78: after a VBlank.  Countdown still running -> poll again.  Run out -> refresh the
 * planes, draw the cursor sprite (VRAM $FC60: y, size/link $0F00, tile, x $A8) and step the
 * blink; then a new pass from $008D8E.  Live out: d4-d7. */
HLE_RANGE(0x008E78, menu_select_tick, 30, MS_POLL, MS_FRAME)
{
	if (!(RAM16(FRAME_COUNTDOWN) & 0x8000)) {
		hle_jump(ctx, MS_POLL);
		return;
	}
	menu_refresh_planes(ctx);
	ctx->cycles += 300;
	uint16_t d4 = (uint16_t)ctx->d[4], d5 = (uint16_t)ctx->d[5];
	uint16_t tile = (uint16_t)(RAM16(MENU_CURSOR_TILE) + ROM16(ROM_CURSOR_BLINK + (d4 & 7)));
	ctx->sr = 0x2700;
	vdp_ctrl_w32(0x7C600003);
	vdp_data_w((uint16_t)(0xD0 + d5));
	vdp_data_w(0x0F00);
	vdp_data_w(tile);
	vdp_data_w(0xA8);
	ctx->sr = 0x2000;
	int16_t d6 = (int16_t)ctx->d[6] - 1;
	if (d6 < 0) {                               /* the blink phase lasts two passes */
		d6 = 1;
		d4 = (d4 + 2) & 7;
		set_w(&ctx->d[4], d4);
	}
	set_w(&ctx->d[6], (uint16_t)d6);
	hle_jump(ctx, MS_FRAME);
}

/* $008EE6: Start was chosen and the spin at $008EDC (frame countdown) is over: refresh the
 * planes, hide the cursor sprite, return d0 = the chosen item (d5 / $18).  Live out: d0. */
HLE_RANGE(0x008EE6, menu_select_done, 140, MS_RTS)
{
	menu_refresh_planes(ctx);
	RAM16(FRAME_COUNTDOWN) = 4;
	int16_t d5 = (int16_t)ctx->d[5];
	uint32_t item = 0;
	for (;;) {
		d5 -= 0x18;
		if (d5 < 0)
			break;
		item++;
	}
	ctx->d[0] = item;
	set_w(&ctx->d[5], (uint16_t)d5);
	vdp_ctrl_w32(0x7C600003);
	bus_w32(VDP_DATA, 0);
	hle_jump(ctx, MS_RTS);
}

/* ================================================================== screens behind the menu items */

/* $008F8A quit_confirm: QUIT MYSTERY: "NO! DONT DO IT!" (item 0, cancels) / yes.  [68k: bsr
 * menu_choose at $008F90] */
HLE_RANGE(0x008F8A, quit_confirm, 12, 0x008F90)
{
	ctx->a[0] = 0x018F58;
	hle_jump(ctx, 0x008F90);
}

/* $008F94: back from the confirmation: item 0 returns to the menu, the other quits */
HLE_RANGE(0x008F94, quit_confirm_done, 15, MENU_OPEN, 0x008F9A)
{
	hle_jump(ctx, (uint16_t)ctx->d[0] == 0 ? MENU_OPEN : 0x008F9A);
}

/* $008F9A soft_reset: "quit": [68k: bsr palette_read_cram at $008FA2] then SP and PC are taken
 * from the vectors and the game starts over (stays 68k: the SP change cannot be a range). */
HLE_RANGE(0x008F9A, soft_reset_begin, 24, 0x008FA2)
{
	RAM8(VBLANK_FLAGS) &= (uint8_t)~VBF_FRAME_WORK;
	hle_jump(ctx, 0x008FA2);
}

/* $008CDA menu_start_game: [68k: bsr palette_read_cram at $008CD6]  Restore the gameplay hooks
 * and return out of title_screen (the caller carries on with game_start).  Entered by the
 * title/pause menus, the password check ($00933E) and the DEBUG editors.  `rts` is $008D34. */
HLE_RANGE(0x008CDA, menu_start_game, 250, 0x008D34)
{
	vdp_ctrl_w(0x9001);
	RAM8(MENU_FLAGS) |= MF_GAME_STARTED;
	uint16_t room = RAM16(ROOM_INDEX);
	set_w(&ctx->d[0], room);
	if (room != RAM16(MENU_ROOM))
		RAM16(ROOM_ENTRY_OFS) = 0;       /* a different start room: first entry point */
	RAM16(ROOM_INDEX) = RAM16(MENU_ROOM);
	ctx->sr = 0x2700;
	vdp_ctrl_w(0x856C);
	RAM8(SCREEN_FLAGS) &= (uint8_t)~0x02;
	RAM32_W(HBLANK_HOOK, 0x01006C);          /* hblank_verb_bar */
	RAM32_W(VBLANK_HOOK, 0x00A65C);          /* vblank_main */
	ctx->sr = 0x2000;
	hle_jump(ctx, 0x008D34);
}

/* $008FB8 / $008FC2: PLAY BLAKE'S HOTEL / PLAY HA HA CARNIVAL: pick the mystery */
HLE_RANGE(0x008FB8, menu_play_blake, 30, 0x008FCA)
{
	RAM16(MYSTERY_IDX_W) = 0;
	hle_jump(ctx, 0x008FCA);
}
HLE_RANGE(0x008FC2, menu_play_carnival, 20, 0x008FCA)
{
	RAM16(MYSTERY_IDX_W) = 1;
	hle_jump(ctx, 0x008FCA);
}

/* $008FCA mystery_menu: reset the game state for the mystery and show its sub-menu START /
 * CONTINUE / CANCEL (3 items).  Ends in front of `bsr menu_choose` ($008FE6).
 * Live out: a0 = screen stream, d0 = 3. */
#define MYSTERY_CHOOSE 0x008FE6
HLE_RANGE(0x008FCA, mystery_menu, 3100, MYSTERY_CHOOSE)
{
	hle_call68k(ctx, 0x007F22);              /* game_state_init */
	ctx->a[0] = RAM16(MYSTERY_IDX_W) ? 0x018B3C : 0x018A56;
	ctx->d[0] = 3;
	hle_jump(ctx, MYSTERY_CHOOSE);
}

/* $008FEA: the sub-menu's choice: START -> the game, CANCEL -> main menu, CONTINUE -> the
 * ENTER PASSWORD screen (its art is unpacked here; the spin on the frame countdown at
 * $009008 follows, then password_screen_start).  Live out: nothing. */
HLE_RANGE(0x008FEA, mystery_menu_done, 400, 0x008CD6, MENU_OPEN, 0x009008)
{
	uint16_t d0 = (uint16_t)ctx->d[0];
	if (d0 == 0) {
		hle_jump(ctx, 0x008CD6);
		return;
	}
	if (d0 == 2) {
		hle_jump(ctx, MENU_OPEN);
		return;
	}
	rle(ctx, 0x018E48, LZSS_RING);
	hle_jump(ctx, 0x009008);
}

/* ================================================================== screen refresh helpers */

/* $008F1C menu_refresh_planes: upload plane A's strip data ($FFE000 -> VRAM $F000, $4B0
 * words) and the plane A names ($FFC400 -> $C800, 512 words), then update the title sprites.
 * Falls into $008F40. */
HLE_FUNC(0x008F1C, menu_refresh_planes, 750)
{
	menu_dma(ctx, 0xF000, 0x4B0, ZONE_MAP);
	menu_dma(ctx, 0xC800, 0x200, PLANEA_NAMES);
	menu_title_sprites(ctx);
}

/* $008F40 menu_title_sprites: write the 12 title sprites' Y coordinates ($0085F8 table +
 * TITLE_SCROLL / 8) into the sprite table at VRAM $FC00 (every 8th byte: auto-increment 8).
 * SR is saved after `lsr.w #3,d1` and restored at the end: the flags left behind are that
 * shift's (X = C = bit 2 of TITLE_SCROLL, Z = result 0). */
HLE_FUNC(0x008F40, menu_title_sprites, 740)
{
	uint16_t d1 = (uint16_t)lsrw(ctx, RAM8(TITLE_SCROLL), 3);   /* the flags saved below */
	uint16_t sr_in = ctx->sr;
	uint32_t a0 = ROM_TITLE_SAT;
	uint16_t d0 = 0;
	ctx->sr = 0x2700;
	vdp_ctrl_w(0x8F08);
	vdp_ctrl_w32(0x7C000003);
	for (int i = 0; i < 12; i++) {
		d0 = (uint16_t)(ROM16(a0) + d1);
		vdp_data_w(d0);
		a0 += 8;
	}
	vdp_ctrl_w(0x8F02);
	set_w(&ctx->d[0], d0);
	set_w(&ctx->d[1], d1);
	set_w(&ctx->d[2], 0xFFFF);
	ctx->a[0] = a0;
	ctx->sr = sr_in;
}

/* $008876: DMA the title strip ($FF8C00) to VRAM $DC00. Never called (the strip is uploaded
 * by vblank_title_fade). Kept so that nothing of the title code is left as unnamed 68k. */
HLE_FUNC(0x008876, title_strip_dma, 100)
{
	uint16_t sr_in = ctx->sr;
	ctx->sr = 0x2700;
	vdp_ctrl_w(0x8F04);
	set_w(&ctx->d[1], 0x40);
	set_w(&ctx->d[0], 0xDC00);
	ctx->a[0] = PLANEA_TMP;
	hle_call68k(ctx, A_VDP_DMA);
	vdp_ctrl_w(0x8F02);
	ctx->sr = sr_in;
}
