/* The hidden DEBUG menu (GitLab #21): ROOMS ($0088A2) and OBJECTS ($008C40) editors, their list
 * browsers ($0088D8, $008AB4) and the screen loader ($0096F4).  Enabled by the cheat code
 * A B A C A B Left Right A B Left on the pause menu (menus.c: cheat_step).  See
 * docs/subsystems/menus.md, "Hidden DEBUG menu".
 *
 * ROOMS: lists this mystery's rooms ($FF068E header, +8 = room table of 20-byte records, +$20 =
 * the name text); D-pad moves the red bar (16 rows a page, A/C page up/down), B selects: the
 * start room ($FF000A) becomes that room.  OBJECTS: the same browser over the object table
 * (+$28 = 8-byte records, 16 a page) picks an object, then the rooms browser picks its new
 * location (object +6): room 1 means "carried" (inventory list $FF0A1E).
 *
 * The browsers wait for VBlank twice per pass (before drawing the page, before moving the bar) so
 * they are HLE_RANGEs split at those waits.  The 68k stack juggling in sub_008C40 (push the
 * object's index and record pointer around the rooms browser) stays 68k.
 *
 * Registers: the browsers carry d3 (bar row on the page), d4 (entry count), d5 (page), d7 (last row
 * of the page) and a2 (mystery header) across the waits.  d0-d2, d6 and a1 are recomputed after
 * every wait; at `rts` only d0 (row + 16 * page) is live.
 */
#include "menus.h"

/* ---- the highlight bar: 8 sprites in a chain at VRAM $FC00 (y = $B8 + row * 8, tile $7D0) */
static void list_draw_bar(hle_ctx *ctx, uint16_t row)
{
	uint16_t y = (uint16_t)(row * 8 + 0xB8);
	ctx->sr = 0x2700;
	vdp_ctrl_w32(0x7C000003);
	uint16_t x = 0x80;
	for (uint16_t link = 0xC01; link != 0xC08; link++, x += 0x20) {
		vdp_data_w(y);
		vdp_data_w(link);
		vdp_data_w(0x07D0);
		vdp_data_w(x);
	}
	vdp_data_w(y);              /* last sprite of the chain: link 0 */
	vdp_data_w(0x0C00);
	vdp_data_w(0x07D0);
	vdp_data_w(x);
	ctx->sr = 0x2000;
}

/* the three exits of the browsers' input pass */
typedef struct list_exits {
	uint32_t redraw;            /* new page: back to the page set-up */
	uint32_t bar;               /* nothing chosen: wait a frame and look again */
	uint32_t pick;              /* B: the `rts` with d0 = row + 16 * page */
} list_exits;

/* The input pass shared by both browsers ($008A08..$008A9A and its twin $008BAC..$008C3E):
 * up/down move the bar (past a page's end they page), A = page up, C = page down, B = pick.
 * A press is "down now, up in the previous frame" (pad copy at $FF09E2). */
static uint32_t list_input(hle_ctx *ctx, const list_exits *ex)
{
	uint8_t pad = RAM8(PAD1_CUR), prev = RAM8(PAD1_PREV);
	uint16_t d3 = (uint16_t)ctx->d[3], d4 = (uint16_t)ctx->d[4], d5 = (uint16_t)ctx->d[5],
	         d7 = (uint16_t)ctx->d[7];
	uint16_t last_page = (uint16_t)((d4 - 1) >> 4);
	uint32_t go = ex->bar;
	bool paged = false;
	set_w(&ctx->d[0], 0x07D0);                      /* left over from the bar's sprite words */
	if (menu_pressed(pad, prev, PAD_U)) {
		if (d3)
			d3--;
		else if (d5) {          /* up past the first row: previous page, bar on its last row */
			d3 = 15;
			d5--;
			paged = true;
		}
	} else if (menu_pressed(pad, prev, PAD_D)) {
		if (d3 != d7)
			d3++;
		else {
			set_w(&ctx->d[0], last_page);           /* (d4 - 1) >> 4 is computed in d0 */
			if (last_page != d5) {
				d3 = 0;
				d5++;
				paged = true;
			}
		}
	}
	if (!paged && menu_pressed(pad, prev, PAD_A) && d5) {                 /* A: page up */
		d5--;
		d3 = 0;
		paged = true;
	}
	if (!paged && menu_pressed(pad, prev, PAD_C)) {                       /* C: page down */
		set_w(&ctx->d[0], last_page);
		if (last_page != d5) {
			d5++;
			d3 = 0;
			paged = true;
		}
	}
	if (paged) {
		go = ex->redraw;
	} else if (menu_pressed(pad, prev, PAD_B)) {
		set_w(&ctx->d[0], (uint16_t)((d5 << 4) + d3));
		go = ex->pick;
	}
	set_w(&ctx->d[3], d3);
	set_w(&ctx->d[5], d5);
	return go;
}

