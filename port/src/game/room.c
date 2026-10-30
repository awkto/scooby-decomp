/* Room / scene loader (issue #18).  See docs/subsystems/room.md and docs/formats.md "Rooms".
 *
 * The LZSS decompressor ($0097AA) is not ported here (issue #14): it is called through
 * hle_call68k, so it runs as a C function as soon as the compression subsystem provides one.
 * VRAM writes go through vram_write_word()/vram_write_words()/vram_fill_words() (vdp.c).
 *
 * Terminology used below:
 *   hdr    the active mystery header (pointer in MYSTERY_HDR, layout in docs/formats.md)
 *   entry  the 0x14-byte room record in hdr+8, indexed by (room number - 2)
 *   tiles  4bpp tile pixels; "names" are VDP nametable words
 */
#include "game.h"
#include <string.h>
#include "gx.h"

/* ---- mystery header field offsets (all absolute longs, see docs/formats.md) */
enum {
	HDR_STATE_START = 0x00,  /* initial game-state block start (copied to $FF2A00) */
	HDR_OBJECTS     = 0x04,  /* end of the state block = start of the object init records */
	HDR_ROOMS       = 0x08,  /* room table, 0x14 bytes per room; also end of the object records */
	HDR_TILES_BASE  = 0x0C,
	HDR_MAPS_BASE   = 0x10,
	HDR_PALETTES    = 0x14,  /* 128 bytes per room */
	HDR_START_ROOM  = 0x30,  /* -> { u16 room, u16 music, u16 script_len, script... } */
};
/* ---- room table entry field offsets */
enum {
	ENT_NAME   = 0x00,  /* + hdr names_base ($20) */
	ENT_TILES  = 0x04,  /* + hdr tiles_base */
	ENT_MAP    = 0x08,  /* + hdr maps_base */
	ENT_SIZE   = 0x14,
};
/* ---- object record (0x1A bytes in RAM) */
enum {
	OBJ_OVERLAY = 0x00,  /* word: tile overlay id (index+1 into hdr+$24 table); bit15 = restore, bit14 = one-shot */
	OBJ_TYPE    = 0x02,  /* word: sprite/actor definition id (matched against the table at $032670) */
	OBJ_ROOM    = 0x06,  /* word: room it is in (0 = nowhere, 1 = inventory) */
	OBJ_TIMER   = 0x0A,  /* word: countdown, -1 = off */
	OBJ_X       = 0x12,  /* word: spawn x (pixels) -- actors only */
	OBJ_Y       = 0x14,  /* word: spawn y */
	OBJ_FLAGS   = 0x18,  /* byte: bit0 in current room, bit1 actor, bit2 timer expired, bit3 ? */
	OBJ_SLOT    = 0x19,  /* byte: actor slot (0..3) or $FF */
};

#define SHARED_PALETTES   0x032480  /* CRAM palettes 0-1 (32 words) */
#define UI_FONT_LZSS      0x030916
#define UI_TILES_A        0x031AFA  /* 0x40 words */
#define UI_TILES_B        0x031B7A  /* 0xC0 words */
#define UI_TILES_C        0x031CFA  /* 0x80 words */
#define VERB_BAR_NAMES    0x030016
#define FONT_GLYPHS       0x030F12  /* 0x5F0 words of 1-colour glyph tiles, recoloured by font_tiles_load */
#define ACTOR_DEF_PTRS    0x032670  /* 36 longs: pointers to actor sprite definitions (first word = id) */
#define ACTOR_DEF_SIZES   0x0325E0  /* 36 longs: tile data size in bytes of each definition */

#define VBLANK_FLAGS      0xFF09DC
#define FLAGS_09DE        0xFF09DE  /* byte: room/scene mode flags (bit2, bit3, bit5, bit6, bit7 used here) */
#define GAME_STATE        0xFF2A00

/* Heavy nested 68k code (LZSS): the HLE layer rewinds the CPU clock after every nested call
 * and adds the nested time to the function's charge (hle.c, #31), so a later Z80 bus request
 * no longer catches the Z80/YM2612 up by frames at once (the YM buffer overflow found by the
 * mouse soak test). Charges below are "original total - hle_nested_cycles()". */
static void nested68k(hle_ctx *ctx, uint32_t addr)
{
	hle_call68k(ctx, addr);
}

/* total 68k cost of the original, minus what the nested calls already accounted for */
static int charge_total(int original_total)
{
	int c = original_total - hle_nested_cycles();
	return c > 0 ? c : 0;
}

/* LZSS: a0 = stream (after the u32 length), a1 = ring buffer, a2 = output; size left at (a1).l */
static void lzss(hle_ctx *ctx, uint32_t stream, uint32_t out)
{
	ctx->a[0] = stream;
	ctx->a[1] = LZSS_RING;
	ctx->a[2] = out;
	nested68k(ctx, 0x0097AA);
}

/* VRAM helpers (register-free wrappers around the vdp.c routines). */
static void vram_poke(hle_ctx *ctx, uint16_t addr, uint16_t v)
{
	ctx->d[0] = addr;
	ctx->d[1] = v;
	vram_write_word(ctx);
}
static void vram_copy(hle_ctx *ctx, uint16_t addr, uint32_t src, uint16_t nwords)
{
	ctx->d[0] = addr;
	ctx->d[1] = nwords;
	ctx->a[0] = src;
	vram_write_words(ctx);
}
static void vram_fill(hle_ctx *ctx, uint16_t addr, uint16_t nwords, uint16_t v)
{
	ctx->d[0] = addr;
	ctx->d[1] = nwords;
	ctx->d[2] = v;
	vram_fill_words(ctx);
}

