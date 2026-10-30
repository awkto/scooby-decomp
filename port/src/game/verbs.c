/* Verb bar and inventory UI (docs/subsystems/verbs.md).
 *
 * The bottom 56 lines of the screen are a plane-A "verb bar" at VRAM $A000 (rows 21-27 of a
 * 64x32 plane), shown by the HBlank hook hblank_verb_bar ($01006C).  Pressing C slides it left/
 * right (16 px per frame, ram_hscroll_verb) between the ten verbs and the inventory page, which is
 * 8 item icons (4 per row, 2 rows) plus page-up / page-down arrows.
 *
 * Functions here:
 *   $005B5E object_name_ptr       object id -> pointer to its name string
 *   $005BDC verb_label_redraw     redraw the 4x2 tile block of one verb in a given colour variant
 *   $005D1C inventory_build_page  build the inventory name table + icon tiles for the page in RAM
 */
#include "game.h"

/* ---- RAM ($FF0000 work RAM) ---- */
#define RAM_MYSTERY_HDR     0xFF068E   /* long: active mystery header (ROM pointer) */
#define RAM_OBJECTS         0xFF1200   /* object table, 26 ($1A) byte records */
#define RAM_VERB_CUR        0xFF06C2   /* word: verb id whose label verb_label_redraw redraws */
#define RAM_INV_PAGE        0xFF06EC   /* word: current inventory page (0-based, 4 items / page) */
#define RAM_UI_TILE_BASE_E  0xFF0802   /* word: first VRAM tile index of the verb-bar font/art */
#define RAM_INV_LIST        0xFF0A1E   /* words: object indices carried, terminated by a negative word */
#define RAM_INV_NAMETABLE   0xFF0328   /* 128 bytes: inventory plane-A name table (2 x 3 rows) */
#define RAM_INV_ICON_TILES  0xFF2C00   /* 8 icons x 384 bytes: 4x3 tiles of 4bpp, DMA'd to VRAM $B000 */

/* ---- ROM ---- */
#define ROM_VERB_IDS        0x02FE3C   /* 10 words: verb ids in bar order (TAKE LOOK OPEN PUSH EAT GIVE TALK SHUT PULL USE) */
#define ROM_NAME_SHAGGY     0x02FE50   /* "Shaggy" */
#define ROM_NAME_SCOOBY     0x02FE57   /* "Scooby" */
#define ROM_VERB_BAR_NAMES  0x030016   /* 3 variants x 6 rows x 64 words of verb-bar name table */

/* --- tiny helpers for the flag effects 68k callers could observe --- */
static void flags_sub16(hle_ctx *c, uint16_t dst, uint16_t src)
{
	uint16_t r = (uint16_t)(dst - src);
	unsigned cc = 0;
	if (r & 0x8000) cc |= CC_N;
	if (!r) cc |= CC_Z;
	if (((dst ^ src) & (dst ^ r)) & 0x8000) cc |= CC_V;
	if (src > dst) cc |= CC_C | CC_X;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, cc);
}
static void flags_add16(hle_ctx *c, uint16_t dst, uint16_t src)
{
	uint32_t w = (uint32_t)dst + src;
	uint16_t r = (uint16_t)w;
	unsigned cc = 0;
	if (r & 0x8000) cc |= CC_N;
	if (!r) cc |= CC_Z;
	if ((~(dst ^ src) & (dst ^ r)) & 0x8000) cc |= CC_V;
	if (w > 0xFFFF) cc |= CC_C | CC_X;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, cc);
}

/* $005B5E: D0.w = object id -> A0 = pointer to the object's name string.
 *   id 0: nothing (A0 = start of the name pool, header+$20); id 1 = "Shaggy"; id 2 = "Scooby";
 *   id n>=3: object n-3 of the room data: A0 = name pool + offset stored at (header+$28 table)[n-3].
 * Clobbers A0, A1 (= header+$28 table), and D0 for ids >= 3.  CCR is that of the last
 * subq/lsl/move executed. */
