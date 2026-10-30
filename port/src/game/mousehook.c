/* Game-side hooks for mouse control: see mousehook.h and docs/mouse.md. */
#include "mousehook.h"
#include "walk.h"
#include "emu.h"

#define VBLANK_HOOK     0xFF0000   /* l: current VBlank work routine */
#define VBLANK_MAIN     0x00A65C   /* in-game VBlank handler */
#define MYSTERY_HDR     0xFF068E
#define ROOM_INDEX      0xFF06AC
#define TARGET_OBJ      0xFF06AE
#define TARGET2_OBJ     0xFF06B6
#define VERB_CUR        0xFF06C0
#define DEFAULT_VERB    0xFF06C4
#define OBJECTS         0xFF1200
#define OBJECT_SIZE     0x1A
#define OBJECT_LAST     0xFF06F4
#define FLAGS_0ABF      0xFF0ABF
/* actor hit boxes, indexed by (object id - 1) * 2: left/right/up/down extents */
#define BOX_LEFT        0x032550
#define BOX_RIGHT       0x032598
#define BOX_UP          0x0324C0
#define BOX_DOWN        0x032508

static inline bool bit8(uint32_t a, int b) { return (RAM8(a) >> b) & 1; }
static inline int s16(uint32_t a) { return (int16_t)RAM16(a); }

bool mg_in_room(void)
{
	return RAM32(VBLANK_HOOK) == VBLANK_MAIN && RAM32(MYSTERY_HDR) != 0 &&
	       RAM16(MAP_WIDTH) != 0 && RAM16(MAP_HEIGHT) != 0;
}

mg_mode mg_mode_get(void)
{
	if (!mg_in_room())
		return MG_NONE;
	if (bit8(VBLANK_FLAGS, 7))
		return MG_BUSY;                         /* action / script running */
	if (bit8(VBLANK_FLAGS, 3))
		return MG_POINTER;
	/* the conditions under which player_update reaches verb_menu_open_check with D-pad
	 * walking enabled (walk.c: player_update, verb_bar_slide, verb_menu_open_check_body) */
	if (bit8(GAME_STATE, 1) || bit8(GAME_FLAGS_09DE, 2) || bit8(ACTORS_ON_PATH, 0) ||
	    bit8(GAME_FLAGS_09E8, 2) || bit8(FLAGS_0ABF, 0) || bit8(GAME_FLAGS_09E9, 4) ||
	    bit8(GAME_FLAGS_09E9, 6) || mg_bar_sliding())
		return MG_BUSY;
	return MG_FREE;
}

static void place(int sx, int sy, int *cx, int *cy)
{
	int x = sx - 8, y;
	if (sy >= MG_BAR_TOP) {
		/* bar: pointer_update maps the cell from the pointer's top-left: cell k of the
		 * verb view spans pointer x 8+40k..47+40k, i.e. screen 16+40k..55+40k; the
		 * row split is pointer y > $C0 */
		y = sy;
		if (mg_inventory_view() && x < 40)
			x = 40;                             /* column 0 is not a slot (verbs.md 5) */
	} else {
		y = sy - 8;                             /* the hit test uses (x+8, y-8+16) */
	}
	if (x < -8) x = -8;
	if (x > 0xF7) x = 0xF7;
	if (y < 0) y = 0;
	if (y > 0xCF) y = 0xCF;
	*cx = x;
	*cy = y;
}

void mg_pointer_place(int sx, int sy)
{
	int x, y;
	place(sx, sy, &x, &y);
	RAM16(MENU_CURSOR_X) = (uint16_t)x;
	RAM16(MENU_CURSOR_Y) = (uint16_t)y;
}

void mg_pointer_get(int *x, int *y)
{
	*x = s16(MENU_CURSOR_X);
	*y = s16(MENU_CURSOR_Y);
}