/* $007A2A: load the 94-tile text font ($030F12) into VRAM and recolour it.
 *   D0.w = colour index for glyph pixels (non-zero source nibbles), D1.w = colour index for the
 *   background (zero nibbles); D2.w = font base name word (bit 15 = priority flag, low 11 bits =
 *   tile index - 0x20 ... i.e. the tiles are written at VRAM ((D2 & $7FF) + $20) * 32).
 * Returns D0.w = VRAM address after the last word; the other registers hold the loop leftovers
 * (callers only use D0).  CCR: X = carry of the last `addq.w #2,d0`, N/Z from the final
 * `tst.l (sp)+` of the pushed colour pair, V = C = 0. */
HLE_FUNC(0x007A2A, font_tiles_load, 0)
{
	uint32_t *d = ctx->d;
	uint16_t glyph_col = (uint16_t)(d[0] << 12);   /* pushed first  -> (2,sp) */
	uint16_t bg_col    = (uint16_t)(d[1] << 12);   /* pushed second -> (sp)   */
	uint16_t addr, w = 0, d3 = 0, d4 = 0, d6 = 0, d7 = 0;
	uint16_t base = (uint16_t)d[2];
	uint32_t src = FONT_GLYPHS;
	unsigned x_carry = 0;

	set_w(&d[3], 0x000C);
	addr = (uint16_t)((((base & 0x7FF) + 0x20)) << 5);
	for (int i = 0; i <= 0x5EF; i++) {
		w = ROM16(src);
		src += 2;
		d3 = 0xF000; d4 = bg_col; d7 = glyph_col;
		for (int n = 0; n < 4; n++) {              /* four 4-bit pixels, high nibble first */
			d6 = w & d3;
			if (d6 == 0) {
				w |= d4;                           /* background pixel */
			} else {
				w = (uint16_t)((w & ~d3) | d7);    /* glyph pixel */
			}
			d3 >>= 4; d4 >>= 4; d7 >>= 4;
		}
		vram_poke(ctx, addr, w);
		x_carry = ((uint32_t)addr + 2) > 0xFFFF;
		addr += 2;
	}
	set_w(&d[0], addr);
	set_w(&d[1], w);
	set_w(&d[2], 0xFFFF);
	set_w(&d[3], d3);
	set_w(&d[4], d4);
	set_w(&d[5], 0xFFFF);
	set_w(&d[6], d6);
	set_w(&d[7], d7);
	ctx->a[0] = src;
	{
		uint32_t pair = ((uint32_t)bg_col << 16) | glyph_col;
		cc_logic32(ctx, pair);
		cc_set(ctx, CC_X, x_carry ? CC_X : 0);
	}
	ctx->cycles = 1021000;   /* original avg; capped by the layer when called with VBlank unmasked (scripts) */
}

/* $007950: draw the 32 x 19 cell window of the room at the camera (CAM_X, CAM_Y; multiples of
 * 8) into both scroll planes: plane B ($E000) from PLANEB_TMP, plane A ($C000) from PLANEA_TMP.
 * Map row 0 lands on plane row 2 (screen line 16); the planes are 64 x 32 and wrap.  Afterwards
 * the four scroll-column helpers ($00FC3A, $00FD5E, $00FE74, $00FF68) are run (68k).
 * Caller contract: all registers and flags are dead after the call ($007950 is only called
 * from room_load and from the screen-restore code at $0046xx, which restore all registers
 * with movem), so they are not reproduced (difftest runs with --ignore-regs/--ignore-flags). */
HLE_FUNC(0x007950, room_draw_view, 0)
{
	uint16_t width = RAM16(MAP_WIDTH);
	int32_t row_skip = (int32_t)(uint16_t)(width * 2) - 0x40;  /* bytes to the next map row */
	uint16_t tx = RAM16(CAM_X) >> 3;
	uint16_t ty = RAM16(CAM_Y) >> 3;
	uint16_t cell_ofs = (uint16_t)(2 * tx + (uint16_t)((uint32_t)(uint16_t)(2 * ty) * width));
	uint16_t row_base = (uint16_t)(((ty + 2) & 0xFFFF) << 7);   /* plane row (ty+2) * 128 bytes */
	uint16_t col_base = (uint16_t)(2 * tx);

	for (int plane = 0; plane < 2; plane++) {
		uint32_t src = (plane == 0 ? PLANEB_TMP : PLANEA_TMP) + (int16_t)cell_ofs;
		uint16_t vram_base = plane == 0 ? 0xE000 : 0xC000;
		uint16_t row = row_base;
		for (int y = 0; y < 19; y++) {
			uint16_t col = col_base;
			row &= 0xFFF;
			for (int x = 0; x < 32; x++) {
				uint16_t a = (uint16_t)(row + (col & 0x7F) + vram_base);
				vram_poke(ctx, a, RAM16(src));
				src += 2;
				col += 2;
			}
			src += row_skip;
			row += 0x80;
		}
	}
	hle_call68k(ctx, 0x00FC3A);
	hle_call68k(ctx, 0x00FD5E);
	hle_call68k(ctx, 0x00FE74);
	hle_call68k(ctx, 0x00FF68);
	ctx->cycles = charge_total(380000);
}

/* $007A8C: (re)initialise the per-room actor slots after a load.
 * Walks the object table; every object flagged "actor" (bit1) that is in the current room and
 * whose type id is found in the actor definition table ($032670, 36 entries) takes the next
 * free actor slot 2..5 (slots 0/1 are Shaggy and Scooby):
 *   FF0478[k]  = pointer to the definition          FF051C[k] = tile data size (bytes)
 *   FF0530[k]  = size / 2 (words)                   FF04E4/FF04FC[k] = x/y position words
 *   FF048C[k]  = overlay word of the object         FF05E0[k], FF05EC[k] = x, y again
 * and sets bit k of the slot masks FF0AB4 (present), FF0AB5 (needs sprite load), FF0AB8
 * (flag bit3 of the object).  The object's byte +$19 receives the slot number (k - 2), $FF if
 * it did not get one.
 * Caller contract: registers and flags are dead on return (only caller is room_load, which
 * ends with rts and whose caller restores everything). */
