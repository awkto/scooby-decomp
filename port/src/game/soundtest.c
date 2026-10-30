/* Sound test screen ($00940C, GitLab #21), reached from the title menu's third item.
 * docs/sound.md has the sound ids and names ($1F7082 names, $1F7492 ids: 87 entries).
 *
 * D-pad: up/down changes the high digit (0..5), left/right the low digit (0..15, row 5 only
 * 0..6): the selection is d6 * 16 + d7.  B plays the selected sound, A stops it, C leaves
 * (the music that was playing before is restarted).  The screen shows the two hex digits as
 * font tiles at VRAM $C51E/$C520 and the sound's name centred on row 17.
 *
 * The loop polls the pad without waiting for VBlank (the countdown $FF08AE paces the redraw),
 * so it is a range that loops on itself.  Registers: d5 = index of the name on screen ($FFFF
 * before the first), d6 = high digit, d7 = low digit; a1 = pointer to the name while it is being
 * drawn.  The pad is read as `down now and up in the last frame, or the edge pad_read
 * accumulated since pad_ref` (menu_pressed_edge).
 */
#include "menus.h"

#define ROM_SOUND_NAMES   0x1F7082
#define ROM_SOUND_IDS     0x1F7492
#define ROM_DIGIT_TILES   0x0093FC    /* font tile offset of the hex digits 0..15 */
#define ST_SPIN_EXIT      0x00963E    /* spin on the countdown, then leave (the C key) */
#define ST_POLL           0x00947C
#define ST_FRAME          0x00946C
#define ST_NAME_WAIT      0x0096A8    /* bsr wait_vblank before the name is drawn */

/* keep d6/d7 on the valid grid: rows 0..5 of 16 digits, the last row only has 7 sounds.
 * (moveq #0,d6 clears the whole register, the other updates only its low word.) */
static void st_wrap(hle_ctx *ctx)
{
	int16_t hi = (int16_t)ctx->d[6], lo = (int16_t)ctx->d[7];
	bool clear6 = false;
	if (hi == 5) {
		if (lo < 0) {                    /* left of column 0: end of the row above */
			hi--;
			lo &= 15;
		} else if (lo == 7) {            /* past the last sound: wrap to the start */
			clear6 = true;
			lo = 0;
		}
	} else {
		if (lo < 0) {
			hi--;
			lo &= 15;
		} else if (lo == 16) {           /* past column 15: next row */
			lo &= 15;
			hi++;
		}
	}
	if (!clear6) {
		if (hi < 0)
			hi = 5;
		else if (hi > 5)
			clear6 = true;
	}
	if (!clear6 && hi == 5 && lo >= 7)
		lo = 6;
	if (clear6)
		ctx->d[6] = 0;
	else
		set_w(&ctx->d[6], (uint16_t)hi);
	set_w(&ctx->d[7], (uint16_t)lo);
}

/* the two hex digit tiles of the selection */
static void st_draw_digits(hle_ctx *ctx)
{
	set_w(&ctx->d[0], 0xC51E);
	ctx->a[0] = ROM_DIGIT_TILES;
	set_w(&ctx->d[1], (uint16_t)((int8_t)ROM8(ROM_DIGIT_TILES + (uint16_t)ctx->d[6]) + RAM16(FONT_BASE)));
	hle_call68k(ctx, A_VDP_WRITE_WORD);
	set_w(&ctx->d[0], ctx->d[0] + 2);
	set_w(&ctx->d[1], (uint16_t)((int8_t)ROM8(ROM_DIGIT_TILES + (uint16_t)ctx->d[7]) + RAM16(FONT_BASE)));
	hle_call68k(ctx, A_VDP_WRITE_WORD);
}