int mg_target(void) { return s16(TARGET_OBJ); }
int mg_target2(void) { return s16(TARGET2_OBJ); }
bool mg_second_object(void) { return bit8(GAME_FLAGS_09DE, 0); }
int mg_verb(void) { return s16(VERB_CUR); }
int mg_default_verb(void) { return s16(DEFAULT_VERB); }
bool mg_inventory_view(void) { return bit8(VBLANK_FLAGS, 4); }
bool mg_bar_sliding(void) { return RAM8(VERB_BAR_SLIDE) != 0xFF; }
int mg_room(void) { return s16(ROOM_INDEX); }

/* $FF09E0 is active low in the order U D L R B C A S; emu.h's mask is U D L R A B C S */
static unsigned pad_latched_mask(void)
{
	unsigned g = (uint8_t)~RAM8(PAD1_CUR), m = g & 0x0F;
	if (g & 0x10) m |= PAD_B;
	if (g & 0x20) m |= PAD_C;
	if (g & 0x40) m |= PAD_A;
	if (g & 0x80) m |= PAD_START;
	return m;
}

bool mg_pad_latched(unsigned pad_mask) { return (pad_latched_mask() & pad_mask) == pad_mask; }
bool mg_pad_latched_any(unsigned pad_mask) { return (pad_latched_mask() & pad_mask) != 0; }

int mg_object_default_verb(int target)
{
	if (target <= 0)
		return 0;
	if (target < 3)
		return 10;                              /* Shaggy / Scooby: Look */
	return (int8_t)RAM8(OBJECTS + OBJECT_SIZE * (uint32_t)(target - 3) + 0x16);
}

/* $000E94-$001066 for pointer y < $A8, minus the "second object" bookkeeping */
int mg_hit_test(int sx, int sy)
{
	if (!mg_in_room() || sy >= MG_BAR_TOP)
		return 0;
	int px, py;
	place(sx, sy, &px, &py);
	if (py - 8 < 0)
		return 0;
	int camx = s16(CAMERA_X), camy = s16(CAMERA_Y);
	int cx = (uint16_t)(px + 8 + camx) >> 3, cy = (uint16_t)(py - 8 + camy) >> 3;
	int hit = 0;
	/* Scooby: crosshair top-left against his box */
	if (!bit8(FLAGS_0ABF, 1) && bit8(ACTORS_ACTIVE, 1)) {
		int x = px + camx, y = py + camy, ax = s16(ACTOR_X(1)), ay = s16(ACTOR_Y(1));
		if (x <= ax + (int16_t)ROM16(BOX_RIGHT + 2) &&
		    x >= ax - (int16_t)ROM16(BOX_LEFT + 2) &&
		    y <= ay + (int16_t)ROM16(BOX_DOWN + 2) && y >= ay - (int16_t)ROM16(BOX_UP + 2))
			hit = 2;
	}
	uint32_t hdr = RAM32(MYSTERY_HDR);
	uint32_t base = bus_r32(hdr + 0x1C), offs = bus_r32(hdr + 0x24);
	uint16_t room = RAM16(ROOM_INDEX);
	int last = s16(OBJECT_LAST);
	for (int i = 0; i <= last && i < 0x800 / OBJECT_SIZE; i++) {
		uint32_t rec = OBJECTS + OBJECT_SIZE * (uint32_t)i;
		if (RAM16(rec + 6) != room)
			continue;
		uint16_t id = RAM16(rec + 2);
		if (RAM8(rec + 0x18) & 2) {             /* sprite actor: box around its position */
			uint32_t slot = 4u * RAM8(rec + 0x19);
			unsigned k = 2u * (uint16_t)(id - 1);
			int ax = s16(ACTOR_X(2) + slot), ay = s16(ACTOR_Y(2) + slot);
			int x = px + camx, y = py + camy;
			if (ax - (int16_t)ROM16(BOX_LEFT + k) <= x && ax + (int16_t)ROM16(BOX_RIGHT + k) >= x &&
			    ay - (int16_t)ROM16(BOX_UP + k) <= y && ay + (int16_t)ROM16(BOX_DOWN + k) >= y)
				hit = i + 3;
			continue;
		}
		if (!id)
			continue;
		uint32_t r = base + bus_r32(offs + 4u * (uint16_t)(id - 1));
		int x = (int16_t)bus_r16(r), y = (int16_t)bus_r16(r + 2);
		int w = (int16_t)bus_r16(r + 4), h = (int16_t)bus_r16(r + 6);
		if (cx >= x && cy >= y && cx < x + w && cy < y + h)
			hit = i + 3;
	}
	return hit;
}

