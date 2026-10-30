/* Camera -> scroll registers (VBlank), GitLab #24 follow-up.  The column/row streaming helpers
 * it calls ($00FC3A, $00FD5E, $00FE74, $00FF68) are the room subsystem's and stay 68k. */
#include "game.h"
#include "cc68k.h"

#define CAMERA_X        0xFF06CA   /* w: world x of the left screen edge */
#define CAMERA_Y        0xFF06CE   /* w: world y at the top of the view */
#define HSCROLL_A       0xFF07F8   /* w: plane A h-scroll = -camera x (or follows Shaggy, see below) */
#define HSCROLL_B       0xFF07FA   /* w: plane B h-scroll = -camera x */
#define VSCROLL_A       0xFF07FC   /* w: plane A v-scroll */
#define VSCROLL_B       0xFF07FE   /* w: plane B v-scroll (the pair is written to VSRAM as one long) */
#define SHAKE_POS       0xFF08AA   /* w: byte index into the shake table, -1 = pause between shakes */
#define SHAKE_PHASE     0xFF08AC   /* b: which of the 4 pause lengths comes next */
#define PAN_STEP        0xFF08B4   /* w: signed pixels per frame of an automatic camera pan */
#define PAN_TARGET      0xFF08B6   /* w: camera x at which the pan stops */
#define SHAKE_PAUSE     0xFF0ACF   /* b: frames left of the pause */
#define SCROLL_REQ      0xFF0AB3   /* b: bit 0 stream a column right, 1 left, 2 row up, 3 down */
#define ROM_SHAKE_PAUSES 0x03247C  /* b[4]: 119, 119, 14, 0 */
#define ROM_SHAKE_OFFSETS 0x03244E /* w[22]: 0 0 1 2 3 4 5 6 7 7 8 8 8 7 7 6 5 4 3 2 1 0 (read as pairs) */
#define HSCROLL_TABLE   0xDC00     /* VRAM: h-scroll table (full-screen mode: plane A, plane B words) */

/* $00FA78 camera_update: called first thing in every in-game VBlank (vblank_main).
 *  - an automatic pan (bit 5 of $FF09E9) moves camera x by PAN_STEP per frame and asks for a new
 *    tile column (SCROLL_REQ bit 0/1) whenever the camera crosses a tile boundary;
 *  - h-scroll B = -(camera x & $3FF); plane A follows the camera too, except in the cut-scene
 *    layout (verb_state bit 6) where it is Shaggy's screen position minus a fixed offset;
 *  - v-scroll = camera y & $FF (same exception);
 *  - the screen shake (bit 7 of $FF09E8) adds a table offset to both v-scrolls;
 *  - the requested columns/rows are streamed by the room helpers, then the h-scroll words go to
 *    VRAM $DC00 and the v-scroll pair to VSRAM.
 * Bit 1 of $FF09EB (no camera scroll) skips straight to the VDP writes.
 * Registers/flags: d0-d3, a0 are scratch and flags are dead (the only caller is vblank_main, which
 * reads neither; see the exemption in difftest.c). */
HLE_FUNC(0x00FA78, camera_update, 0)
{
	if (!(RAM8(0xFF09EB) & 0x02)) {
		if (RAM8(0xFF09E9) & 0x20) {
			int16_t step = (int16_t)RAM16(PAN_STEP);
			uint16_t x = (uint16_t)(RAM16(CAMERA_X) + step);
			RAM16(CAMERA_X) = x;
			if ((x & 7) == (uint16_t)((8 + step) & 7))       /* crossed a tile boundary */
				RAM8(SCROLL_REQ) |= step >= 0 ? 0x01 : 0x02;
			if (x == RAM16(PAN_TARGET))
				RAM8(0xFF09E9) &= (uint8_t)~0x20;
		}
		uint16_t h = (uint16_t)-(RAM16(CAMERA_X) & 0x3FF);
		RAM16(HSCROLL_B) = h;
		bool cutscene = RAM8(0xFF09DE) & 0x40;            /* verb_state bit 6 */
		if (cutscene)
			h = (uint16_t)(RAM16(0xFF04DC) - RAM16(CAMERA_X) - 0x2C);
		RAM16(HSCROLL_A) = h;
		uint16_t v = RAM16(CAMERA_Y) & 0xFF;
		RAM16(VSCROLL_B) = v;
		if (cutscene)
			v = (uint16_t)-(int16_t)(RAM16(0xFF04F4) - RAM16(CAMERA_Y) - 0x30);
		RAM16(VSCROLL_A) = v;

		if (RAM8(0xFF09E8) & 0x80) {                      /* screen shake */
			bool run = true;
			if ((int16_t)RAM16(SHAKE_POS) < 0) {          /* pausing between two shakes */
				RAM8(SHAKE_PAUSE)--;
				if ((int8_t)RAM8(SHAKE_PAUSE) >= 0) {
					run = false;                          /* still waiting */
				} else {
					int phase = (int8_t)RAM8(SHAKE_PHASE);
					RAM8(SHAKE_PAUSE) = ROM8(ROM_SHAKE_PAUSES + phase);
					RAM8(SHAKE_PHASE) = (uint8_t)((phase + 1 == 4) ? 0 : phase + 1);
					RAM16(SHAKE_POS) = 0;
				}
			}
			if (run) {
				uint16_t pos = RAM16(SHAKE_POS);
				RAM16(SHAKE_POS) = (uint16_t)(pos + 2);
				if (RAM16(SHAKE_POS) == 0x2C)
					RAM16(SHAKE_POS) = 0xFFFF;             /* end: pause again */
				RAM16(VSCROLL_A) += ROM16(ROM_SHAKE_OFFSETS + pos);
				RAM16(VSCROLL_B) += ROM16(ROM_SHAKE_OFFSETS + pos + 2);
			}
		}
		/* stream the requested columns / rows (room subsystem, 68k) */
		static const uint32_t streamers[4] = { 0x00FC3A, 0x00FD5E, 0x00FE74, 0x00FF68 };
		for (int bit = 0; bit < 4; bit++)
			if (RAM8(SCROLL_REQ) & (1 << bit))
				hle_call68k(ctx, streamers[bit]);
		RAM8(SCROLL_REQ) = 0;
	}
	set_w(&ctx->d[0], HSCROLL_TABLE);
	set_w(&ctx->d[1], RAM16(HSCROLL_A));
	vram_write_word(ctx);
	set_w(&ctx->d[0], HSCROLL_TABLE + 2);
	set_w(&ctx->d[1], RAM16(HSCROLL_B));
	vram_write_word(ctx);
	vdp_ctrl_w32(0x40000010);                             /* VSRAM write at 0 */
	bus_w32(VDP_DATA, RAM32(VSCROLL_A));
	ctx->cycles = 980;
}