/* One pass of the pad poll ($00947C..$009678).  Returns the exit address. */
static uint32_t st_poll(hle_ctx *ctx)
{
	uint8_t pad = RAM8(PAD1_CUR), prev = RAM8(MENU_PAD_PREV);
	if (menu_pressed_edge(pad, prev, PAD_U))
		set_w(&ctx->d[6], ctx->d[6] + 1);
	if (menu_pressed_edge(pad, prev, PAD_D))
		set_w(&ctx->d[6], ctx->d[6] - 1);
	if (menu_pressed_edge(pad, prev, PAD_R))
		set_w(&ctx->d[7], ctx->d[7] + 1);
	if (menu_pressed_edge(pad, prev, PAD_L))
		set_w(&ctx->d[7], ctx->d[7] - 1);
	st_wrap(ctx);
	if (menu_pressed_edge(pad, prev, PAD_B)) {           /* B: play the sound on screen */
		hle_call68k(ctx, A_SOUND_STOP_ALL);
		uint32_t id = ROM8(ROM_SOUND_IDS + (int16_t)ctx->d[5]);
		ctx->d[0] = id;
		hle_call68k(ctx, A_SOUND_PLAY);
	}
	if (menu_pressed_edge(pad, prev, PAD_A))             /* A: stop */
		hle_call68k(ctx, A_SOUND_STOP_ALL);
	/* C: leave.  This one does not re-arm the edge (the screen is going away). */
	if (!(pad & PAD_C) && ((prev & PAD_C) || (RAM8(PAD1_EDGE) & PAD_C)))
		return ST_SPIN_EXIT;
	RAM8(MENU_PAD_PREV) = pad;
	if (!(RAM16(FRAME_COUNTDOWN) & 0x8000))              /* the next frame is not due yet */
		return ST_POLL;
	RAM16(FRAME_COUNTDOWN) = 4;
	menu_refresh_planes(ctx);
	ctx->cycles += 100;
	uint16_t sel = (uint16_t)(((uint16_t)ctx->d[6] << 4) | (uint16_t)ctx->d[7]);
	set_w(&ctx->d[0], sel);
	if ((uint16_t)ctx->d[5] == sel) {                    /* same sound as before: only the digits */
		st_draw_digits(ctx);
		return ST_FRAME;
	}
	set_w(&ctx->d[5], sel);
	/* a1 -> the sel'th NUL-terminated name */
	uint32_t a1 = ROM_SOUND_NAMES;
	for (uint16_t n = sel; n; n--) {
		while (ROM8(a1++))
			;
	}
	ctx->a[1] = a1;
	set_w(&ctx->d[0], 0xFFFF);
	return ST_NAME_WAIT;
}

/* $00940C sound_test: stop the sound, load the screen ($018C72); the spin on the frame countdown
 * at $009444 follows.  Live out: d5-d7. */
HLE_RANGE(0x00940C, sound_test, 500, 0x009444)
{
	RAM8(MENU_PAD_PREV) = RAM8(PAD1_PREV);
	hle_call68k(ctx, A_SPRITES_E40C);
	hle_call68k(ctx, A_SPRITES_F272);
	hle_call68k(ctx, A_SOUND_STOP_ALL);
	ctx->a[0] = 0x018C72;
	ctx->a[2] = LZSS_RING;
	rle_call(ctx);
	ctx->d[6] = 0;
	ctx->d[7] = 0;
	set_w(&ctx->d[5], 0xFFFF);
	RAM8(FONT_BASE) |= 0x80;               /* font palette 4 (high bit of the tile attribute) */
	hle_jump(ctx, 0x009444);
}

/* $00944E: first frame: upload the screen and the planes */
HLE_RANGE(0x00944E, sound_test_show, 100, ST_FRAME)
{
	RAM16(FRAME_COUNTDOWN) = 4;
	menu_dma(ctx, 0xC000, 0x380, LZSS_RING);
	menu_refresh_planes(ctx);
	hle_jump(ctx, ST_FRAME);
}

/* $00946C: redraw the sprites (all registers kept); the pad pass follows at $00947C */
HLE_RANGE(0x00946C, sound_test_frame, 400, ST_POLL)
{
	menu_sprites_refresh(ctx);
	hle_jump(ctx, ST_POLL);
}

/* $00947C: the pad pass alone (this loop runs between VBlanks) */
HLE_RANGE(0x00947C, sound_test_poll, 360, ST_SPIN_EXIT, ST_POLL, ST_NAME_WAIT, ST_FRAME)
{
	hle_jump(ctx, st_poll(ctx));
}

/* $0096AC: after the VBlank wait: blank row 17, draw the name (a1) centred, then the digits */
HLE_RANGE(0x0096AC, sound_test_name, 400, ST_FRAME)
{
	ctx->a[0] = 0x02FFF4;                  /* blank line */
	set_w(&ctx->d[2], 17);
	ctx->d[1] = 1;
	hle_call68k(ctx, A_TEXT_9F72);
	ctx->d[1] = 1;
	ctx->a[0] = ctx->a[1];
	hle_call68k(ctx, A_TEXT_9F30);
	st_draw_digits(ctx);
	hle_jump(ctx, ST_FRAME);
}

/* $009648: C pressed and the countdown ran out: leave: refresh, restore the font palette, stop the
 * sound and restart the music that played before the test (ram_cur_music), back to the menu */
HLE_RANGE(0x009648, sound_test_exit, 150, MENU_OPEN_ADDR)
{
	menu_refresh_planes(ctx);
	RAM8(FONT_BASE) &= (uint8_t)~0x80;
	hle_call68k(ctx, A_SOUND_STOP_ALL);
	ctx->d[0] = RAM32(MUSIC_CUR);
	hle_call68k(ctx, A_SOUND_PLAY);
	hle_jump(ctx, MENU_OPEN_ADDR);
}