bool mg_screen_to_world(int sx, int sy, int *wx, int *wy)
{
	*wx = sx + s16(CAMERA_X);
	*wy = sy - MG_ROOM_TOP + s16(CAMERA_Y);
	return sy >= MG_ROOM_TOP && sy < MG_BAR_TOP;
}

void mg_shaggy_pos(int *x, int *y)
{
	*x = s16(ACTOR_X(0));
	*y = s16(ACTOR_Y(0));
}

/* ------------------------------------------------------------------ verb bar */
static const uint8_t verb_ids[10] = { 2, 10, 3, 7, 9, 6, 8, 4, 1, 5 };   /* $02FE3C */

bool mg_verb_cell_center(int verb_id, int *sx, int *sy)
{
	for (int s = 0; s < 10; s++)
		if (verb_ids[s] == verb_id) {
			*sx = 32 + 40 * (s % 5);           /* label x 16+40k..47+40k */
			*sy = s < 5 ? 183 : 200;           /* label rows 176-191 / 192-207 */
			return true;
		}
	return false;
}

int mg_verb_at(int sx, int sy)
{
	/* pointer_update: slot = 1 + (pointer_x - 8) / 40 with pointer_x = sx - 8, clamped to
	 * the 5 columns; lower row when pointer_y > $C0 */
	int col = sx < 16 ? 0 : (sx - 16) / 40;
	if (col > 4)
		col = 4;
	return verb_ids[col + (sy > 0xC0 ? 5 : 0)];
}

int mg_inv_page(void) { return s16(0xFF06EC); }

int mg_inventory_item_at(int sx, int sy)
{
	int px, py;
	place(sx, sy, &px, &py);
	/* pointer_update (inventory view): t = 1 + min(x / 40, 5), +6 on the lower row;
	 * slot = t - 1 up to 7, else t - 2 (5 and 10 are the page arrows) */
	int col = px < 0 ? 0 : px / 40;
	if (col > 5)
		col = 5;
	int t = 1 + col + (py > 0xC0 ? 6 : 0);
	int slot = t <= 7 ? t - 1 : t - 2;
	if (slot == 5 || slot == 10)
		return 0;
	/* main loop $0011B8: list entry 4 * page + slot - 1 (slot - 1 above 5) */
	if (slot >= 5)
		slot--;
	int idx = 4 * mg_inv_page() + slot, count = 0;
	while (count < 64 && !(RAM16(0xFF0A1E + 2u * (unsigned)count) & 0x8000))
		count++;
	if (count < idx || idx < 1)
		return 0;
	return s16(0xFF0A1E + 2u * (unsigned)(idx - 1)) + 3;
}

mg_bar_hit mg_bar_at(int sx, int sy)
{
	if (sy < MG_BAR_TOP || sx < 0 || sx >= MG_SCREEN_W)
		return BAR_NONE;
	if (!mg_inventory_view()) {
		if (sx >= 208 && sx < 232)
			return BAR_TOGGLE;                  /* the "C" arrow icon */
		return sx >= 16 && sx < 208 ? BAR_VERB : BAR_NONE;
	}
	if (sx < 48)
		return BAR_TOGGLE;                      /* left arrow back to the verbs */
	if (sx >= 208)
		return sy <= 0xC0 ? BAR_PAGE_UP : BAR_PAGE_DOWN;
	return BAR_ITEM;
}

void mg_page_arrow_center(bool down, int *sx, int *sy)
{
	*sx = 220;
	*sy = down ? 200 : 182;
}