HLE_FUNC(0x005B5E, object_name_ptr, 40)
{
	uint32_t hdr = RAM32(RAM_MYSTERY_HDR);
	ctx->a[1] = bus_r32(hdr + 0x28);
	ctx->a[0] = bus_r32(hdr + 0x20);
	uint16_t id = (uint16_t)ctx->d[0];
	uint16_t d = (uint16_t)(id - 1);
	flags_sub16(ctx, id, 1);
	set_w(&ctx->d[0], d);
	if (id == 0)
		return;                           /* id 0: bmi -> rts */
	if (d == 0) {                             /* id 1 */
		ctx->a[0] = ROM_NAME_SHAGGY;
		return;
	}
	flags_sub16(ctx, d, 1);
	d--;
	set_w(&ctx->d[0], d);
	if (d == 0) {                             /* id 2 */
		ctx->a[0] = ROM_NAME_SCOOBY;
		return;
	}
	flags_sub16(ctx, d, 1);
	d--;
	/* lsl.w #3,d0 : X=C = bit 13 of d, V=0 */
	{
		uint32_t w = (uint32_t)d << 3;
		uint16_t r = (uint16_t)w;
		cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
		       ((w >> 16) & 1 ? (CC_C | CC_X) : 0) | ((r & 0x8000) ? CC_N : 0) | (r ? 0 : CC_Z));
		set_w(&ctx->d[0], r);
	}
	/* move.l (4,a1,d0.w),d0 : offset of the name inside the name pool (8-byte records) */
	uint32_t off = bus_r32(ctx->a[1] + 4 + (int16_t)ctx->d[0]);
	ctx->d[0] = off;
	cc_logic32(ctx, off);
	ctx->a[0] += off;
}

/* $005BDC: redraw the label of the verb whose id is in ram_verb_cur ($FF06C2), in colour variant
 * D0.w (0 = normal, 1 = highlighted/selected, 2 = hover/default verb).
 *
 * The verb bar's name table in ROM ($030016) has three variants of 6 rows x 64 words, identical
 * except for the palette bits.  A verb's label is a 4x2 block of tiles; verbs are laid out 5 tiles
 * (40 px) apart: ids in bar order -> column 0..4 of row 0 or 1.  The block starts at plane tile
 * (col 2 + 5*i, row 22 + 2*row); its VRAM address is $AA80+$84+10*i (+$100 for the second row).
 * Does nothing when ram_verb_cur is 0.
 *
 * Original register usage preserved: D2/D3 saved; D0 = address after the last word, D1.w = last
 * tile value, A0 = ROM source after the last row. */
HLE_FUNC(0x005BDC, verb_label_redraw, 0)
{
	uint16_t verb = RAM16(RAM_VERB_CUR);
	cc_logic16(ctx, verb);
	if (verb == 0) {
		ctx->cycles = 24;
		return;
	}
	uint16_t variant = (uint16_t)ctx->d[0];
	uint32_t variant_off = (uint32_t)variant * 0x300;      /* mulu.w #$300 */
	uint16_t variant_w = (uint16_t)variant_off;

	/* slot index of this verb id in the bar (the original loops without a bound) */
	uint32_t a = ROM_VERB_IDS;
	uint16_t slot = 0;
	while (bus_r16(a) != verb) { a += 2; slot++; }
	a += 2;                                           /* (a0)+ after the matching compare */

	uint16_t addr = 0x84;                             /* word offset of the first label in the table */
	if (slot > 4) {                                   /* second row of verbs: 2 tile rows lower */
		addr += 0x100;
		slot -= 5;
	}
	uint16_t col_off = (uint16_t)(slot * 10);
	addr += col_off;
	uint16_t src_off = (uint16_t)(addr + variant_w);  /* offset of the label inside the variant */
	uint32_t src = ROM_VERB_BAR_NAMES + (int16_t)src_off;
	uint16_t vram = (uint16_t)(addr + 0xAA80);

	uint32_t save_d2 = ctx->d[2], save_d3 = ctx->d[3];
	uint16_t tile_base = RAM16(RAM_UI_TILE_BASE_E);
	uint16_t d1 = 0;
	for (int row = 0; row < 2; row++) {
		for (int col = 0; col < 4; col++) {
			d1 = (uint16_t)(bus_r16(src) + tile_base);
			src += 2;
			set_w(&ctx->d[0], vram);
			set_w(&ctx->d[1], d1);
			vram_write_word(ctx);                     /* d0 = VRAM address, d1 = value */
			vram = (uint16_t)(vram + 2);
			flags_add16(ctx, (uint16_t)(vram - 2), 2);
		}
		flags_add16(ctx, vram, 0x78);
		vram = (uint16_t)(vram + 0x78);               /* next tile row: 128 bytes minus the 8 written */
		src += 0x78;
	}
	ctx->d[0] = vram;                                 /* upper word was cleared by moveq/mulu */
	set_w(&ctx->d[1], d1);
	ctx->d[2] = (uint32_t)(int32_t)(int16_t)save_d2;   /* movem.w d2-d3 restores sign-extended */
	ctx->d[3] = (uint32_t)(int32_t)(int16_t)save_d3;
	ctx->a[0] = src;
	ctx->cycles = 600;
}