/* d5 = page of entry d3, d3 = its row: d3 mod 16 by repeated subtraction (as the original) */
static void list_split_page(hle_ctx *ctx, uint16_t entry)
{
	int16_t page = -1, row = (int16_t)entry;
	do {
		page++;
		row -= 16;
	} while (row >= 0);
	ctx->d[5] = 0xFFFF0000u | (uint16_t)page;   /* moveq #-1,d5 / addq.w #1,d5: the high word stays $FFFF */
	set_w(&ctx->d[3], (uint16_t)(row + 16));
}

/* d6 = entries on this page - 1 (at most 15), d7 = d6; first text row 7, column 1 */
static void list_page_rows(hle_ctx *ctx)
{
	int16_t d6 = (int16_t)((uint16_t)ctx->d[4] - ((uint16_t)ctx->d[5] << 4));
	if (d6 > 15)
		d6 = 15;
	ctx->d[1] = 1;
	set_w(&ctx->d[2], 7);
	set_w(&ctx->d[6], (uint16_t)d6);
	set_w(&ctx->d[7], (uint16_t)d6);
}

/* draw string a0 at column d1, row d2 (text subsystem $009F46; keeps d1 and a0) */
static void list_text(hle_ctx *ctx, uint32_t str)
{
	ctx->a[0] = str;
	hle_call68k(ctx, A_TEXT_9F46);
	set_w(&ctx->d[2], ctx->d[2] + 1);
}

/* ================================================================== rooms browser ($0088D8) */
/* d0 = room to start on; returns d0 = chosen room.  Room records are 20 bytes: the first long is
 * the offset of the name in the text block (header +$20).  The first screen of a page above the
 * table shows two fixed strings ($008A9C for the line right before the table, $008AA2 above it). */
#define RB_PAGE   0x0088FC
#define RB_ROWS   0x00892C
#define RB_WAIT1  0x008928
#define RB_BAR    0x008972
#define RB_RTS    0x008A9A
#define ROM_STR_BEFORE_TABLE 0x008A9C
#define ROM_STR_ABOVE_TABLE  0x008AA2
#define ROM_BLANK_LINE       0x02FFF4

/* $0088D8: split d0 into page and row, count the rooms */
HLE_RANGE(0x0088D8, rooms_browser, 226, RB_PAGE)
{
	uint16_t entry = (uint16_t)ctx->d[0];
	ctx->d[3] = (ctx->d[3] & 0xFFFF0000u) | entry;
	ctx->a[2] = RAM32(MYSTERY_HDR);
	uint32_t span = ROM32(ctx->a[2] + 12) - ROM32(ctx->a[2] + 8);
	/* divu.w #20,d4: quotient in the low word, remainder in the high word */
	ctx->d[4] = ((span % 20) << 16) | ((span / 20) & 0xFFFF);
	list_split_page(ctx, entry);
	hle_jump(ctx, RB_PAGE);
}

/* $0088FC: draw a page: a1 -> its first record (the table starts at header +8; the page begins
 * -$28 before so that the first rows can hold the placeholder strings); then wait a frame. */
HLE_RANGE(0x0088FC, rooms_page, 147, RB_WAIT1)
{
	uint32_t table = ROM32(ctx->a[2] + 8);
	ctx->a[1] = table + (uint32_t)(uint16_t)ctx->d[5] * 0x140 - 0x28;
	list_page_rows(ctx);
	hle_jump(ctx, RB_WAIT1);
}

/* $00892C: the page's rows (the name strings), blanks down to row $17, then wait a frame */
HLE_RANGE(0x00892C, rooms_rows, 500, RB_BAR)
{
	uint32_t table = ROM32(ctx->a[2] + 8);
	uint32_t text = ROM32(ctx->a[2] + 0x20);
	int16_t d6 = (int16_t)ctx->d[6];
	do {
		int32_t before = (int32_t)(table - ctx->a[1]);       /* > 0: this row is above the table */
		uint32_t str;
		if (before <= 0)
			str = text + ROM32(ctx->a[1]);
		else
			str = (uint16_t)before == 0x28 ? ROM_STR_BEFORE_TABLE : ROM_STR_ABOVE_TABLE;
		set_w(&ctx->d[0], (uint16_t)before);
		list_text(ctx, str);
		ctx->a[1] += 0x14;
	} while (d6-- != 0);
	set_w(&ctx->d[6], 0xFFFF);
	while ((uint16_t)ctx->d[2] != 0x17)
		list_text(ctx, ROM_BLANK_LINE);
	hle_jump(ctx, RB_BAR);
}