HLE_FUNC(0x007A8C, room_setup_actors, 0)
{
	RAM8(0xFF0ABF) &= 0x03;
	RAM8(0xFF0AC3) = 0;
	RAM8(0xFF0AC0) = 0;
	RAM16(0xFF0688) = 0x64;
	RAM8(FLAGS_09DE) &= ~0x20;
	for (int i = 0; i < 6; i++) {               /* two parallel "none" lists */
		RAM16(0xFF050C + 2 * i) = 0xFFFF;
		RAM16(0xFF0618 + 2 * i) = 0xFFFF;
	}
	RAM8(0xFF0ACC) = 0;
	RAM8(0xFF0AB7) &= 0xFC;
	RAM16(0xFF0686) = 0x12C;
	RAM16(0xFF0488) = 4 + RAM16(0xFF05B8);       /* ram_cur_index: Shaggy animation set */
	if (!(RAM8(FLAGS_09DE) & 0x80))
		RAM16(0xFF048A) = 4 + RAM16(0xFF05B8);   /* Scooby's */
	RAM8(0xFF0ABE) = 0;

	uint8_t present = 3, load = 3, flag3 = 3;
	int slot = 2;
	uint32_t o = OBJECTS;
	uint32_t ptr_out = 0xFF0478, size_out = 0xFF051C, ovl_out = 0xFF048C;
	uint32_t x_out = 0xFF04E4;
	uint16_t n = RAM16(OBJ_LAST);
	for (uint32_t i = 0; i <= n; i++, o += OBJ_SIZE) {
		if (!(RAM8(o + OBJ_FLAGS) & 0x02))
			continue;
		RAM8(o + OBJ_SLOT) = 0xFF;
		if (RAM16(o + OBJ_ROOM) != RAM16(ROOM_INDEX))
			continue;
		uint16_t id = RAM16(o + OBJ_TYPE);
		int k;
		for (k = 0; k < 36; k++)
			if (ROM16(ROM32(ACTOR_DEF_PTRS + 4 * k)) == id)
				break;
		if (k == 36)
			continue;
		present |= 1 << slot;
		if (RAM8(o + OBJ_FLAGS) & 0x08)
			flag3 |= 1 << slot;
		uint32_t sz = ROM32(ACTOR_DEF_SIZES + 4 * k);
		RAM32_W(size_out, sz);
		RAM32_W(size_out + 0x14, sz >> 1);
		size_out += 4;
		load |= 1 << slot;
		RAM8(o + OBJ_SLOT) = (uint8_t)(slot - 2);
		RAM16(x_out) = RAM16(o + OBJ_X);
		RAM16(x_out + 0x18) = RAM16(o + OBJ_Y);
		RAM16(ovl_out + 0x154) = RAM16(o + OBJ_X);
		RAM16(ovl_out + 0x160) = RAM16(o + OBJ_Y);
		x_out += 4;
		RAM16(ovl_out) = RAM16(o + OBJ_OVERLAY);
		ovl_out += 2;
		RAM32_W(ptr_out, ROM32(ACTOR_DEF_PTRS + 4 * k));
		ptr_out += 4;
		if (++slot >= 6)
			break;
	}
	RAM8(0xFF0AB4) = present;
	RAM8(0xFF0AB5) = load;
	RAM8(0xFF0AB8) = flag3;
	RAM8(0xFF0AB6) = 0;
	RAM8(0xFF0AB9) = 0;
	if (RAM8(FLAGS_09DE) & 0x80)
		RAM8(0xFF0ABF) |= 0x02;
	ctx->cycles = 10500;
}

/* $00758A: load the current room (RAM ROOM_INDEX of mystery MYSTERY_HDR) into VRAM / RAM.
 *
 *  1. clear all 64 KiB of VRAM, reset camera and scroll, stage the shared UI palettes
 *  2. mark objects that are in this room (object flag bit0)
 *  3. copy the room's palettes 2-3 to FF07B8
 *  4. unpack the room tiles to VRAM $0000, then the 94-tile text font right behind them,
 *     the UI font, three raw UI tile blocks and a solid tile; remember all tile bases
 *  5. fill the window plane with the blank tile, write the verb bar names
 *  6. refresh the portrait/inventory VRAM areas ($005D1C, $005CB6, optionally $0047AC)
 *  7. unpack the map pack into the RAM working copies: plane A, plane B, zone map
 *  8. locate the post-zone tables (per row data, entry points, spawn list, waypoints)
 *  9. place Shaggy at entry point ROOM_ENTRY_OFS/4 (unless 09DE bit3), pick the camera,
 *     draw the window, set up the actors
 *
 * Caller contract: the only caller ($002138) saves and restores every register around the
 * call and loads SR itself, so register and flag results are not reproduced
 * (difftest: --ignore-regs d0-d7,a0-a6 --ignore-flags). */
