/* Password screens of the menus (GitLab #21): the encoder that shows the current password, the
 * ENTER PASSWORD screen with its key grid, and the checker behind END.
 *
 * The codec itself is password.c (pure C, also used by the save-slot feature later).  This file
 * is the glue to the 68k screens: it keeps the original's RAM layout (the menus use the sprite
 * buffer ram_sat_buf as scratch: raw bytes at $FF001E, symbols at $FF003E, text at $FF08BC) so
 * that everything the 68k code leaves in RAM is bit-identical.  docs/passwords.md has the
 * format, docs/subsystems/menus.md the screen layout.
 *
 * Register contracts: nobody reads the registers or flags these functions leave behind, except
 * where said otherwise; difftest ignores them through the table in difftest.c (proofs below).
 */
#include "menus.h"
#include "password.h"
#include <string.h>

/* ------------------------------------------------------------------ small 68k idioms */

/* roxl.w #1,dN: rotate left through X. Returns the new word, updates *x. */
static uint16_t roxl16(uint16_t v, unsigned *x)
{
	unsigned out = v >> 15;
	v = (uint16_t)((v << 1) | *x);
	*x = out;
	return v;
}
/* roxl.b #1,dN */
static uint8_t roxl8(uint8_t v, unsigned *x)
{
	unsigned out = v >> 7;
	v = (uint8_t)((v << 1) | *x);
	*x = out;
	return v;
}

/* ------------------------------------------------------------------ encoder ($008660) */

/* $00870E / $008710: the bit shifter of the encoder. Rotates d2.w, d3.w left through X, one bit
 * each, d4.w times, so that the top bits of d2 stream into the bottom of d3 (X is the link
 * between the two registers: d2.msb -> X -> d3.lsb, d3.msb -> X -> d2.lsb).
 * The loop ends on `subq.w #1,d4` reaching zero: Z=1, N=V=C=0 and X=0; with d4 = 0 nothing
 * runs and only Z=1, N=V=C=0 (tst.w). */
HLE_FUNC(0x008710, password_shift_bits, 20)
{
	unsigned x = (ctx->sr & CC_X) ? 1 : 0;
	uint16_t d2 = (uint16_t)ctx->d[2], d3 = (uint16_t)ctx->d[3], n = (uint16_t)ctx->d[4];
	ctx->cycles = 20 + 24 * n;
	for (uint16_t i = 0; i < n; i++) {
		d2 = roxl16(d2, &x);
		d3 = roxl16(d3, &x);
	}
	set_w(&ctx->d[2], d2);
	set_w(&ctx->d[3], d3);
	set_w(&ctx->d[4], 0);
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	if (n)
		cc_set(ctx, CC_X, 0);        /* subq.w #1,d4 down to 0: no borrow */
}

/* $00870E: d4 = 5 (one symbol's worth of bits), then the same */
HLE_FUNC(0x00870E, password_shift_symbol, 24)
{
	ctx->d[4] = 5;
	password_shift_bits(ctx);
}

/* $00873E: scramble the password bytes in place: r[0] = (r[1] ^ r[0]) & $1F, then a running
 * xor chain over the 29 state bytes (docs/passwords.md).  Registers after: scratch (d0-d2, a0);
 * the only caller (password_show, $008660) loads them afresh, flags dead. */
HLE_FUNC(0x00873E, password_scramble, 400)
{
	uint8_t acc = (uint8_t)((RAM8(PW_RAW + 1) ^ RAM8(PW_RAW)) & 0x1F);
	RAM8(PW_RAW) = acc;
	for (int i = 0; i < PASSWORD_STATE_BYTES; i++) {
		acc ^= RAM8(PW_RAW + 2 + i);
		RAM8(PW_RAW + 2 + i) = acc;
	}
}

/* $00876C: the inverse (decode): room = (r[1] ^ r[0]) & $1F into r[0]'s place, state[i] =
 * r[2+i] ^ r[1+i] (r[0] standing in for r[1]). Scratch regs d0-d3, a0, flags dead (only caller:
 * password_check). */
HLE_FUNC(0x00876C, password_unscramble, 450)
{
	uint8_t prev = RAM8(PW_RAW);
	RAM8(PW_RAW) = (uint8_t)((RAM8(PW_RAW + 1) ^ prev) & 0x1F);
	for (int i = 0; i < PASSWORD_STATE_BYTES; i++) {
		uint8_t b = RAM8(PW_RAW + 2 + i);
		RAM8(PW_RAW + 2 + i) = (uint8_t)(b ^ prev);
		prev = b;
	}
}