/* $008976: the bar, then the pad */
HLE_RANGE(0x008976, rooms_input, 1000, RB_PAGE, RB_BAR, RB_RTS)
{
	list_draw_bar(ctx, (uint16_t)ctx->d[3]);
	static const list_exits ex = { RB_PAGE, RB_BAR, RB_RTS };
	hle_jump(ctx, list_input(ctx, &ex));
}

/* ================================================================== objects browser ($008AB4) */
/* Returns d0 = index of the chosen object.  Object-name table at header +$28: 8-byte records
 * (+4 = name offset in the text block), 16 a page; the count is $FF06F4 (last index). */
#define OB_PAGE   0x008AD4
#define OB_WAIT1  0x008AFC
#define OB_ROWS   0x008B00
#define OB_BAR    0x008B16
#define OB_RTS    0x008C3E

HLE_RANGE(0x008AB4, objects_browser, 80, OB_PAGE)
{
	ctx->d[0] = 0;
	ctx->a[2] = RAM32(MYSTERY_HDR);
	set_w(&ctx->d[3], 0);
	set_w(&ctx->d[4], RAM16(OBJ_LAST));
	list_split_page(ctx, 0);
	hle_jump(ctx, OB_PAGE);
}

HLE_RANGE(0x008AD4, objects_page, 122, OB_WAIT1)
{
	uint16_t ofs = (uint16_t)((uint16_t)ctx->d[5] << 7);        /* lsl.w #7,d1 / lea (a1,d1.w) */
	ctx->a[1] = ROM32(ctx->a[2] + 0x28) + (uint32_t)(int32_t)(int16_t)ofs;
	list_page_rows(ctx);
	hle_jump(ctx, OB_WAIT1);
}

HLE_RANGE(0x008B00, objects_rows, 500, OB_BAR)
{
	uint32_t text = ROM32(ctx->a[2] + 0x20);
	int16_t d6 = (int16_t)ctx->d[6];
	do {
		list_text(ctx, text + ROM32(ctx->a[1] + 4));
		ctx->a[1] += 8;
	} while (d6-- != 0);
	set_w(&ctx->d[6], 0xFFFF);
	hle_jump(ctx, OB_BAR);
}

HLE_RANGE(0x008B1A, objects_input, 1000, OB_PAGE, OB_BAR, OB_RTS)
{
	list_draw_bar(ctx, (uint16_t)ctx->d[3]);
	static const list_exits ex = { OB_PAGE, OB_BAR, OB_RTS };
	hle_jump(ctx, list_input(ctx, &ex));
}

/* ================================================================== screen loader ($0096F4) */
/* a0 = RLE stream of a screen: unpack it to the ring buffer, wait a frame, upload it to plane A
 * ($C000, $380 words).  [68k: bsr wait_vblank at $00970C] */
HLE_RANGE(0x0096F4, menu_load_screen, 100, 0x00970C)
{
	ctx->a[2] = LZSS_RING;
	rle_call(ctx);
	set_w(&ctx->d[0], 0xC000);
	set_w(&ctx->d[1], 0x380);
	ctx->a[0] = LZSS_RING;
	hle_jump(ctx, 0x00970C);
}

HLE_RANGE(0x009710, menu_load_screen_dma, 50, 0x009714)
{
	hle_call68k(ctx, A_VDP_DMA);
	hle_jump(ctx, 0x009714);                    /* rts */
}

/* ================================================================== DEBUG ROOMS ($0088A2) */
/* show the rooms screen ($018DD0), then choose a start room with the browser; room <= 1 is no
 * choice (the browser returns 0/1 for the placeholder rows): ask again */
HLE_RANGE(0x0088A2, debug_rooms, 12, 0x0088A8)
{
	ctx->a[0] = 0x018DD0;
	hle_jump(ctx, 0x0088A8);                    /* bsr menu_load_screen */
}

HLE_RANGE(0x0088AC, debug_rooms_pick, 16, 0x0088B2)
{
	set_w(&ctx->d[0], RAM16(MENU_ROOM));
	hle_jump(ctx, 0x0088B2);                    /* bsr rooms_browser */
}