/* $005D1C: build the inventory page for ram_inv_page ($FF06EC).
 *
 * The carried objects are the word list at $FF0A1E (object indices, ends at a negative word).
 * The screen shows 8 slots (two rows of four) starting at list entry 4*page, so paging moves by
 * ONE ROW (4 entries), not by a whole screen.  Each slot copies the object's 4x3-tile icon
 * (header+$18 table, $182 bytes per icon: a palette word, then 384 bytes of tiles) into the buffer
 * at $FF2C00, and writes a 4x3 block of name-table words (tile index $580 + 12*slot + 3*col + row,
 * priority bit set, OR the icon's palette word) into the 128-byte table at $FF0328 (10 bytes per
 * slot column; second slot row +$78).  Unused slots get $8000 (blank tile, priority) so old icons
 * disappear.  The caller then uploads both (sub_005CB6 / sub_005CF6).  Clamps ram_inv_page to the
 * last valid page.
 *
 * The last word of each icon block, "icon id" at object+4, is 0 for objects that have no icon;
 * such list entries consume a slot but draw nothing (original behaviour: the slot keeps its old
 * contents).  Unused slots get $8000 (the blank tile with priority).
*/
HLE_FUNC(0x005D1C, inventory_build_page, 3000)
{
	/* The original keeps everything in registers and callers see the leftovers, so the locals
	 * below mirror D0-D5 / A0-A4 (word operations keep the upper half, like the 68k). */
	uint32_t d0 = 0, d1 = ctx->d[1], d2 = 8, d3 = ctx->d[3], d4 = ctx->d[4], d5 = ctx->d[5];
	uint32_t list = RAM_INV_LIST;                  /* a0: walks the carried-object list */
	uint32_t a1 = list, a2 = ctx->a[2], a3 = ctx->a[3], a4 = ctx->a[4];
	uint32_t table = RAM_INV_NAMETABLE;            /* a5 */

	/* count the carried objects (list ends at a negative word) */
	while ((int16_t)RAM16(a1) >= 0) { a1 += 2; d0 = (d0 + 1) & 0xFFFF; }
	a1 += 2;
	set_w(&d1, d0);
	int build = (uint16_t)d1 != 0;

	if (build) {
		uint16_t last_page = (uint16_t)(((uint16_t)d0 - 1) >> 2);
		d0 = last_page;
		if ((int16_t)last_page < (int16_t)RAM16(RAM_INV_PAGE))
			RAM16(RAM_INV_PAGE) = last_page;       /* clamp the page to the last valid one */
		set_w(&d0, d1);
		set_w(&d0, (uint16_t)d0 - 1);
		set_w(&d1, (uint16_t)(RAM16(RAM_INV_PAGE) << 2));   /* list index of the page's first item */
		if ((int16_t)d1 > (int16_t)d0) {
			build = 0;                              /* page is past the end: show nothing */
		} else {
			set_w(&d0, (uint16_t)d0 - (uint16_t)d1);   /* items left after the first one */
			set_w(&d1, (uint16_t)d1 * 2);
			list += (int16_t)d1;                    /* a0 -> first object index of the page */
			a1 = RAM_OBJECTS;
			a2 = bus_r32(bus_r32(RAM_MYSTERY_HDR) + 0x18);   /* object icon table (0x182 bytes each) */
			a4 = RAM_INV_ICON_TILES;
			set_w(&d2, (uint16_t)d2 - 1);          /* d2 = 7: slot counter, position = d2 ^ 7 */
		}
	}

	uint32_t p = 0;
	int cleared_any = 0;
	if (build) {
		for (;;) {
			/* one slot: list entry -> object record -> icon number at object+4 */
			d1 = (uint32_t)RAM16(list) * 0x1A;      /* move.w / mulu.w #$1a */
			list += 2;
			set_w(&d1, RAM16(a1 + (int16_t)d1 + 4));
			if ((uint16_t)d1 != 0) {                /* object has an icon */
				d1 = (uint32_t)(uint16_t)((uint16_t)d1 - 1) * 0x182;
				a3 = a2 + (int16_t)d1;
				set_w(&d5, bus_r16(a3));            /* palette bits for the name-table words */
				a3 += 2;
				for (int i = 0; i < 0x60; i++) {    /* copy the 384 bytes of icon tiles */
					RAM32_W(a4, bus_r32(a3));
					a3 += 4; a4 += 4;
				}
				set_w(&d3, 0xFFFF);                 /* dbra counter ran out */
				uint16_t pos = (uint16_t)d2 ^ 7;    /* slot position 0..7 */
				set_w(&d1, pos);
				set_w(&d3, pos);
				set_w(&d1, pos & 3);                /* column in the row */
				set_w(&d4, (uint16_t)d1);
				set_w(&d1, (uint16_t)d1 << 3);
				set_w(&d1, (uint16_t)d1 + (uint16_t)d4 * 2);        /* column * 10 bytes */
				if ((int16_t)(uint16_t)d3 >= 4)
					set_w(&d1, (uint16_t)d1 + 0x78);               /* second row of slots */
				set_w(&d4, (uint16_t)d3);
				set_w(&d4, (uint16_t)d4 << 3);
				set_w(&d3, (uint16_t)d3 << 2);
				set_w(&d3, (uint16_t)d3 + (uint16_t)d4);            /* 12 * pos */
				set_w(&d3, (uint16_t)d3 + 0x8580);                  /* tile $580, priority */
				set_w(&d3, (uint16_t)d3 | (uint16_t)d5);            /* icon palette */
				p = table + (int16_t)d1;                           /* a6 */
				d4 = 2;
				for (int row = 0; row < 3; row++) {
					d5 = 3;
					for (int col = 0; col < 4; col++) {
						RAM16(p) = (uint16_t)d3; p += 2;
						set_w(&d3, (uint16_t)d3 + 3);               /* next column: +3 tiles */
					}
					d5 = 0xFFFF;
					RAM16(p) = 0x8000;                              /* spacer */
					p += 0x20;
					set_w(&d3, (uint16_t)d3 - 0xB);                 /* next row: -12 + 1 */
				}
				d4 = 0xFFFF;
			}
			/* next list entry; stop when the page's items are used up or slots run out */
			set_w(&d0, (uint16_t)d0 - 1);
			if ((int16_t)(uint16_t)d0 < 0) break;
			set_w(&d2, (uint16_t)d2 - 1);
			if ((uint16_t)d2 == 0xFFFF) break;
		}
	}

	/* blank the remaining slots (d2-1 .. 0) with $8000 */
	flags_sub16(ctx, (uint16_t)d2, 1);
	set_w(&d2, (uint16_t)d2 - 1);
	if ((int16_t)(uint16_t)d2 >= 0) {
		for (;;) {
			uint16_t pos = (uint16_t)d2 ^ 7;
			set_w(&d1, pos);
			set_w(&d3, pos);
			set_w(&d1, pos & 3);
			set_w(&d4, (uint16_t)d1);
			set_w(&d1, (uint16_t)d1 << 3);
			set_w(&d1, (uint16_t)d1 + (uint16_t)d4 * 2);
			if ((int16_t)(uint16_t)d3 >= 4)
				set_w(&d1, (uint16_t)d1 + 0x78);
			p = table + (int16_t)d1;
			for (int row = 0; row < 3; row++) {
				for (int col = 0; col < 5; col++) { RAM16(p) = 0x8000; p += 2; }   /* 4 tiles + spacer */
				p += 0x20 - 2;                                      /* the spacer was not advanced */
			}
			d4 = 0xFFFF; d5 = 0xFFFF;
			cleared_any = 1;
			uint16_t before = (uint16_t)d2;
			set_w(&d2, before - 1);
			if (before == 0) break;
		}
		/* last move.w #$8000,(a6): N=1 Z=0 V=C=0; X from the final addi.w #$78 = 0 */
		cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, CC_N);
	}
	(void)cleared_any;

	ctx->d[0] = d0; ctx->d[1] = d1; ctx->d[2] = d2; ctx->d[3] = d3; ctx->d[4] = d4; ctx->d[5] = d5;
	ctx->a[0] = list; ctx->a[1] = a1; ctx->a[2] = a2; ctx->a[3] = a3; ctx->a[4] = a4;
	/* a5/a6 are saved and restored by the original (movem.l a5-a6) */
}