/* $008660 password_show: encode the current room ($FF06AC) and story state ($FF2A00..+28)
 * into the password text at $FF08BC (2 rows x (5 groups of 5 characters + space) + NUL).
 * Called by the title screen (the pre-filled password), the pause menu and the CLEAR key.
 *
 * Leaves the raw bytes at $FF001E and the 50 symbols at $FF003E like the original.  The last
 * symbol's two low bits come from raw[31] = stale $FF003D (the checker never looks at them), so
 * the stale byte is read, not assumed to be 0.
 * Registers (d0-d5, a0, a1) and flags are scratch for every caller: title_screen reloads them
 * before use, the CLEAR key restores them with movem, the password screen setup sets d4-d7. */
HLE_FUNC(0x008660, password_show, 14000)
{
	password_state st;
	st.room = RAM8(ROOM_INDEX + 1);
	for (int i = 0; i < PASSWORD_STATE_BYTES; i++)
		st.state[i] = RAM8(GAME_STATE + i);
	uint8_t raw[PASSWORD_RAW_BYTES], sym[PASSWORD_CHARS];
	password_raw_from_state(&st, raw);
	raw[31] = RAM8(PW_RAW + 31);
	for (int i = 0; i < 31; i++)
		RAM8(PW_RAW + i) = raw[i];
	password_unpack(raw, sym);
	for (int i = 0; i < PASSWORD_CHARS; i++)
		RAM8(PW_SYM + i) = sym[i];
	uint32_t o = TEXT_BUF;
	for (int row = 0; row < 2; row++) {
		for (int group = 0; group < 5; group++) {
			for (int k = 0; k < 5; k++)
				RAM8(o++) = ROM8(ROM_ALPHABET + sym[row * 25 + group * 5 + k]);
			RAM8(o++) = ' ';
		}
		RAM8(o++) = 0;
	}
}

/* ------------------------------------------------------------------ checker ($009298) */

/* $009392: next password bit.  d1 = bits left in d3, d3 = the current symbol shifted so that
 * its 5 bits are on top; when empty, load the next symbol byte from (a0)+ and shift it up by 3.
 * Returns the bit in X (and C).  Used only by password_read_byte; flags other than X are dead. */
HLE_FUNC(0x009392, password_read_bit, 40)
{
	unsigned x = (ctx->sr & CC_X) ? 1 : 0;
	uint8_t d3 = (uint8_t)ctx->d[3];
	if ((uint16_t)ctx->d[1] == 0) {
		d3 = (uint8_t)(RAM8(ctx->a[0]) << 3);
		ctx->a[0]++;
		set_w(&ctx->d[1], 5);
	}
	set_w(&ctx->d[1], ctx->d[1] - 1);
	d3 = roxl8(d3, &x);
	set_b(&ctx->d[3], d3);
	cc_set(ctx, CC_X, x ? CC_X : 0);
}

/* $009382: collect 8 password bits into d2.b (MSB first) */
HLE_FUNC(0x009382, password_read_byte, 400)
{
	unsigned x = (ctx->sr & CC_X) ? 1 : 0;
	uint16_t d2 = 0;
	ctx->d[2] = 0;
	for (int i = 0; i < 8; i++) {
		cc_set(ctx, CC_X, x ? CC_X : 0);
		password_read_bit(ctx);
		x = (ctx->sr & CC_X) ? 1 : 0;
		d2 = roxl16(d2, &x);
	}
	set_w(&ctx->d[2], d2);
	set_w(&ctx->d[4], 0xFFFF);
	cc_set(ctx, CC_X, x ? CC_X : 0);
}

/* $009298 password_check: the END key.  Turns the typed text into raw bytes, decodes them and
 * verifies the checksum.  Bad password: error sound $05 and rts back into the entry loop.
 * Good password: leaves through $009328 (68k: pops the key handler's return address, plays
 * sound $59, waits 61 frames) and the accept code, see password_accept.
 *
 * The text is looked up in the alphabet; the bits are read 5 per symbol over 256 bits, so the
 * last byte r[31] comes from symbols 49 (low bits) .. 51 ($FF0070/71 are stale scratch).
 * d4 is saved/restored by the original; d5-d7 are untouched; everything else is scratch. */