HLE_FUNC(0x00758A, room_load, 0)
{
	/* 1 */
	vram_fill(ctx, 0, 0x8000, 0);
	RAM16(CAM_X) = 0;
	RAM16(CAM_Y) = 0;
	RAM32_W(0xFF07FC, 0);            /* vscroll (2 words) */
	RAM32_W(0xFF07F8, 0);            /* hscroll A/B */
	for (int i = 0; i < 32; i++)
		RAM16(0xFF0778 + 2 * i) = ROM16(SHARED_PALETTES + 2 * i);

	/* 2 */
	uint16_t room = RAM16(ROOM_INDEX);
	for (uint32_t i = 0, o = OBJECTS; i <= RAM16(OBJ_LAST); i++, o += OBJ_SIZE) {
		if (RAM16(o + OBJ_ROOM) == room)
			RAM8(o + OBJ_FLAGS) |= 0x01;
		else
			RAM8(o + OBJ_FLAGS) &= ~0x01;
	}

	/* 3 */
	uint32_t hdr = RAM32(MYSTERY_HDR);
	uint16_t idx = (uint16_t)(room - 2);
	uint32_t pal = ROM32(hdr + HDR_PALETTES) + ((uint32_t)(int32_t)(int16_t)idx << 7) + 0x40;
	for (int i = 0; i < 32; i++)
		RAM16(0xFF07B8 + 2 * i) = ROM16(pal + 2 * i);
	RAM8(FLAGS_09DE) &= ~0x40;
	RAM8(0xFF0ACD) = 0;

	/* 4: room tiles at VRAM 0 */
	uint32_t entry = ROM32(hdr + HDR_ROOMS) + (uint32_t)idx * ENT_SIZE;
	uint32_t tiles = ROM32(entry + ENT_TILES) + ROM32(hdr + HDR_TILES_BASE);
	lzss(ctx, tiles + 4, DECOMP_BUF);
	uint32_t tile_bytes = RAM32(LZSS_RING);
	if ((int32_t)tile_bytes < 0x400) {
		tile_bytes = 0x400;
		RAM32_W(LZSS_RING, tile_bytes);
	}
	vram_copy(ctx, 0, DECOMP_BUF, (uint16_t)tile_bytes >> 1);

	/* text font: 94 tiles right after the room tiles */
	uint16_t vram = (uint16_t)tile_bytes;
	uint16_t font_base = (uint16_t)(((vram >> 5) - 0x20) | 0x8000);
	RAM16(FONT_BASE) = font_base;
	ctx->d[0] = 0xF; ctx->d[1] = 1; ctx->d[2] = font_base;
	font_tiles_load(ctx);
	RAM16(0xFFFFFE) = (uint16_t)ctx->d[0];     /* dead store of the end address (kept: RAM is compared) */
	vram += 0xBE0;
	uint16_t base_e = (uint16_t)((vram >> 5) | 0x8000);
	RAM16(0xFF0802) = base_e;                  /* ram_ui_tile_base_e: UI font tile base */

	/* UI font (LZSS) */
	lzss(ctx, UI_FONT_LZSS, DECOMP_BUF);
	uint16_t ui_words = (uint16_t)(RAM32(LZSS_RING) >> 1);
	vram_copy(ctx, vram, DECOMP_BUF, ui_words);
	vram += 2 * ui_words;

	/* raw UI tile blocks.  Note: block B is 0xC0 words but the running address advances by only
	 * 0xC0 bytes, so block C overwrites the second half of block B (kept as is). */
	vram_copy(ctx, vram, UI_TILES_A, 0x40);
	RAM16(0xFF06E4) = (uint16_t)((vram >> 5) | 0x8000);   /* ui_tile_base_b */
	vram += 0x80;
	RAM16(0xFF06E8) = vram >> 5;                          /* ui_tile_base_d */
	vram_copy(ctx, vram, UI_TILES_B, 0xC0);
	vram += 0xC0;
	RAM16(0xFF06E6) = vram >> 5;                          /* ui_tile_base_c */
	vram_copy(ctx, vram, UI_TILES_C, 0x80);
	vram += 0x100;
	RAM16(0xFF08B8) = (uint16_t)((vram >> 5) | 0x8000);   /* ui_tile_base_f: blank tile */
	vram_fill(ctx, vram, 0x10, 0x1111);                   /* one solid colour-1 tile */
	RAM16(0xFF068A) = (uint16_t)(((vram >> 5) + 1) | 0x8000);   /* ui_tile_base_a */

	/* 5: window plane ($D000) = blank tile; verb bar names ($AA80, 6 rows x 61 words) */
	vram_fill(ctx, 0xD000, 0x400, RAM16(0xFF08B8));
	uint16_t ui_e = RAM16(0xFF0802);
	uint16_t dst = 0xAA80;
	uint32_t vb = VERB_BAR_NAMES;
	for (int row = 0; row < 6; row++) {
		for (int col = 0; col < 61; col++) {
			vram_poke(ctx, dst, (uint16_t)(ROM16(vb) + ui_e));
			vb += 2;
			dst += 2;
		}
		vb += 6;
		vram_fill(ctx, dst, 3, (uint16_t)(ROM16(VERB_BAR_NAMES) + ui_e));
		dst += 6;
	}

	/* 6: portraits / inventory areas */
	hle_call68k(ctx, 0x005D1C);
	hle_call68k(ctx, 0x005CB6);
	if ((RAM8(GAME_STATE) & 0x02) || (RAM8(FLAGS_09DE) & 0x04)) {
		RAM8(VBLANK_FLAGS) ^= 0x10;
		hle_call68k(ctx, 0x0047AC);
		RAM8(VBLANK_FLAGS) ^= 0x10;
	}

	/* 7: names, map pack */
	RAM32_W(ROOM_NAME_PTR, ROM32(entry + 0x00) + ROM32(hdr + 0x20));
	uint32_t map = ROM32(entry + ENT_MAP) + ROM32(hdr + HDR_MAPS_BASE);
	RAM16(MAP_WIDTH) = ROM16(map);
	RAM16(MAP_HEIGHT) = ROM16(map + 2);
	uint32_t planeb_stream = map + 8;
	uint32_t planea_stream = planeb_stream + ROM32(planeb_stream - 4) + 4;
	lzss(ctx, planea_stream, PLANEA_TMP);
	/* The original copies (LZSS ring address)/2 words, i.e. a constant 0x1800 words, from the
	 * temporary plane A to the pristine copy -- not the unpacked size.  Kept. */
	for (uint32_t i = 0; i < (LZSS_RING & 0xFFFF) / 2; i++)
		RAM16(PLANEA_NAMES + 2 * i) = RAM16(PLANEA_TMP + 2 * i);
	lzss(ctx, planeb_stream, PLANEB_TMP);
	for (uint32_t i = 0; i < (uint16_t)(RAM32(LZSS_RING) >> 1); i++)
		RAM16(PLANEB_NAMES + 2 * i) = RAM16(PLANEB_TMP + 2 * i);
	uint32_t zone_stream = planea_stream + ROM32(planea_stream - 4) + 4;
	lzss(ctx, zone_stream, ZONE_MAP);

	/* 8: tables behind the zone map */
	uint32_t post = zone_stream + ROM32(zone_stream - 4);
	RAM32_W(ROOM_DATA_PTR, post);
	uint32_t entries = post + (uint32_t)(int16_t)(uint16_t)(2 * RAM16(MAP_HEIGHT));
	RAM32_W(ROOM_ENTRY_TBL, entries);
	uint32_t spawns = entries + 0x20;
	RAM32_W(ROOM_SPAWN_TBL, spawns);
	uint16_t nspawn = 0;
	while (nspawn != 8) {
		uint32_t v = ROM32(spawns + 4 * nspawn);
		if (v == 0xFFFFFFFF)
			break;
		nspawn++;
	}
	RAM16(ROOM_SPAWN_COUNT) = nspawn;

	/* 9: Shaggy's start, camera, first draw */
	uint32_t ent = entries + (uint32_t)(int32_t)(int16_t)RAM16(ROOM_ENTRY_OFS);
	if (!(RAM8(FLAGS_09DE) & 0x08)) {
		uint16_t x = (uint16_t)(ROM16(ent) << 3), y = (uint16_t)(ROM16(ent + 2) << 3);
		RAM16(SHAGGY_X) = x;
		RAM16(SHAGGY_X + 4) = x;       /* ram_shaggy_x_prev ($FF04E0): Scooby's slot x */
		RAM16(SHAGGY_Y) = y;
		RAM16(SHAGGY_Y + 4) = (uint16_t)(y - 1);
	}
	int16_t cx = (int16_t)((RAM16(SHAGGY_X) >> 3) - 0x10);
	int16_t cy = (int16_t)((RAM16(SHAGGY_Y) >> 3) - 0x0A);
	if (cx < 0) cx = 0;
	if (cy < 0) cy = 0;
	/* clamp so the 32 x 19 window stays inside the map */
	if ((int16_t)(cx + 0x20 - (int16_t)RAM16(MAP_WIDTH)) > 0)
		cx = (int16_t)(RAM16(MAP_WIDTH) - 0x20);
	if ((int16_t)(cy + 0x13 - (int16_t)RAM16(MAP_HEIGHT)) > 0)
		cy = (int16_t)(RAM16(MAP_HEIGHT) - 0x13);
	RAM16(CAM_X) = (uint16_t)(cx << 3);
	RAM16(CAM_Y) = (uint16_t)(cy << 3);
	room_draw_view(ctx);
	room_setup_actors(ctx);
	/* original avg 8.8M cycles, with interrupts masked by the caller ($002138): charged in
	 * full (the original loses those VBlanks too) */
	ctx->cycles = charge_total(8800000);
}