HLE_RANGE(0x0088B6, debug_rooms_done, 70, 0x0088A2, MENU_OPEN_ADDR)
{
	int16_t room = (int16_t)ctx->d[0];
	if (room <= 1) {
		hle_jump(ctx, 0x0088A2);
		return;
	}
	RAM16(MENU_ROOM) = (uint16_t)room;
	menu_dma(ctx, 0xFC00, 0x34, ROM_TITLE_SAT);         /* the title sprite table is back in VRAM */
	hle_jump(ctx, MENU_OPEN_ADDR);
}

/* ================================================================== DEBUG OBJECTS ($008C40) */
/* font colours, screen $018E08, [68k: bsr menu_load_screen, bsr objects_browser], then
 * debug_objects_pick (push the index and the record, start the rooms browser on the object's
 * current location) and debug_objects_move (pop, move the object) below */
HLE_RANGE(0x008C40, debug_objects, 100, 0x008C58)
{
	set_w(&ctx->d[0], 0xF);
	set_w(&ctx->d[1], 0);
	set_w(&ctx->d[2], RAM16(FONT_BASE));
	hle_call68k(ctx, 0x007A2A);                 /* font_tiles_load */
	ctx->a[0] = 0x018E08;
	hle_jump(ctx, 0x008C58);                    /* bsr menu_load_screen */
}

/* $008C60: the object browser returned the chosen object index in d0.  Keep it on the stack
 * (word), with d0 = index * 26 (its record's offset) and a0 = the object table (long pair), and
 * leave in front of `bsr rooms_browser` ($008C74) with d0 = the object's current location, so
 * that the browser starts on it.  10 bytes are pushed: 2 + 8 (movem.l d0/a0,-(sp): d0 lands below
 * a0). */
HLE_RANGE(0x008C60, debug_objects_pick, 40, HLE_EXIT_SP(0x008C74, -10))
{
	push16(ctx, (uint16_t)ctx->d[0]);                        /* move.w d0,-(sp) */
	ctx->d[0] = (uint32_t)(uint16_t)ctx->d[0] * 0x1A;        /* mulu.w #26,d0 */
	ctx->a[0] = 0xFF1200;                                    /* lea objects,a0 */
	push32(ctx, ctx->a[0]);                                  /* movem.l d0/a0,-(sp) */
	push32(ctx, ctx->d[0]);
	set_w(&ctx->d[0], RAM16(ctx->a[0] + (uint16_t)ctx->d[0] + 6));   /* the object's location */
	cc_logic16(ctx, ctx->d[0]);
	hle_jump(ctx, 0x008C74);
}

/* $008C78: back from the rooms browser: d0 = the chosen location.  Pop what debug_objects_pick
 * pushed (d1 = new location, d0/a0 = record offset / table, d2 = object index).  The object d2
 * (record at a0 + d0) moves to location d1 (a room number, 1 = carried by the player).  Keeps the
 * carried-items list ($FF0A1E: object indices, $FFFF-terminated) in step: leaving room 1 removes
 * the index, entering it appends. */
HLE_RANGE(0x008C78, debug_objects_move, 90, HLE_EXIT_SP(0x008CC0, +10))
{
	set_w(&ctx->d[1], ctx->d[0]);                            /* move.w d0,d1 */
	ctx->d[0] = pop32(ctx);                                  /* movem.l (sp)+,d0/a0 */
	ctx->a[0] = pop32(ctx);
	set_w(&ctx->d[2], pop16(ctx));                           /* move.w (sp)+,d2 */
	uint32_t rec = ctx->a[0] + (uint16_t)ctx->d[0] + 6;     /* word: location */
	uint16_t new_loc = (uint16_t)ctx->d[1], idx = (uint16_t)ctx->d[2];
	uint16_t old_loc = RAM16(rec);
	if (new_loc != old_loc) {
		if (old_loc == 1) {
			uint32_t a1 = 0xFF0A1E;
			while (RAM16(a1) != idx)        /* find it, then shift the rest down */
				a1 += 2;
			for (;; a1 += 2) {
				RAM16(a1) = RAM16(a1 + 2);
				if (RAM16(a1 + 2) & 0x8000)
					break;
			}
		} else if (new_loc == 1) {
			uint32_t a1 = 0xFF0A1E;
			while (!(RAM16(a1) & 0x8000))   /* to the terminator, replace it, new terminator */
				a1 += 2;
			RAM16(a1) = idx;
			RAM16(a1 + 2) = 0xFFFF;
		}
		RAM16(rec) = new_loc;
	}
	hle_jump(ctx, 0x008CC0);
}

HLE_RANGE(0x008CC0, debug_objects_done, 40, MENU_OPEN_ADDR)
{
	menu_dma(ctx, 0xFC00, 0x34, ROM_TITLE_SAT);
	hle_jump(ctx, MENU_OPEN_ADDR);
}