#define PW_CHECK_RTS   0x009326
#define PW_CHECK_OK    0x009328
HLE_RANGE(0x009298, password_check, 9000, PW_CHECK_RTS, PW_CHECK_OK)
{
	/* typed text -> symbol numbers: the position of each character in the alphabet */
	for (int row = 0; row < 2; row++)
		for (int group = 0; group < 5; group++)
			for (int k = 0; k < 5; k++) {
				uint8_t ch = RAM8(TEXT_BUF + row * 31 + group * 6 + k);
				int16_t i = 0;
				while (ROM8(ROM_ALPHABET + i) != ch)   /* the original scans on past the table */
					i++;
				RAM8(PW_SYM + row * 25 + group * 5 + k) = (uint8_t)i;
			}
	/* 256 bits, 5 per symbol byte (low 5 bits), MSB first */
	uint8_t sym[52];
	for (int i = 0; i < 52; i++)
		sym[i] = RAM8(PW_SYM + i);
	uint8_t raw[PASSWORD_RAW_BYTES];
	memset(raw, 0, sizeof raw);
	for (int bit = 0; bit < 256; bit++)
		if ((sym[bit / 5] >> (4 - bit % 5)) & 1)
			raw[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
	for (int i = 0; i < PASSWORD_RAW_BYTES; i++)
		RAM8(PW_RAW + i) = raw[i];
	/* decode in place (r[0] <- room, state bytes), then verify against r[1] */
	hle_ctx tmp = *ctx;
	password_unscramble(&tmp);
	uint8_t state[PASSWORD_STATE_BYTES];
	for (int i = 0; i < PASSWORD_STATE_BYTES; i++)
		state[i] = RAM8(PW_RAW + 2 + i);
	bool ok = password_checksum(RAM8(PW_RAW), state) == RAM8(PW_RAW + 1);
	ctx->sr = 0x2000;                    /* move.w #$2000,sr */
	if (!ok) {
		menu_sound(ctx, 0x05);
		hle_jump(ctx, PW_CHECK_RTS);
		return;
	}
	hle_jump(ctx, PW_CHECK_OK);
}

/* $009340: after the success sound and the 61-frame wait: start the game from the password.
 * Clears the state block, copies the 29 decoded bytes in, sets the start room ($FF0A90) and
 * goes on to menu_start_game ($008CD6).  MF_FROM_PASSWORD makes the game's intro cutscenes
 * count as a restored game. */
HLE_RANGE(0x00933E, password_accept, 900, 0x008CD6)
{
	RAM8(MENU_FLAGS) |= MF_FROM_PASSWORD;
	for (int i = 0; i < 0x100; i++)      /* 128 words */
		RAM8(GAME_STATE + i) = 0;
	for (int i = 0; i < PASSWORD_STATE_BYTES; i++)
		RAM8(GAME_STATE + i) = RAM8(PW_RAW + 2 + i);
	RAM16(0xFF0A90) = RAM8(PW_RAW);      /* start room (ram_start_room) */
	hle_jump(ctx, 0x008CD6);
}

/* ------------------------------------------------------------------ key handlers */
/* The icons of row 4 of the grid ($009164 table): d4 = cursor position in the row (0..24),
 * d5 = row (0/1).  All play sound $75 (move) except CLEAR ($63).  Registers/flags otherwise
 * scratch for the caller (the loop reloads d0-d3 each pass). */

/* $0093A2: cursor left (wraps to the end of the row) */
HLE_FUNC(0x0093A2, password_cursor_left, 100)
{
	menu_sound(ctx, 0x75);
	int16_t d4 = (int16_t)ctx->d[4] - 1;
	set_w(&ctx->d[4], d4 < 0 ? 0x18 : (uint16_t)d4);
}

/* $0093B6: cursor right */
HLE_FUNC(0x0093B6, password_cursor_right, 100)
{
	menu_sound(ctx, 0x75);
	int16_t d4 = (int16_t)ctx->d[4] + 1;
	set_w(&ctx->d[4], d4 > 0x18 ? 0 : (uint16_t)d4);
}

/* $0093CC: switch between the two password rows (icons 2 and 3) */
HLE_FUNC(0x0093CC, password_switch_row, 90)
{
	menu_sound(ctx, 0x75);
	set_w(&ctx->d[5], ctx->d[5] ^ 1);
}

/* $0093DC: CLEAR: sound $63, regenerate the text of the current game state, cursor home.
 * (movem keeps every register but d4/d5.) */
HLE_FUNC(0x0093DC, password_clear, 300)
{
	hle_ctx save = *ctx;
	menu_sound(ctx, 0x63);
	password_show(ctx);
	uint16_t sr = ctx->sr;
	*ctx = save;
	ctx->sr = sr;
	ctx->d[4] = 0;
	ctx->d[5] = 0;
}

/* $009162: icon 6 does nothing (rts) */
HLE_FUNC(0x009162, password_key_noop, 0) { (void)ctx; }

/* $0093F8: unreferenced (`bra menu_start_game`); never run, kept as a tail jump */
HLE_FUNC(0x0093F8, password_unused_start, 0) { hle_jump(ctx, 0x008CD6); }

/* ------------------------------------------------------------------ cancel ($00927A) */
/* The CANCEL icon: [68k: wait_vblank at $00927A], hide the cursor sprites, then pop the key
 * handler's return address ([68k: tst.l (sp)+ at $009292]) and go back to the menu ($00851E). */
HLE_RANGE(0x00927E, password_cancel, 50, 0x009292)
{
	vdp_ctrl_w32(0x7C600003);        /* VRAM $FC60: the two cursor sprites */
	bus_w32(VDP_DATA, 0);
	hle_jump(ctx, 0x009292);
}

/* ================================================================== ENTER PASSWORD screen */

/* Screen setup, after the screen's RLE art ($018E48) was unpacked by the 68k (the spin on the
 * frame countdown at $009008 follows it).  d4-d7: cursor position in the text (0..24), row of
 * the text (0/1), column and row of the key grid (8 x 5). */
#define PW_SCREEN_SPIN 0x009008

/* $0090xx: the key grid loop body from $009052: D-pad moves the grid cursor, A/B/Start press
 * the key.  A key press either types the character (grid rows 0..3) or runs an icon (row 4) by
 * `jsr (a0)` ($00915E), which is the 68k's to do: this function stops in front of it.
 * Returns the exit address. */
#define PW_JSR_ICON    0x00915E
#define PW_WAIT_FRAME  0x0091D4   /* bsr wait_vblank */
static uint32_t pw_input_body(hle_ctx *ctx)
{
	uint8_t pad = RAM8(PAD1_CUR), prev = RAM8(MENU_PAD_PREV);
	uint16_t pos = (uint16_t)ctx->d[4], trow = (uint16_t)ctx->d[5];
	int16_t col = (int16_t)ctx->d[6], grow = (int16_t)ctx->d[7];

	if (menu_pressed_edge(pad, prev, PAD_U)) {         /* grid cursor up (wraps 0 -> 4) */
		grow--;
		if (grow < 0)
			grow = 4;
	}
	if (menu_pressed_edge(pad, prev, PAD_D)) {         /* down (wraps 4 -> 0) */
		grow++;
		if (grow >= 5)
			grow = 0;
	}
	if (menu_pressed_edge(pad, prev, PAD_R))
		col = (col + 1) & 7;
	if (menu_pressed_edge(pad, prev, PAD_L))
		col = (col - 1) & 7;
	set_w(&ctx->d[6], (uint16_t)col);
	set_w(&ctx->d[7], (uint16_t)grow);
	/* A, B or Start ($D0), only on a fresh press: none of them was down in the last frame */
	uint32_t exit_pc = PW_WAIT_FRAME;
	if ((prev & 0xD0) == 0xD0 && (pad & 0xD0) != 0xD0) {
		if (grow == 4) {
			/* icon: the handler is called by the 68k (jsr (a0)); d0 = col*4, a0 = handler */
			set_w(&ctx->d[0], (uint16_t)(col << 2));
			ctx->a[0] = ROM32(0x009164 + (uint16_t)(col << 2));
			exit_pc = PW_JSR_ICON;
			return exit_pc;
		}
		menu_sound(ctx, 0x27);                          /* key click */
		uint8_t ch = ROM8(ROM_ALPHABET + grow * 8 + col);
		/* position in the text: 5 characters, a space, 5 characters ...; row 1 starts 31 bytes in */
		uint16_t idx = pos + (trow ? 31 : 0);
		for (int16_t r = (int16_t)pos - 5; r >= 0; r -= 5)
			idx++;
		ctx->a[0] = TEXT_BUF;
		RAM8(TEXT_BUF + idx) = ch;
		pos++;
		if ((int16_t)pos > 0x18) {                      /* end of a row: continue on the other one */
			pos = 0;
			trow ^= 1;
		}
		set_w(&ctx->d[4], pos);
		set_w(&ctx->d[5], trow);
	}
	return exit_pc;
}

/* end of a loop pass ($0091CA): remember the pad for the next edge test, wait for VBlank */
static void pw_end_pass(hle_ctx *ctx)
{
	RAM8(MENU_PAD_PREV) = RAM8(PAD1_CUR);
	hle_jump(ctx, PW_WAIT_FRAME);
}

#define PW_LOOP      0x00903A
#define PW_INPUT     0x009052

/* $009010: the screen's art is in the name table: draw it, show the current password, set up
 * the cursor registers, fall into the loop at $00903A */
HLE_RANGE(0x009010, password_screen_start, 4000, PW_LOOP)
{
	RAM16(FRAME_COUNTDOWN) = 4;
	menu_refresh_planes(ctx);
	password_show(ctx);
	menu_dma(ctx, 0xC000, 0x380, LZSS_RING);
	ctx->d[4] = ctx->d[5] = ctx->d[6] = ctx->d[7] = 0;
	hle_jump(ctx, PW_LOOP);
}

/* $00903A: new frame of the loop: reset the pacing countdown, redraw the sprites, then input */
HLE_RANGE(0x00903A, password_screen_frame, 400, PW_JSR_ICON, PW_WAIT_FRAME)
{
	RAM16(FRAME_COUNTDOWN) = 4;
	menu_sprites_refresh(ctx);
	uint32_t e = pw_input_body(ctx);
	if (e == PW_JSR_ICON) {
		hle_jump(ctx, e);
		return;
	}
	pw_end_pass(ctx);
}

/* $009052: input pass without the countdown reset (the countdown has not run out yet) */
HLE_RANGE(0x009052, password_screen_input, 300, PW_JSR_ICON, PW_WAIT_FRAME)
{
	uint32_t e = pw_input_body(ctx);
	if (e == PW_JSR_ICON) {
		hle_jump(ctx, e);
		return;
	}
	pw_end_pass(ctx);
}

/* $009160: back from an icon handler: end of pass */
HLE_RANGE(0x009160, password_screen_icon_done, 20, PW_WAIT_FRAME)
{
	pw_end_pass(ctx);
}

/* $0091D8: after the VBlank wait: countdown still running -> poll the pad again; run out ->
 * refresh the planes, place the two cursor sprites (the key grid cursor at $FC60 and the
 * text cursor), redraw the two text rows and start a new frame ($00903A). */
HLE_RANGE(0x0091D8, password_screen_tick, 3000, PW_LOOP, PW_INPUT)
{
	if (!(RAM16(FRAME_COUNTDOWN) & 0x8000)) {
		hle_jump(ctx, PW_INPUT);
		return;
	}
	menu_refresh_planes(ctx);
	uint16_t pos = (uint16_t)ctx->d[4], trow = (uint16_t)ctx->d[5];
	int16_t col = (int16_t)ctx->d[6], grow = (int16_t)ctx->d[7];
	uint16_t grid_y = (uint16_t)(grow * 16 + 0xF0), grid_x = (uint16_t)(col * 8 + 0xD8);
	uint16_t groups = pos / 5;                          /* separators before the cursor */
	uint16_t text_x = (uint16_t)((pos + groups) * 8 + 0x88);
	uint16_t text_y = (uint16_t)(trow * 8 + 0xB8);
	ctx->sr = 0x2700;
	vdp_ctrl_w32(0x7C600003);                           /* sprites 12 and 13 of the table at VRAM $FC00 */
	vdp_data_w(grid_y);
	vdp_data_w(0x010D);
	vdp_data_w(0x07D0);
	vdp_data_w(grid_x);
	vdp_data_w(text_y);
	vdp_data_w(0x0000);
	vdp_data_w(0x07D0);
	vdp_data_w(text_x);
	ctx->sr = 0x2000;
	/* the two password text rows, plane A rows 7 and 8, columns from 1 */
	ctx->a[0] = TEXT_BUF;
	ctx->d[2] = 7;
	ctx->d[1] = 1;
	hle_call68k(ctx, A_TEXT_9F58);
	set_w(&ctx->d[2], ctx->d[2] + 1);
	hle_call68k(ctx, A_TEXT_9F58);
	hle_jump(ctx, PW_LOOP);
}