/* $007F22: copy the initial game-state block of mystery MYSTERY_IDX into FF2A00 (the first 256
 * bytes are zeroed, then hdr[0]..hdr[4] is copied) and fetch the start room into ROOM_INDEX.
 * Registers/flags left by the original are dead for its only caller (only caller: $008FCA, mystery start). */
HLE_FUNC(0x007F22, game_state_init, 0)
{
	uint32_t hdr = ROM32(0x031AF2 + 4 * RAM16(MYSTERY_IDX));
	for (int i = 0; i < 0x80; i++)
		RAM16(GAME_STATE + 2 * i) = 0;
	uint32_t src = ROM32(hdr + HDR_STATE_START), end = ROM32(hdr + 4);
	uint32_t dst = GAME_STATE;
	while (src != end)
		RAM8(dst++) = ROM8(src++);
	RAM16(ROOM_INDEX) = ROM16(ROM32(hdr + HDR_START_ROOM));
	ctx->cycles = 5000;
}

/* $007F66: build the object table from the object init records of mystery MYSTERY_IDX.
 * Record in ROM (14 bytes, 18 if flags bit9 "has position"): long -> +0, long -> +4,
 * word -> +8, word -> +$16, word -> +$18 (high byte = flag byte, low byte replaced by $FF),
 * [long -> +$12].  Objects whose room is 1 (inventory) are listed (by index) in FF0A1E.
 * Then OBJ_LAST, the start room/music and the start script are read from hdr+$30 and the
 * routine continues in 68k at $00800A (intro + sound stop), see the end of the function.
 * Not difftestable (the intro never returns in the sandbox): port/tools/dt-room compares the
 * table built at boot between pure emulation and this function. */
HLE_FUNC(0x007F66, object_table_init, 0)
{
	uint32_t hdr = ROM32(0x031AF2 + 4 * RAM16(MYSTERY_IDX));
	uint32_t src = ROM32(hdr + HDR_OBJECTS), end = ROM32(hdr + HDR_ROOMS);
	uint32_t o = OBJECTS, inv = 0xFF0A1E;
	uint16_t idx = 0xFFFF;
	RAM32_W(MYSTERY_HDR, hdr);
	do {
		for (int i = 0; i < OBJ_SIZE; i++)
			RAM8(o + i) = 0;
		RAM32_W(o, ROM32(src));
		RAM32_W(o + 4, ROM32(src + 4));
		RAM16(o + 8) = ROM16(src + 8);
		RAM16(o + 0x16) = ROM16(src + 10);
		RAM16(o + 0x18) = ROM16(src + 12);
		src += 14;
		RAM16(o + OBJ_TIMER) = 0;
		RAM8(o + OBJ_SLOT) = 0xFF;
		if (RAM8(o + OBJ_FLAGS) & 0x02) {
			RAM32_W(o + OBJ_X, ROM32(src));
			src += 4;
		}
		if (RAM16(o + OBJ_ROOM) == 1) {
			RAM16(inv) = (uint16_t)(idx + 1);
			inv += 2;
		}
		idx++;
		o += OBJ_SIZE;
	} while (src != end);
	RAM16(inv) = 0xFFFF;
	RAM16(OBJ_LAST) = idx;

	uint32_t st = ROM32(hdr + HDR_START_ROOM);
	RAM16(ROOM_INDEX) = ROM16(st);
	RAM32_W(0xFF06A6, (uint32_t)(int32_t)(int16_t)ROM16(st + 2));   /* ram_cur_music */
	uint16_t len = ROM16(st + 4);
	ctx->d[0] = (uint32_t)(int32_t)(int16_t)ROM16(st + 2);
	set_w(&ctx->d[1], len);
	ctx->a[5] = st + 6;
	ctx->a[6] = st + 6 + (uint32_t)(int32_t)(int16_t)len;
	/* The rest of the original is `bsr $0013BA` (mystery intro: waits for VBlanks, so it cannot run
	 * inside a C function where interrupts never fire) followed by sound_stop($1E) and rts.  Resume the
	 * 68k code at that `bsr` ($00800A) with the registers it expects instead of calling it. */
	hle_jump(ctx, 0x00800A);
}

/* $00750A, $0073D6 and $007476 were ported in parallel by #16 (walk.c: pad_dir8,
 * walk_blocked_probe, walk_blocked_line_probe), which are the registered replacements. These
 * independent versions are kept for reference only. */
#if 0
/* $00750A: map the D-pad (pad 1, active low) to a direction index in D0:
 *   R+D 0, L+U 1, R+U 2, L+D 3, L 4, R 5, U 6, D 7; no direction pressed leaves D0 unchanged.
 * CCR: after `moveq` N=0, Z=(result==0), V=C=0, X unchanged; in the "none pressed" case only
 * Z=0 (from the last `btst`) changes. */
HLE_FUNC(0x00750A, pad_direction, 114)
{
	uint8_t pad = RAM8(0xFF09E0);
	int up = !(pad & 0x01), down = !(pad & 0x02), left = !(pad & 0x04), right = !(pad & 0x08);
	int dir = -1;
	if (left)       dir = up ? 1 : down ? 3 : 4;
	else if (right) dir = up ? 2 : down ? 0 : 5;
	else if (up)    dir = 6;
	else if (down)  dir = 7;
	if (dir < 0) {
		cc_set(ctx, CC_Z, 0);    /* last btst found D (bit1) set -> Z = 0 */
		return;
	}
	ctx->d[0] = (uint32_t)dir;
	cc_logic32(ctx, (uint32_t)dir);
}

/* ------------------------------------------------------------------------------------------
 * Zone-map collision probes.
 * The zone map holds one word per 8x8 cell: bit 15 = flag, bits 0-14 = index into the word table at
 * $02AF8E.  That word is { bit 12: flip y, bit 11: do NOT flip x, bits 0-10: collision mask tile }.
 * The mask tiles (4bpp, $02B0BA, 32 bytes each) say which pixels of the cell are solid, so
 * walkability is pixel exact.  The result is the masked nibble of the probed pixel (non-zero = solid).
 * Registers and flags of every exit are reproduced because callers branch on D5 and the CCR.
 * ------------------------------------------------------------------------------------------ */
#define ZONE_FLIP_TABLE 0x02AF8E
#define ZONE_MASK_TILES 0x02B0BA

static void cc_cmp16(hle_ctx *c, uint16_t dst, uint16_t src)   /* cmp.w src,dst */
{
	uint32_t r = (uint32_t)dst - src;
	unsigned n = (r >> 15) & 1, z = (uint16_t)r == 0;
	unsigned v = ((dst ^ src) & (dst ^ r) & 0x8000) != 0, cy = dst < src;
	cc_set(c, CC_N | CC_Z | CC_V | CC_C, (n ? CC_N : 0) | (z ? CC_Z : 0) | (v ? CC_V : 0) | (cy ? CC_C : 0));
}

static void zone_probe(hle_ctx *ctx, uint32_t xaddr, uint32_t yaddr, int bounded, uint16_t idx_mask)
{
	uint32_t *d = ctx->d;
	set_w(&d[0], RAM16(xaddr));
	set_w(&d[1], RAM16(yaddr));
	if ((int16_t)d[0] < 0) { cc_logic16(ctx, d[0]); return; }
	if ((int16_t)d[1] < 0) { cc_logic16(ctx, d[1]); return; }
	if (bounded) {
		cc_cmp16(ctx, d[2], d[0]);
		if ((int16_t)(uint16_t)d[2] < (int16_t)(uint16_t)d[0]) return;
		cc_cmp16(ctx, d[3], d[1]);
		if ((int16_t)(uint16_t)d[3] < (int16_t)(uint16_t)d[1]) return;
	}
	set_w(&d[0], (uint16_t)d[0] >> 3);
	set_w(&d[0], (uint16_t)d[0] * 2);
	set_w(&d[1], (uint16_t)d[1] >> 3);
	set_w(&d[4], RAM16(MAP_WIDTH));
	set_w(&d[4], (uint16_t)d[4] * 2);
	d[1] = (uint32_t)(uint16_t)d[1] * (uint16_t)d[4];                 /* mulu.w */
	unsigned x_carry = ((uint32_t)(uint16_t)d[0] + (uint16_t)d[1]) > 0xFFFF;
	set_w(&d[0], (uint16_t)d[0] + (uint16_t)d[1]);
	ctx->a[0] = ZONE_MAP;
	set_w(&d[0], RAM16(ZONE_MAP + (int16_t)(uint16_t)d[0]));
	set_w(&d[0], (uint16_t)d[0] & idx_mask);
	if ((uint16_t)d[0] == 0) {
		cc_logic16(ctx, 0);
		cc_set(ctx, CC_X, x_carry ? CC_X : 0);
		return;
	}
	set_w(&d[0], (uint16_t)d[0] * 2);
	ctx->a[0] = ZONE_FLIP_TABLE;
	set_w(&d[0], ROM16(ZONE_FLIP_TABLE + (int16_t)(uint16_t)d[0]));
	set_w(&d[5], d[0]);
	set_w(&d[0], (uint16_t)d[0] & 0x7FF);
	set_w(&d[0], (uint16_t)d[0] << 5);
	ctx->a[0] = ZONE_MASK_TILES + (uint32_t)(int32_t)(int16_t)(uint16_t)d[0];
	set_w(&d[0], RAM16(xaddr) & 7);
	set_w(&d[1], RAM16(yaddr) & 7);
	if (!(d[5] & 0x800)) set_w(&d[0], (uint16_t)d[0] ^ 7);   /* mirror x unless bit 11 is set */
	if (d[5] & 0x1000)   set_w(&d[1], (uint16_t)d[1] ^ 7);   /* mirror y when bit 12 is set */
	set_w(&d[1], (uint16_t)d[1] << 2);
	set_w(&d[0], (uint16_t)d[0] << 2);
	uint32_t row = ROM32(ctx->a[0] + (int16_t)(uint16_t)d[1]);
	d[5] = (0xFu << ((uint16_t)d[0] & 63)) & row;
	cc_logic32(ctx, d[5]);
	cc_set(ctx, CC_X, 0);
}

/* $0073D6: probe the pixel at the cursor (ram_cursor_x/y = $FF0642/$FF0646).  D2/D3 = largest
 * allowed x/y (pixels): outside 0..D2 / 0..D3 nothing is probed.  Zone index mask $7FFF. */
HLE_FUNC(0x0073D6, zone_probe_cursor, 248)
{
	zone_probe(ctx, 0xFF0642, 0xFF0646, 1, 0x7FFF);
}

/* $007476: same for the point ($FF064A, $FF064E) with no upper bound; zone index mask $07FF. */
HLE_FUNC(0x007476, zone_probe_point, 216)
{
	zone_probe(ctx, 0xFF064A, 0xFF064E, 0, 0x07FF);
}
#endif /* duplicates of walk.c */

/* ------------------------------------------------------------------------------------------
 * Tile overlays (objects that change the background: doors opening, items vanishing...).
 * ------------------------------------------------------------------------------------------ */
#define OVL_X 0x00      /* record fields (words), see docs/formats.md "Overlays and hotspots" */
#define OVL_Y 0x02
#define OVL_W 0x04
#define OVL_H 0x06
#define OVL_PLANE 0x0C
#define OVL_NAMES 0x0E
#define IO_FLAGS_09EB 0xFF09EB

static uint32_t overlay_record(uint16_t id)    /* id is 1-based */
{
	uint32_t hdr = RAM32(MYSTERY_HDR);
	/* subq.w #1,d0 / lsl.w #2,d0: the table index is a sign-extended 16-bit value */
	return ROM32(ROM32(hdr + 0x24) + (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)(id - 1) << 2)) + ROM32(hdr + 0x1C);
}

/* $007D54: show (D0.w = overlay id) or restore (bit 15 set) a tile overlay.
 * draw:    copy the overlay's names over the working plane copy ($FF7000/$FF8C00), then write the part
 *          that is inside the 33 x 21 cell window around the camera to the VRAM plane
 *          (plane A $C000 / B $E000, 64 x 32, wrapped).
 * restore: copy the pristine names ($FFA800/$FFC400) back and redraw the same way.
 * With bit 0 of $FF09EB set (screens whose planes are 32 cells wide) the rows are DMA'd instead
 * ($007C82 in the original).  All registers are preserved.  Flags are not reproduced: the only caller,
 * objects_update, overwrites them (difftest --ignore-flags). */
HLE_FUNC(0x007D54, overlay_apply, 0)
{
	uint32_t sd[8], sa[7];
	memcpy(sd, ctx->d, sizeof sd);
	memcpy(sa, ctx->a, sizeof sa);
	int16_t id = (int16_t)(uint16_t)ctx->d[0];
	hle_ctx *c = ctx;

	if (RAM8(IO_FLAGS_09EB) & 1) {
		/* DMA variant: the original saves only d0-d3/a0-a1 */
		uint32_t s03[4] = { c->d[0], c->d[1], c->d[2], c->d[3] }, sa01[2] = { c->a[0], c->a[1] };
		if (id > 0) {
			uint32_t rec = overlay_record((uint16_t)id);
			uint16_t addr = (uint16_t)(2 * ROM16(rec + OVL_X) + (ROM16(rec + OVL_Y) << 7));
			addr += ROM16(rec + OVL_PLANE) ? 0xC100 : 0xE100;
			set_w(&c->d[1], ROM16(rec + OVL_W));
			set_w(&c->d[2], ROM16(rec + OVL_H) - 1);
			c->a[0] = rec + OVL_NAMES;
			set_w(&c->d[0], addr);
			do {
				hle_call68k(c, 0x0099F6);
				set_w(&c->d[0], (uint16_t)c->d[0] + 0x80);
				set_w(&c->d[2], (uint16_t)c->d[2] - 1);
			} while ((uint16_t)c->d[2] != 0xFFFF);
		} else if (id < 0) {
			uint32_t rec = overlay_record((uint16_t)(id & 0x7FFF));
			uint16_t d1 = (uint16_t)(ROM16(rec + OVL_Y) << 6);
			uint16_t d0 = (uint16_t)(2 * ROM16(rec + OVL_X) + d1);
			uint16_t d2 = d0;
			d0 += d1;
			uint32_t src;
			if (ROM16(rec + OVL_PLANE) == 0) { d0 += 0xE100; src = PLANEB_NAMES; }
			else                              { d0 += 0xC100; src = PLANEA_NAMES; }
			src += (int16_t)d2;
			uint16_t w = ROM16(rec + OVL_W);
			uint16_t rows = ROM16(rec + OVL_H);
			uint16_t skip = (uint16_t)(2 * (0x20 - w));
			c->a[1] = rec;
			set_w(&c->d[1], w);
			set_w(&c->d[3], skip);
			set_w(&c->d[2], rows - 1);
			set_w(&c->d[0], d0);
			c->a[0] = src;
			do {
				hle_call68k(c, 0x0099F6);
				set_w(&c->d[0], (uint16_t)c->d[0] + 0x80);
				c->a[0] += (int16_t)(uint16_t)c->d[3];
				set_w(&c->d[2], (uint16_t)c->d[2] - 1);
			} while ((uint16_t)c->d[2] != 0xFFFF);
		}
		for (int i = 0; i < 4; i++) c->d[i] = s03[i];
		c->a[0] = sa01[0]; c->a[1] = sa01[1];
		ctx->cycles = 7400;
		return;
	}

	if (id != 0) {
		int restore = id < 0;
		uint32_t rec = overlay_record((uint16_t)(id & 0x7FFF));
		uint16_t width2 = (uint16_t)(2 * RAM16(MAP_WIDTH));
		uint16_t w = ROM16(rec + OVL_W), h = ROM16(rec + OVL_H);
		uint16_t ox = ROM16(rec + OVL_X), oy = ROM16(rec + OVL_Y);
		int plane_a = ROM16(rec + OVL_PLANE) != 0;
		uint16_t ofs = (uint16_t)((uint32_t)oy * width2 + 2 * ox);
		uint32_t sofs = (uint32_t)(int32_t)(int16_t)ofs;
		uint32_t dst = (plane_a ? PLANEA_TMP : PLANEB_TMP) + sofs;
		uint32_t src = restore ? (plane_a ? PLANEA_NAMES : PLANEB_NAMES) + sofs : rec + OVL_NAMES;
		uint16_t skip = (uint16_t)(width2 - 2 * w);
		uint32_t row0 = dst;
		for (uint16_t y = 0; y < h; y++) {
			for (uint16_t x = 0; x < w; x++) {
				RAM16(dst) = restore ? RAM16(src) : ROM16(src);
				dst += 2; src += 2;
			}
			dst += (int16_t)skip;
			if (restore) src += (int16_t)skip;
		}
		/* redraw the visible part */
		int16_t camx = (int16_t)(RAM16(CAM_X) >> 3), camy = (int16_t)(RAM16(CAM_Y) >> 3);
		int16_t x0 = (int16_t)(ox - camx), x1 = (int16_t)(ox + w - camx);
		int16_t y0 = (int16_t)(oy - camy), y1 = (int16_t)(oy + h - camy);
		uint32_t p = row0;
		for (int16_t ry = y0; ry != y1; ry++) {
			for (int16_t rx = x0; rx != x1; rx++) {
				if (rx >= -1 && ry >= -1 && rx < 0x21 && ry < 0x15) {
					uint16_t a = (uint16_t)((((camy + ry) << 7) & 0xFFF) + (((camx + rx) * 2) & 0x7F));
					if (plane_a) { a += 0xC100; if (a >= 0xD000) a -= 0x1000; }
					else         { a += 0xE100; if (a >= 0xF000) a -= 0x1000; }
					vram_poke(c, a, RAM16(p));
				}
				p += 2;
			}
			p += (int16_t)skip;
		}
	}
	memcpy(ctx->d, sd, sizeof sd);
	memcpy(ctx->a, sa, sizeof sa);
	ctx->cycles = 7400;
}

/* $007C04: per-frame object update (called from the VBlank-driven main loop, flag bit 2 of
 * $FF09DC).  For every object: count its timer down (flag bit2 = "fired" once it passes zero),
 * and apply the overlay of every non-actor object of the current room whose bit0 flag is set
 * (set by room_load or by scripts that change an object).  Overlay word: bit 15 = restore the
 * background, bit 14 = clear the overlay word afterwards.
 * Registers: D5.w = $FFFF, D6.w = room, A3 = end of the table and D0.w = overlay word of the last
 * object processed are left behind exactly as the original does, because vblank_main goes on to call
 * $005E4A without reloading them.  Flags are not reproduced (dt_ignores). */
HLE_FUNC(0x007C04, objects_update, 0)
{
	uint16_t room = RAM16(ROOM_INDEX);
	uint32_t o = OBJECTS;
	for (uint32_t n = (uint32_t)RAM16(OBJ_LAST) + 1; n; n--, o += OBJ_SIZE) {
		if ((int16_t)RAM16(o + OBJ_TIMER) >= 0) {
			RAM16(o + OBJ_TIMER)--;
			if ((int16_t)RAM16(o + OBJ_TIMER) < 0)
				RAM8(o + OBJ_FLAGS) |= 0x04;
			else
				RAM8(o + OBJ_FLAGS) &= ~0x04;
		}
		uint8_t fl = RAM8(o + OBJ_FLAGS);
		if (!(fl & 0x01) || RAM16(o + OBJ_ROOM) != room || (fl & 0x02))
			continue;
		uint16_t ovl = RAM16(o + OBJ_OVERLAY) & 0xBFFF;
		set_w(&ctx->d[0], ovl);
		overlay_apply(ctx);
		if (ovl & 0x8000) {
			if (RAM8(o + OBJ_OVERLAY) & 0x40)
				RAM16(o + OBJ_OVERLAY) = 0;
			RAM16(o + OBJ_OVERLAY) &= 0x7FFF;
		}
		RAM8(o + OBJ_FLAGS) &= ~0x01;
	}
	set_w(&ctx->d[5], 0xFFFF);
	set_w(&ctx->d[6], room);
	ctx->a[3] = o;
	ctx->cycles = 14500;
}
