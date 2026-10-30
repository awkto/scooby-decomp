/* Walking: D-pad -> Shaggy movement, per-pixel collision against the room's zone map,
 * camera follow, Scooby following/wandering, depth scaling and scripted actor paths.
 * docs/subsystems/walking.md describes the data and the algorithms; this file is the
 * decompilation of $005E4A-$0073D4 (one routine with four tail entry points) and its
 * collision helpers $0073D6/$007476/$00750A.
 *
 * Register contract of player_update and its tails: they are only entered from
 * vblank_main ($00A6B0 bsr) or by branches inside $005E4A, and always leave through the
 * single rts at $0073D4.  vblank_main reads no register after the call and the VBlank
 * vector ($0008BC) restores D0-D7/A0-A6 and SR with movem/rte, so the final register
 * values and flags are dead.  difftest therefore ignores D0-D7/A0-A6 and the CCR for
 * these five entries (see dt_ignores in port/tools/difftest.c). */
#include "walk.h"

/* ------------------------------------------------------------------ small helpers */
static inline int16_t S16(uint32_t a) { return (int16_t)RAM16(a); }
static inline bool bit8(uint32_t a, int b) { return (RAM8(a) >> (b & 7)) & 1; }
static inline void set8(uint32_t a, int b) { RAM8(a) |= (uint8_t)(1u << (b & 7)); }
static inline void clr8(uint32_t a, int b) { RAM8(a) &= (uint8_t)~(1u << (b & 7)); }
/* pad bits (active low): 0 U, 1 D, 2 L, 3 R, 4 B, 5 C, 6 A, 7 Start */
static inline bool held(int b) { return !bit8(PAD1_CUR, b); }
static inline bool held_before(int b) { return !bit8(PAD1_PREV, b); }
static inline bool pressed_now(int b) { return held(b) && !held_before(b); }
enum { PAD_U, PAD_D, PAD_L, PAD_R, PAD_B, PAD_C, PAD_A, PAD_S };

/* zone word of the cell containing world pixel (x, y): mirrors
 *   lsr.w #3,dx / add.w dx,dx / lsr.w #3,dy / mulu.w (width*2),dy / add.w dy,dx
 *   move.w (0,a0,dx.w),..      (16-bit index, sign-extended) */
static uint16_t zone_word(uint16_t x, uint16_t y)
{
	uint16_t idx = (uint16_t)(((x >> 3) * 2) + (uint16_t)((uint32_t)(y >> 3) * (uint16_t)(RAM16(MAP_WIDTH) * 2)));
	return RAM16(ZONE_MAP + (int16_t)idx);
}

/* ------------------------------------------------------------------ collision */
typedef struct {
	bool blocked;   /* result: the pixel is outside the room or on a non-zero collision pixel */
	uint32_t d0;    /* D0 as the 68k leaves it (pad_dir8 falls back to it) */
} probe_result;

/* Core of $0073D6/$007476: look up pixel (x, y) in the collision tiles.
 * zone index -> COLL_NAMES[index] (bits 0-10 tile, 11 hflip, 12 vflip) -> nibble of
 * COLL_TILES[tile] at (x&7, y&7). Fills ctx registers like the original. */
static void coll_pixel(hle_ctx *ctx, uint16_t x, uint16_t y, uint16_t zone)
{
	uint16_t name = ROM16(COLL_NAMES + (int16_t)(uint16_t)(zone * 2));
	set_w(&ctx->d[5], name);
	uint16_t tile_off = (uint16_t)((name & 0x7FF) << 5);
	ctx->a[0] = COLL_TILES + (int32_t)(int16_t)tile_off;
	uint16_t px = x & 7, py = y & 7;
	if (!(name & 0x0800))           /* tile rows hold pixel 0 in the top nibble; hflip */
		px ^= 7;                    /* is the *unflipped* case for this shift-based read */
	if (name & 0x1000)
		py ^= 7;
	py <<= 2;
	px <<= 2;
	set_w(&ctx->d[1], py);
	set_w(&ctx->d[0], px);
	uint32_t mask = 0xFu << px;
	uint32_t row = ROM32(ctx->a[0] + (int16_t)py);
	ctx->d[5] = mask & row;
	cc_logic32(ctx, ctx->d[5]);
	cc_set(ctx, CC_X, 0);           /* lsl.w #2,d0 / lsl.l d0,d5 shifted out zeros */
}

/* shared index arithmetic of both probes; returns the zone word */
static uint16_t probe_index(hle_ctx *ctx, uint16_t x, uint16_t y)
{
	uint16_t cx = (uint16_t)((x >> 3) * 2);
	uint16_t w2 = (uint16_t)(RAM16(MAP_WIDTH) * 2);
	set_w(&ctx->d[4], w2);
	ctx->d[1] = (uint32_t)(y >> 3) * w2;              /* mulu.w: full 32-bit product */
	uint32_t sum = (uint32_t)cx + (ctx->d[1] & 0xFFFF);
	set_w(&ctx->d[0], sum);
	ctx->a[0] = ZONE_MAP;
	/* add.w d1,d0 is the last X-setting instruction on the zone == 0 path */
	cc_set(ctx, CC_X, (sum & 0x10000) ? CC_X : 0);
	uint16_t z = RAM16(ZONE_MAP + (int16_t)(uint16_t)sum);
	return z;
}

/* $0073D6: is WALK_PROBE (D-pad candidate position) blocked?
 * in:  D2.w = max x (width*8-1), D3.w = max y (height*8-1)
 * out: Z=1 walkable, Z=0 blocked.  Out-of-range coordinates are blocked.
 *      Zone word bit 15 is ignored (mask $7FFF). */
HLE_FUNC(0x0073D6, walk_blocked_probe, 273)
{
	uint16_t x = RAM16(WALK_PROBE_X), y = RAM16(WALK_PROBE_Y);
	set_w(&ctx->d[0], x);
	set_w(&ctx->d[1], y);
	/* tst.w d0 / bmi; tst.w d1 / bmi */
	if ((int16_t)x < 0) { cc_logic16(ctx, x); return; }
	if ((int16_t)y < 0) { cc_logic16(ctx, y); return; }
	/* cmp.w d0,d2 / blt ; cmp.w d1,d3 / blt  (flags of the compare are returned) */
	int32_t r = (int16_t)ctx->d[2] - (int16_t)x;
	uint32_t u = (uint16_t)ctx->d[2] - (uint32_t)x;
	unsigned f = ((r & 0x8000) ? CC_N : 0) | ((u & 0xFFFF) ? 0 : CC_Z) |
	             ((r < -32768 || r > 32767) ? CC_V : 0) | ((u & 0x10000) ? CC_C : 0);
	if ((int16_t)ctx->d[2] < (int16_t)x) { cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, f); return; }
	r = (int16_t)ctx->d[3] - (int16_t)y;
	u = (uint16_t)ctx->d[3] - (uint32_t)y;
	f = ((r & 0x8000) ? CC_N : 0) | ((u & 0xFFFF) ? 0 : CC_Z) |
	    ((r < -32768 || r > 32767) ? CC_V : 0) | ((u & 0x10000) ? CC_C : 0);
	if ((int16_t)ctx->d[3] < (int16_t)y) { cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, f); return; }

	uint16_t z = probe_index(ctx, x, y) & 0x7FFF;
	set_w(&ctx->d[0], z);
	cc_logic16(ctx, z);
	if (!z)
		return;
	ctx->a[0] = COLL_NAMES;
	coll_pixel(ctx, x, y, z);
}

/* $007476: same test for LINE_PROBE (used while tracing a straight line, $5378 and the
 * bumper-car minigame). Only negative coordinates are rejected; zone mask is $7FF. */
HLE_FUNC(0x007476, walk_blocked_line_probe, 220)
{
	uint16_t x = RAM16(LINE_PROBE_X), y = RAM16(LINE_PROBE_Y);
	set_w(&ctx->d[0], x);
	set_w(&ctx->d[1], y);
	if ((int16_t)x < 0) { cc_logic16(ctx, x); return; }
	if ((int16_t)y < 0) { cc_logic16(ctx, y); return; }
	uint16_t z = probe_index(ctx, x, y) & 0x7FF;
	set_w(&ctx->d[0], z);
	cc_logic16(ctx, z);
	if (!z)
		return;
	ctx->a[0] = COLL_NAMES;
	coll_pixel(ctx, x, y, z);
}

/* $00750A: D-pad to direction code (D0.l), used to pick the wall-slide axis:
 *   0 down-right  1 up-left  2 up-right  3 down-left  4 left  5 right  6 up  7 down
 * With no direction held D0 is left unchanged (flags: Z=0 from the last btst). */
static int dir8_code(void)
{
	if (held(PAD_L))
		return held(PAD_U) ? 1 : held(PAD_D) ? 3 : 4;
	if (held(PAD_R))
		return held(PAD_U) ? 2 : held(PAD_D) ? 0 : 5;
	if (held(PAD_U))
		return 6;
	if (held(PAD_D))
		return 7;
	return -1;
}

HLE_FUNC(0x00750A, pad_dir8, 114)
{
	int d = dir8_code();
	if (d < 0) {
		cc_set(ctx, CC_Z, 0);
		return;
	}
	ctx->d[0] = (uint32_t)d;
	cc_logic32(ctx, ctx->d[0]);
}

/* ------------------------------------------------------------------ scripted paths */
/* A path is a list of (x, y) word pairs read every ACTOR_PATH_RELOAD frames; negative x
 * words are commands:  $FFFD y = set animation y;  $FFFC/$FFFB y = set depth (Shaggy only,
 * $FFFB continues with the next entry);  $FFFE = end;  $FFFF = loop. */

/* $005E4A-$005EEA: Shaggy's path playback */
static void shaggy_path_step(void)
{
	if (!bit8(ACTORS_ON_PATH, 0))
		return;
	if ((int8_t)--RAM8(ACTOR_PATH_DELAY(0)) >= 0)
		return;
	RAM8(ACTOR_PATH_DELAY(0)) = RAM8(ACTOR_PATH_RELOAD(0));
	uint32_t path = RAM32(ACTOR_PATH_PTR(0));
	for (;;) {
		int16_t pos = S16(ACTOR_PATH_POS(0));
		RAM16(ACTOR_PATH_POS(0)) += 4;
		uint16_t y = bus_r16(path + pos + 2);
		uint16_t x = bus_r16(path + pos);
		if (!(x & 0x8000)) {
			RAM16(ACTOR_X(0)) = x;
			RAM16(ACTOR_Y(0)) = y;
			return;
		}
		if (x == 0xFFFD) {
			RAM16(ACTOR_ANIM(0)) = y;
			set8(ACTORS_ANIM_NEW, 0);
			continue;
		}
		if (x == 0xFFFC || x == 0xFFFB) {
			if (RAM16(ACTOR_DEPTH_X(0)) != y) {
				RAM16(ACTOR_DEPTH_X(0)) = y;
				RAM16(ACTOR_DEPTH_Y(0)) = y;
				set8(ACTORS_DEPTH_NEW, 0);
			}
			if (x == 0xFFFB)
				continue;
			return;
		}
		if (x == 0xFFFE)
			return;
		RAM16(ACTOR_PATH_POS(0)) = 0;       /* $FFFF: loop */
	}
}

/* stop Shaggy's path once the entry just consumed is the end marker */
static void shaggy_path_check_end(void)
{
	uint32_t path = RAM32(ACTOR_PATH_PTR(0));
	uint16_t last = bus_r16(path + S16(ACTOR_PATH_POS(0)) - 4);
	if (last == 0xFFFE || (last == 0xFFFF && bit8(ACTORS_PATH_ONCE, 0))) {
		clr8(ACTORS_ON_PATH, 0);
		clr8(ACTORS_PATH_ONCE, 0);
	}
}

/* ------------------------------------------------------------------ verb bar (UI) */
/* draw a 10x7 block of the verb-bar names from ROM `src` at VRAM $AAB6 (page swap) */
static void verb_bar_draw_block(hle_ctx *ctx, uint32_t src)
{
	uint16_t base = RAM16(0xFF0802);
	uint16_t vaddr = 0xAAB6;
	for (int row = 0; row < 7; row++) {
		for (int col = 0; col < 10; col++) {
			ctx->d[0] = (ctx->d[0] & 0xFFFF0000u) | vaddr;
			set_w(&ctx->d[1], (uint16_t)(ROM16(src) + base));
			vram_write_word(ctx);
			src += 2;
			vaddr += 2;
		}
		vaddr += 0x6C;
		src += 0x6C;
	}
}

/* $005EEA-$00600C: C toggles between the two verb-bar pages with a 16-frame slide.
 * Returns false when the caller must skip to the given tail instead. */
enum tail { T_NONE, T_632C, T_6D24 };

static enum tail verb_bar_slide(hle_ctx *ctx)
{
	if ((int8_t)RAM8(VERB_BAR_SLIDE) < 0) {
		if (bit8(GAME_FLAGS_09DE, 2))
			return bit8(ACTORS_MOVING, 0) ? T_632C : T_6D24;
		if (bit8(GAME_STATE, 1))
			return T_632C;
		if (!pressed_now(PAD_C))
			return T_NONE;
		if (!bit8(GAME_FLAGS_09DE, 2) && !bit8(GAME_STATE, 1))
			verb_bar_draw_block(ctx, 0x03034C);
		RAM8(VERB_BAR_SLIDE) = 0x10;
	}
	if (bit8(VBLANK_FLAGS, 4))
		RAM16(0xFF0800) += 0x10;
	else
		RAM16(0xFF0800) -= 0x10;
	if (--RAM8(VERB_BAR_SLIDE) != 0)
		return T_NONE;
	if (!bit8(GAME_FLAGS_09DE, 2) && !bit8(GAME_STATE, 1))
		verb_bar_draw_block(ctx, 0x03004C);
	RAM8(VERB_BAR_SLIDE) = 0xFF;
	RAM8(VBLANK_FLAGS) ^= 0x10;
	return T_NONE;
}

/* $0062E8: confirm the verb under the menu cursor (redraws the highlighted verb) */
static void verb_menu_confirm(hle_ctx *ctx)
{
	clr8(VBLANK_FLAGS, 3);
	ctx->d[0] = 0;
	RAM16(0xFF06C2) = RAM16(0xFF06C0);
	hle_call68k(ctx, 0x005BDC);
	uint16_t obj = RAM16(0xFF06AE);
	set_w(&ctx->d[0], obj);
	if (!obj)
		return;
	uint16_t off = (uint16_t)((uint16_t)(obj - 3) * 0x1A);
	RAM16(0xFF06C2) = (uint16_t)(int16_t)(int8_t)RAM8(0xFF1200 + 0x16 + (int16_t)off);
	ctx->d[0] = 0;
	hle_call68k(ctx, 0x005BDC);
}

/* $00600C-$006292: verb menu open: move the menu cursor with the D-pad, A picks.
 * Returns true when it fell through to the B-button check at $006292. */
static bool verb_menu_cursor(hle_ctx *ctx)
{
	if (bit8(ACTORS_ON_PATH, 0))
		shaggy_path_check_end();
	if (pressed_now(PAD_A)) {
		verb_menu_confirm(ctx);
		return false;
	}
	if (S16(MENU_CURSOR_Y) >= 0xA8) {     /* on the verb bar: big horizontal steps */
		RAM16(MENU_STEP_Y) = 3;
		RAM16(MENU_STEP_X) = (held_before(PAD_L) || held_before(PAD_R)) ? 2 : 0x28;
	} else {
		RAM16(MENU_STEP_X) = 2;
		RAM16(MENU_STEP_Y) = 2;
	}
	if (!bit8(VBLANK_FLAGS, 7) && !bit8(0xFF0ABF, 0) && !bit8(GAME_FLAGS_09E8, 2) &&
	    !bit8(GAME_FLAGS_09E9, 4) && !bit8(GAME_FLAGS_09E9, 6)) {
		RAM16(ACTOR_ANIM(0)) = (uint16_t)(RAM16(SHAGGY_FACING) + 4);    /* stand still */
		set8(ACTORS_ANIM_NEW, 0);
	}
	if (held(PAD_L)) {
		RAM16(MENU_CURSOR_X) -= RAM16(MENU_STEP_X);
		if (S16(MENU_CURSOR_X) < -8)
			RAM16(MENU_CURSOR_X) = 0xFFF8;
	}
	if (held(PAD_R)) {
		RAM16(MENU_CURSOR_X) += RAM16(MENU_STEP_X);
		if (S16(MENU_CURSOR_X) > 0xF7)
			RAM16(MENU_CURSOR_X) = 0xF7;
	}
	if (held(PAD_U)) {
		if (S16(MENU_CURSOR_Y) >= 0xC0)
			RAM16(MENU_CURSOR_Y) = 0xBF;
		else {
			RAM16(MENU_CURSOR_Y) -= RAM16(MENU_STEP_Y);
			if (S16(MENU_CURSOR_Y) < 0)
				RAM16(MENU_CURSOR_Y) = 0;
		}
	}
	if (held(PAD_D)) {
		if (S16(MENU_CURSOR_Y) >= 0xA8 && !held_before(PAD_D))
			RAM16(MENU_CURSOR_Y) = 0xC0;
		RAM16(MENU_CURSOR_Y) += RAM16(MENU_STEP_Y);
		if (S16(MENU_CURSOR_Y) > 0xCF)
			RAM16(MENU_CURSOR_Y) = 0xCF;
	}
	if (S16(MENU_CURSOR_Y) >= 0xA8) {
		/* which of the 5+5 verb slots (1-12 with the page arrows) is under the cursor */
		bool page2 = bit8(VBLANK_FLAGS, 4);
		int16_t x = S16(MENU_CURSOR_X);
		if (!page2)
			x -= 8;
		int16_t slot = 1;
		for (x -= 0x28; x >= 0; x -= 0x28)
			slot++;
		if (slot > 6)
			slot = 6;
		if (S16(MENU_CURSOR_Y) > 0xC0)
			slot += 6;
		if (!page2) {
			if (slot > 5) {
				slot--;
				if (slot > 0xA)
					slot--;
			}
		} else {
			if (slot > 7)
				slot--;
			slot--;
		}
		RAM16(MENU_VERB_HOVER) = (uint16_t)slot;
		if (!page2 || slot == 5 || slot == 10)
			return false;
	}
	/* $006292: B on an object/hotspot */
	if (!pressed_now(PAD_B))
		return false;
	RAM16(0xFF06BE) = RAM16(0xFF06C0);
	if (!RAM16(0xFF06BE)) {
		RAM16(0xFF06BE) = RAM16(0xFF06C4);
		if (!RAM16(0xFF06BE))
			return false;
	}
	if (RAM16(0xFF06AE)) {
		set8(VBLANK_FLAGS, 7);
		return false;
	}
	if (bit8(VBLANK_FLAGS, 7))
		return false;
	verb_menu_confirm(ctx);
	return false;
}

/* ------------------------------------------------------------------ Shaggy walking */
static uint16_t shaggy_turn_anim(int dir)
{
	return ROM16(SHAGGY_TURN_TABLE + (uint16_t)(RAM16(SHAGGY_FACING) * 8 + 2 * dir));
}

static bool walk_blocked(hle_ctx *ctx)
{
	walk_blocked_probe(ctx);
	return !(ctx->sr & CC_Z);
}

/* $006960: auto-walk step.  Shaggy moves by (256-depth)*$155>>8 * dir_x / $100 in x and
 * (256-depth)*$AA>>8 * dir_y / $100 in y (8.8 directions, 16.16 positions) until he
 * passes ACTOR_TARGET; then the next queued waypoint (SHAGGY_WP_*) is started. */
static void shaggy_auto_step(void)
{
	RAM16(SHAGGY_IDLE_TIMER) = 0x12C;
	uint32_t sx = ((uint32_t)(uint16_t)(0x100 - RAM16(ACTOR_DEPTH_X(0))) * 0x155) >> 8;
	RAM32_W(ACTOR_X(0), RAM32(ACTOR_X(0)) + (uint32_t)((int32_t)(int16_t)sx * S16(SHAGGY_DIR_X)));
	uint32_t sy = ((uint32_t)(uint16_t)(0x100 - RAM16(ACTOR_DEPTH_Y(0))) * 0xAA) >> 8;
	RAM32_W(ACTOR_Y(0), RAM32(ACTOR_Y(0)) + (uint32_t)((int32_t)(int16_t)sy * S16(SHAGGY_DIR_Y)));

	int16_t x = S16(ACTOR_X(0)), tx = S16(ACTOR_TARGET_X(0));
	if (S16(SHAGGY_DIR_X) >= 0) {
		if (x >= tx) {
			RAM16(ACTOR_X(0)) = (uint16_t)tx;
			RAM16(SHAGGY_DIR_X) = 0;
		}
	} else if (x <= tx) {
		RAM16(SHAGGY_DIR_X) = 0;            /* (no snap when walking left) */
	}
	int16_t y = S16(ACTOR_Y(0)), ty = S16(ACTOR_TARGET_Y(0));
	if (S16(SHAGGY_DIR_Y) >= 0) {
		if (y < ty)
			return;
		RAM16(ACTOR_Y(0)) = (uint16_t)ty;
		RAM16(SHAGGY_DIR_Y) = 0;
	} else if (y <= ty) {
		RAM16(ACTOR_Y(0)) = (uint16_t)ty;
		RAM16(SHAGGY_DIR_Y) = 0;
	}
	if (RAM16(SHAGGY_DIR_X) || RAM16(SHAGGY_DIR_Y))
		return;
	/* arrived: next waypoint (the list is consumed from the end) */
	if (S16(SHAGGY_WP_COUNT) >= 0 && (int16_t)--RAM16(SHAGGY_WP_COUNT) >= 0) {
		uint16_t i = (uint16_t)(RAM16(SHAGGY_WP_COUNT) * 2);
		RAM16(ACTOR_TARGET_X(0)) = RAM16(SHAGGY_WP_X + i);
		RAM16(ACTOR_TARGET_Y(0)) = RAM16(SHAGGY_WP_X + 4 + i);
		RAM16(SHAGGY_DIR_X) = RAM16(SHAGGY_WP_X + 8 + i);
		RAM16(SHAGGY_DIR_Y) = RAM16(SHAGGY_WP_X + 12 + i);
		uint16_t face = RAM16(SHAGGY_WP_X + 16 + i);
		uint16_t rel = (uint16_t)(RAM16(ACTOR_ANIM(0)) - RAM16(SHAGGY_FACING));
		if (rel == 0)
			RAM16(ACTOR_ANIM(0)) = ROM16(SHAGGY_TURN_TABLE + (int16_t)(uint16_t)(RAM16(SHAGGY_FACING) * 8 + face * 2));
		else
			RAM16(ACTOR_ANIM(0)) = (uint16_t)(rel + face);
		RAM16(SHAGGY_FACING) = face;
		set8(ACTORS_ANIM_NEW, 0);
		return;
	}
	clr8(ACTORS_MOVING, 0);
	if (bit8(VBLANK_FLAGS, 7)) {
		RAM16(ACTOR_ANIM(0)) = (uint16_t)(RAM16(SHAGGY_FACING) + 4);
		set8(ACTORS_ANIM_NEW, 0);
	}
}

/* $006478-$006960: D-pad walking.  Moves the WALK_PROBE copy of Shaggy's position by the
 * D-pad, then accepts it if the pixel is free; when blocked, a straight move tries to
 * slide around the obstacle (up to 3 px sideways for vertical moves, 4 px for horizontal
 * ones) by starting a short auto-walk.  The labels follow the 68k code.
 * Returns true if the frame continues with the auto-walk step ($006960). */
static bool shaggy_walk_dpad(hle_ctx *ctx, uint16_t max_x, uint16_t max_y)
{
	uint32_t step;
	uint16_t anim, t;
	if ((RAM8(PAD1_CUR) & 0xF) != 0xF)
		goto dpad_held;
	/* no direction held: let a started turn finish, else idle animations */
	step = RAM32(SHAGGY_SPEED);
	if (bit8(SHAGGY_TURNING, 1)) goto walk_left;
	if (bit8(SHAGGY_TURNING, 0)) goto walk_right;
	step >>= 1;
	if (bit8(SHAGGY_TURNING, 2)) goto walk_up;
	if (bit8(SHAGGY_TURNING, 3)) goto walk_down;
	if (bit8(ACTORS_MOVING, 0))
		return true;
	if ((int16_t)--RAM16(SHAGGY_IDLE_TIMER) < 0) {
		RAM16(SHAGGY_IDLE_TIMER) = 0x12C;
		RAM16(ACTOR_ANIM(0)) = (uint16_t)(RAM16(SHAGGY_FACING) + 0x20);   /* bored animation */
		set8(ACTORS_ANIM_NEW, 0);
		goto probe;
	}
	if (S16(ACTOR_ANIM(0)) >= 0x20 && S16(ACTOR_ANIM(0)) < 0x24 && !bit8(ACTORS_ANIM_DONE, 0))
		goto probe;
	t = (uint16_t)(RAM16(SHAGGY_FACING) + 4);                              /* standing */
	if (RAM16(ACTOR_ANIM(0)) == t || bit8(ACTORS_ANIM_NEW, 0))
		goto probe;
	RAM16(ACTOR_ANIM(0)) = t;
	set8(ACTORS_ANIM_NEW, 0);
	goto probe;

dpad_held:
	step = RAM32(SHAGGY_SPEED);
	RAM16(SHAGGY_IDLE_TIMER) = 0x12C;
	if (!held(PAD_L))
		goto not_left;
walk_left:                                  /* $00656C */
	anim = RAM16(ACTOR_ANIM(0));
	if (anim != 1) {
		t = shaggy_turn_anim(1);
		step >>= 1;                         /* half speed while turning */
		if (anim == t)
			goto move_left;
		RAM16(ACTOR_ANIM(0)) = t;
		set8(ACTORS_ANIM_NEW, 0);
		set8(SHAGGY_TURNING, 1);
		goto after_left;
	}
	RAM16(SHAGGY_FACING) = 1;
	if (!bit8(ACTORS_ANIM_LOOPED, 0))
		goto after_left;
	clr8(SHAGGY_TURNING, 1);
move_left:
	RAM32_W(WALK_PROBE_X, RAM32(WALK_PROBE_X) - step);
	if ((int32_t)RAM32(WALK_PROBE_X) < 0)
		RAM32_W(WALK_PROBE_X, 0);
	goto after_left;
not_left:                                   /* $0065E4 */
	if (bit8(SHAGGY_TURNING, 1) && S16(ACTOR_ANIM(0)) <= 3)
		clr8(SHAGGY_TURNING, 1);
	if (!held(PAD_R))
		goto after_left;
walk_right:                                 /* $00660C */
	anim = RAM16(ACTOR_ANIM(0));
	if (anim != 0) {
		t = shaggy_turn_anim(0);
		step >>= 1;
		if (anim == t)
			goto move_right;
		RAM16(ACTOR_ANIM(0)) = t;
		set8(ACTORS_ANIM_NEW, 0);
		set8(SHAGGY_TURNING, 0);
		goto vertical;
	}
	RAM16(SHAGGY_FACING) = 0;
	if (!bit8(ACTORS_ANIM_LOOPED, 0))
		goto vertical;
	clr8(SHAGGY_TURNING, 0);
move_right:
	RAM32_W(WALK_PROBE_X, RAM32(WALK_PROBE_X) + step);
	if ((int16_t)max_x < S16(WALK_PROBE_X))
		RAM16(WALK_PROBE_X) = max_x;
	goto vertical;
after_left:                                 /* $006682 */
	if (bit8(SHAGGY_TURNING, 0) && S16(ACTOR_ANIM(0)) <= 3)
		clr8(SHAGGY_TURNING, 0);
vertical:                                   /* $00669E */
	step = RAM32(SHAGGY_SPEED);
	if (bit8(SHAGGY_TURNING, 1) || bit8(SHAGGY_TURNING, 0))
		goto probe;
	step >>= 1;                             /* vertical speed is half the horizontal */
	if (!held(PAD_U))
		goto not_up;
	if (held(PAD_L) || held(PAD_R))
		goto face_up;
walk_up:                                    /* $0066E2 */
	anim = RAM16(ACTOR_ANIM(0));
	if (anim == 2)
		goto face_up;
	t = shaggy_turn_anim(2);
	step >>= 1;
	if (anim == t)
		goto move_up;
	RAM16(ACTOR_ANIM(0)) = t;
	set8(ACTORS_ANIM_NEW, 0);
	set8(SHAGGY_TURNING, 2);
	goto probe;
face_up:                                    /* $006726 */
	RAM16(SHAGGY_FACING) = 2;
	if (!bit8(ACTORS_ANIM_LOOPED, 0))
		goto not_up;
	clr8(SHAGGY_TURNING, 2);
	clr8(SHAGGY_TURNING, 3);
move_up:                                    /* $00674A */
	if (bit8(GAME_FLAGS_09E9, 0))           /* inverted vertical controls */
		goto add_y;
sub_y:
	RAM32_W(WALK_PROBE_Y, RAM32(WALK_PROBE_Y) - step);
	if ((int32_t)RAM32(WALK_PROBE_Y) < 0)
		RAM16(WALK_PROBE_Y) = 0;
	goto probe;
not_up:                                     /* $00676C */
	if (!held(PAD_D))
		goto probe;
	if (held(PAD_L) || held(PAD_R))
		goto face_down;
walk_down:                                  /* $006790 */
	anim = RAM16(ACTOR_ANIM(0));
	if (anim == 3)
		goto face_down;
	t = shaggy_turn_anim(3);
	step >>= 1;
	if (anim == t)
		goto move_down;
	RAM16(ACTOR_ANIM(0)) = t;
	set8(ACTORS_ANIM_NEW, 0);
	set8(SHAGGY_TURNING, 3);
	goto probe;
face_down:                                  /* $0067D4 */
	RAM16(SHAGGY_FACING) = 3;
	if (!bit8(ACTORS_ANIM_LOOPED, 0))
		goto probe;
	clr8(SHAGGY_TURNING, 3);
	clr8(SHAGGY_TURNING, 2);
move_down:                                  /* $0067F8 */
	if (bit8(GAME_FLAGS_09E9, 0))
		goto sub_y;
add_y:
	RAM32_W(WALK_PROBE_Y, RAM32(WALK_PROBE_Y) + step);
	if ((int16_t)max_y < S16(WALK_PROBE_Y))
		RAM16(WALK_PROBE_Y) = max_y;

probe:                                      /* $00681A: validate the new position */
	clr8(ACTORS_MOVING, 0);
	ctx->d[2] = (ctx->d[2] & 0xFFFF0000u) | max_x;
	ctx->d[3] = (ctx->d[3] & 0xFFFF0000u) | max_y;
	if (!walk_blocked(ctx)) {
		RAM32_W(ACTOR_X(0), RAM32(WALK_PROBE_X));
		RAM32_W(ACTOR_Y(0), RAM32(WALK_PROBE_Y));
		return false;
	}
	pad_dir8(ctx);
	int16_t dir = (int16_t)ctx->d[0];
	if (dir <= 3)                           /* diagonals: just stop */
		return false;
	if (dir <= 5) {
		/* left/right blocked: look 1..4 px up, then 1..4 px down, for a free pixel */
		for (int k = 0; k < 4; k++) {
			RAM16(WALK_PROBE_Y) -= 1;
			if (!walk_blocked(ctx)) { RAM16(SHAGGY_DIR_Y) = 0xFF00; goto slide_y; }
		}
		RAM16(WALK_PROBE_Y) += 4;
		for (int k = 0; k < 4; k++) {
			RAM16(WALK_PROBE_Y) += 1;
			if (!walk_blocked(ctx)) { RAM16(SHAGGY_DIR_Y) = 0x0100; goto slide_y; }
		}
		RAM16(WALK_PROBE_X) += 1;           /* (dead store, the probe is reset next frame) */
		return false;
slide_y:
		RAM16(SHAGGY_DIR_X) = 0;
		RAM32_W(ACTOR_X(0), RAM32(WALK_PROBE_X));
		RAM16(ACTOR_TARGET_X(0)) = RAM16(WALK_PROBE_X);
		set8(ACTORS_MOVING, 0);
		RAM16(ACTOR_TARGET_Y(0)) = RAM16(WALK_PROBE_Y);
		return true;
	}
	/* up/down blocked: look 1..3 px left, then 1..3 px right */
	for (int k = 0; k < 3; k++) {
		RAM16(WALK_PROBE_X) -= 1;
		if (!walk_blocked(ctx)) { RAM16(SHAGGY_DIR_X) = 0xFF00; goto slide_x; }
	}
	RAM16(WALK_PROBE_X) += 3;
	for (int k = 0; k < 3; k++) {
		RAM16(WALK_PROBE_X) += 1;
		if (!walk_blocked(ctx)) { RAM16(SHAGGY_DIR_X) = 0x0100; goto slide_x; }
	}
	return false;
slide_x:
	RAM16(SHAGGY_DIR_Y) = 0;
	RAM16(ACTOR_TARGET_X(0)) = RAM16(WALK_PROBE_X);
	set8(ACTORS_MOVING, 0);
	RAM32_W(ACTOR_Y(0), RAM32(WALK_PROBE_Y));
	RAM16(ACTOR_TARGET_Y(0)) = RAM16(WALK_PROBE_Y);
	return true;
}


/* ------------------------------------------------------------------ tails */
static void actors_update_body(void);
static void verb_menu_open_check_body(void);

/* $006B28 body: keep Shaggy inside a 75..180 px horizontal and 50..101 px vertical band
 * of the 256x152 room view; flag a column/row stream when the camera crosses a cell. */
static void camera_follow_body(uint16_t max_x, uint16_t max_y)
{
	int16_t cam_max_x = (int16_t)(max_x - 0xFF);
	int16_t sx = S16(ACTOR_X(0)), d0;
	int16_t rel = (int16_t)(sx - S16(CAMERA_X));
	if (rel <= 0x4B) {
		d0 = (int16_t)(sx - 0x4B);
		if (d0 < 0)
			d0 = 0;
	} else if (rel >= 0xB5) {
		d0 = (int16_t)(sx - 0xB4);
		if (d0 > cam_max_x)
			d0 = cam_max_x;
	} else
		goto vertical;
	{
		int16_t newc = (int16_t)((uint16_t)d0 >> 3), oldc = (int16_t)(RAM16(CAMERA_X) >> 3);
		if (newc != oldc)
			set8(CAMERA_SCROLL_REQ, oldc < newc ? 0 : 1);
		RAM16(CAMERA_X) = (uint16_t)d0;
	}
vertical:;
	int16_t cam_max_y = (int16_t)(max_y - 0x97);
	int16_t sy = S16(ACTOR_Y(0));
	rel = (int16_t)(sy - S16(CAMERA_Y));
	if (rel <= 0x32) {
		d0 = (int16_t)(sy - 0x32);
		if (d0 < 0)
			d0 = 0;
	} else if (rel >= 0x66) {
		d0 = (int16_t)(sy - 0x65);
		if (d0 > cam_max_y)
			d0 = cam_max_y;
	} else
		return;
	int16_t newr = (int16_t)((uint16_t)d0 >> 3), oldr = (int16_t)(RAM16(CAMERA_Y) >> 3);
	if (newr != oldr)
		set8(CAMERA_SCROLL_REQ, oldr < newr ? 3 : 2);
	RAM16(CAMERA_Y) = (uint16_t)d0;
}

/* $006AE8 body: Shaggy's ACTORS_BEHIND bit from zone word bit 15 */
static void player_zone_flags_body(void)
{
	if (zone_word(RAM16(ACTOR_X(0)), RAM16(ACTOR_Y(0))) & 0x8000)
		set8(ACTORS_BEHIND, 0);
	else
		clr8(ACTORS_BEHIND, 0);
}

static uint16_t max_x_px(void) { return (uint16_t)(RAM16(MAP_WIDTH) * 8 - 1); }
static uint16_t max_y_px(void) { return (uint16_t)(RAM16(MAP_HEIGHT) * 8 - 1); }

static void tail_6D24(void) { actors_update_body(); }
static void tail_6C28(void) { verb_menu_open_check_body(); actors_update_body(); }
static void tail_6B28(uint16_t mx, uint16_t my)
{
	if (!bit8(GAME_FLAGS_09E8, 1))          /* 09E8 bit 1: camera locked */
		camera_follow_body(mx, my);
	tail_6C28();
}
static void tail_6AE8(uint16_t mx, uint16_t my) { player_zone_flags_body(); tail_6B28(mx, my); }

/* $005E4A: per-frame update of Shaggy (path playback, verb bar slide, verb menu cursor or
 * D-pad walking with collision, auto-walk), camera, verb-menu opening, Scooby and actors.
 * Called once per frame from vblank_main. */
HLE_FUNC(0x005E4A, player_update, 5036)
{
	shaggy_path_step();
	switch (verb_bar_slide(ctx)) {
	case T_6D24: tail_6D24(); return;
	case T_632C: goto walk;
	case T_NONE: break;
	}
	if (bit8(VBLANK_FLAGS, 3)) {             /* verb menu / pointer open */
		verb_menu_cursor(ctx);
		tail_6D24();
		return;
	}
walk:;                                       /* $00632C */
	uint16_t mx = max_x_px(), my = max_y_px();
	if (bit8(ACTORS_ON_PATH, 0)) {
		shaggy_path_check_end();
		tail_6B28(mx, my);
		return;
	}
	if (bit8(GAME_FLAGS_09E8, 2)) {
		if (!bit8(GAME_FLAGS_09EA, 0)) { tail_6C28(); return; }
		tail_6B28(mx, my);
		return;
	}
	if (bit8(0xFF0ABF, 0)) { tail_6D24(); return; }
	if (bit8(GAME_FLAGS_09E9, 4)) { tail_6C28(); return; }
	if (bit8(VBLANK_FLAGS, 7) && !bit8(ACTORS_MOVING, 0)) { tail_6D24(); return; }

	/* $0063EA: start from the current position, speed from the depth scale */
	RAM32_W(WALK_PROBE_X, RAM32(ACTOR_X(0)));
	RAM32_W(WALK_PROBE_Y, RAM32(ACTOR_Y(0)));
	uint32_t speed = (uint32_t)(uint16_t)(0x100 - RAM16(ACTOR_DEPTH_X(0))) * 0x155;
	RAM32_W(SHAGGY_SPEED, speed);
	int16_t s8 = (int16_t)(speed >> 8);
	if (!bit8(ACTORS_MOVING, 0)) {
		RAM16(SHAGGY_DIR_X) = 0x100;
		RAM16(SHAGGY_DIR_Y) = 0x100;
	}
	RAM32_W(SHAGGY_SPEED_X, (uint32_t)((int32_t)s8 * S16(SHAGGY_DIR_X)));
	RAM32_W(SHAGGY_SPEED_Y, (uint32_t)((int32_t)s8 * S16(SHAGGY_DIR_Y)));
	if (bit8(VBLANK_FLAGS, 7)) {             /* script running: only auto-walk */
		if (bit8(ACTORS_MOVING, 0))
			shaggy_auto_step();
		tail_6AE8(mx, my);
		return;
	}
	if (shaggy_walk_dpad(ctx, mx, my))
		shaggy_auto_step();
	tail_6AE8(mx, my);
}

/* Tail entries, only reached by branches inside $005E4A (kept so the 68k version of
 * player_update can run with C tails, and for difftest).  D2.w/D3.w = max x/y. */
HLE_FUNC(0x006AE8, player_zone_flags, 5184)
{
	tail_6AE8((uint16_t)ctx->d[2], (uint16_t)ctx->d[3]);
}

HLE_FUNC(0x006B28, camera_follow, 5000)
{
	tail_6B28((uint16_t)ctx->d[2], (uint16_t)ctx->d[3]);
}

HLE_FUNC(0x006C28, verb_menu_open_check, 4788)
{
	tail_6C28();
}

HLE_FUNC(0x006D24, actors_update, 4281)
{
	tail_6D24();
}

/* $006C28 body: A or B (newly pressed) opens the verb menu with the cursor on Shaggy */
static void verb_menu_open_check_body(void)
{
	if (bit8(VBLANK_FLAGS, 7) || bit8(GAME_STATE, 1))
		return;
	if (!pressed_now(PAD_A) && !pressed_now(PAD_B))
		return;
	set8(VBLANK_FLAGS, 3);
	clr8(GAME_FLAGS_09DE, 0);
	RAM16(MENU_STEP_X) = 2;
	RAM16(MENU_STEP_Y) = 2;
	RAM16(0xFF06C0) = 0;
	RAM16(0xFF06B4) = 0;
	RAM16(0xFF06B6) = 0;
	if (!bit8(GAME_FLAGS_09E9, 6)) {
		if (bit8(0xFF0ABF, 0))
			goto at_shaggy;
		if (!bit8(GAME_FLAGS_09E9, 4)) {
			if (!bit8(GAME_FLAGS_09E8, 2)) {
				RAM16(ACTOR_ANIM(0)) = (uint16_t)(RAM16(SHAGGY_FACING) + 4);
				set8(ACTORS_ANIM_NEW, 0);
			}
			goto at_shaggy;
		}
	}
	RAM16(MENU_CURSOR_X) = 0x80;
	RAM16(MENU_CURSOR_Y) = 0x50;
	return;
at_shaggy:
	RAM16(MENU_CURSOR_X) = (uint16_t)(RAM16(ACTOR_X(0)) - RAM16(CAMERA_X));
	RAM16(MENU_CURSOR_Y) = (uint16_t)(RAM16(ACTOR_Y(0)) - RAM16(CAMERA_Y) + 0x10 - 0x3C);
}

/* ------------------------------------------------------------------ Scooby + actors */
/* $006DAE: Scooby's scripted path (same format as Shaggy's; no depth commands) */
static void scooby_path_step(void)
{
	if ((int8_t)--RAM8(ACTOR_PATH_DELAY(1)) >= 0)
		return;
	RAM8(ACTOR_PATH_DELAY(1)) = RAM8(ACTOR_PATH_RELOAD(1));
	uint32_t path = RAM32(ACTOR_PATH_PTR(1));
	for (;;) {
		int16_t pos = S16(ACTOR_PATH_POS(1));
		RAM16(ACTOR_PATH_POS(1)) += 4;
		uint16_t y = bus_r16(path + pos + 2);
		uint16_t x = bus_r16(path + pos);
		if (!(x & 0x8000)) {
			RAM16(ACTOR_X(1)) = x;
			RAM16(ACTOR_Y(1)) = y;
			return;
		}
		if (x == 0xFFFD) {
			RAM16(ACTOR_ANIM(1)) = y;
			set8(ACTORS_ANIM_NEW, 1);
			continue;
		}
		RAM16(ACTOR_PATH_POS(1)) = 0;
		if (x == 0xFFFE || bit8(ACTORS_PATH_ONCE, 1)) {
			clr8(ACTORS_ON_PATH, 1);
			clr8(ACTORS_PATH_ONCE, 1);
			return;
		}
	}
}

/* $006EC0: Scooby's auto-walk step (speeds $12A/$95 instead of Shaggy's $155/$AA) */
static void scooby_auto_step(void)
{
	uint32_t sx = ((uint32_t)(uint16_t)(0x100 - RAM16(ACTOR_DEPTH_X(1))) * 0x12A) >> 8;
	RAM32_W(ACTOR_X(1), RAM32(ACTOR_X(1)) + (uint32_t)((int32_t)(int16_t)sx * S16(SCOOBY_DIR_X)));
	uint32_t sy = ((uint32_t)(uint16_t)(0x100 - RAM16(ACTOR_DEPTH_Y(1))) * 0x95) >> 8;
	RAM32_W(ACTOR_Y(1), RAM32(ACTOR_Y(1)) + (uint32_t)((int32_t)(int16_t)sy * S16(SCOOBY_DIR_Y)));

	int16_t x = S16(ACTOR_X(1)), tx = S16(ACTOR_TARGET_X(1));
	if (S16(SCOOBY_DIR_X) >= 0) {
		if (x >= tx) {
			RAM16(ACTOR_X(1)) = (uint16_t)tx;
			RAM16(SCOOBY_DIR_X) = 0;
		}
	} else if (x <= tx) {
		RAM16(SCOOBY_DIR_X) = 0;
	}
	int16_t y = S16(ACTOR_Y(1)), ty = S16(ACTOR_TARGET_Y(1));
	if (S16(SCOOBY_DIR_Y) >= 0) {
		if (y < ty)
			return;
		RAM16(ACTOR_Y(1)) = (uint16_t)ty;
		RAM16(SCOOBY_DIR_Y) = 0;
	} else if (y <= ty) {
		RAM16(ACTOR_Y(1)) = (uint16_t)ty;
		RAM16(SCOOBY_DIR_Y) = 0;
	}
	if (RAM16(SCOOBY_DIR_X) || RAM16(SCOOBY_DIR_Y))
		return;
	if (S16(SCOOBY_WP_COUNT) >= 0 && (int16_t)--RAM16(SCOOBY_WP_COUNT) >= 0) {
		uint16_t i = (uint16_t)(RAM16(SCOOBY_WP_COUNT) * 2);
		RAM16(ACTOR_TARGET_X(1)) = RAM16(SCOOBY_WP_X + i);
		RAM16(ACTOR_TARGET_Y(1)) = RAM16(SCOOBY_WP_X + 4 + i);
		RAM16(SCOOBY_DIR_X) = RAM16(SCOOBY_WP_X + 8 + i);
		RAM16(SCOOBY_DIR_Y) = RAM16(SCOOBY_WP_X + 12 + i);
		uint16_t face = RAM16(SCOOBY_WP_X + 16 + i);
		uint16_t rel = (uint16_t)(RAM16(ACTOR_ANIM(1)) - RAM16(SCOOBY_FACING));
		if (rel == 0)
			RAM16(ACTOR_ANIM(1)) = ROM16(SCOOBY_TURN_TABLE + (int16_t)(uint16_t)(RAM16(SCOOBY_FACING) * 8 + face * 2));
		else
			RAM16(ACTOR_ANIM(1)) = (uint16_t)(rel + face);
		RAM16(SCOOBY_FACING) = face;
		set8(ACTORS_ANIM_NEW, 1);
		RAM16(SCOOBY_ANIM_LOCK) = 0xFFFF;
		return;
	}
	/* $007022: arrived */
	uint16_t f = RAM16(SCOOBY_FACING);
	if (bit8(VBLANK_FLAGS, 7)) {
		RAM16(ACTOR_ANIM(1)) = (uint16_t)(f + 4);
		set8(ACTORS_ANIM_NEW, 1);
		clr8(ACTORS_MOVING, 1);
		return;
	}
	set8(GAME_FLAGS_09DE, 5);                /* pose at the idle spot next */
	RAM16(ACTOR_ANIM(1)) = ROM16(SCOOBY_POSE_TABLE + (int16_t)(uint16_t)(f * 8 + RAM16(SCOOBY_POSE_FACING) * 2));
	RAM16(SCOOBY_ANIM_LOCK) = 0xFFFF;
	set8(ACTORS_ANIM_NEW, 1);
}

/* Scooby: ride along / scripted path / wander timer / idle pose / auto-walk ($006D24-$00707E) */
static void scooby_update(void)
{
	if (bit8(GAME_FLAGS_09EB, 2))
		return;
	if (bit8(GAME_FLAGS_09EB, 6)) {          /* Scooby glued to Shaggy */
		RAM16(ACTOR_X(1)) = RAM16(ACTOR_X(0));
		RAM16(ACTOR_Y(1)) = RAM16(ACTOR_Y(0));
		uint16_t a = RAM16(SHAGGY_FACING);
		if ((RAM8(PAD1_CUR) & 0xF) == 0xF || bit8(VBLANK_FLAGS, 3))
			a += 4;
		if (RAM16(ACTOR_ANIM(1)) != a) {
			RAM16(ACTOR_ANIM(1)) = a;
			set8(ACTORS_ANIM_NEW, 1);
		}
		return;
	}
	if (bit8(ACTORS_ON_PATH, 1)) {
		scooby_path_step();
		return;
	}
	if (!bit8(ACTORS_MOVING, 1)) {
		if (S16(SCOOBY_WANDER_TIMER) >= 0)
			RAM16(SCOOBY_WANDER_TIMER) -= 1;
		return;
	}
	if (bit8(GAME_FLAGS_09DE, 5)) {          /* arrived at an idle spot: strike the pose */
		if (bit8(ACTORS_ANIM_NEW, 1) || !bit8(ACTORS_ANIM_DONE, 1))
			return;
		RAM16(ACTOR_ANIM(1)) = (uint16_t)(RAM16(SCOOBY_POSE_ANIM) + RAM16(SCOOBY_POSE_FACING));
		RAM16(SCOOBY_FACING) = RAM16(SCOOBY_POSE_FACING);
		RAM16(SCOOBY_ANIM_LOCK) = 0xFFFF;
		set8(ACTORS_ANIM_NEW, 1);
		clr8(ACTORS_MOVING, 1);
		clr8(GAME_FLAGS_09DE, 5);
		return;
	}
	scooby_auto_step();
}

/* $00715A: depth scale for one actor from the room's per-row depth table:
 * linear interpolation (in 1/16 units) between the defined rows around the actor's row.
 * `last_prev_row` mirrors D4, which carries over between actors. */
static uint16_t actor_depth(uint16_t y, uint16_t *last_prev_row)
{
	int16_t row = (int16_t)(y >> 3);
	uint32_t p = RAM32(ROOM_DEPTH_ROWS);
	int16_t prev_v = 0, r = 0;
	do {                                    /* rows 0..row-1 (row 0 always consumed) */
		int16_t v = (int16_t)bus_r16(p);
		p += 2;
		if (v >= 0) {
			prev_v = v;
			*last_prev_row = (uint16_t)r;
		}
		r++;
	} while (r < row);
	int16_t next_v = 0, next_r = S16(MAP_HEIGHT);
	if (r < next_r) {
		do {
			int16_t v = (int16_t)bus_r16(p);
			p += 2;
			if (v >= 0) {
				next_v = v;
				next_r = r;
				break;
			}
			r++;
		} while (r < S16(MAP_HEIGHT));
	}
	int16_t below = (int16_t)(next_r * 8 + 7 - y);         /* px above the next row's bottom */
	uint16_t d7 = (uint16_t)(next_v << 4);
	int16_t dv = (int16_t)(uint16_t)((uint16_t)(next_v - prev_v) << 4);
	int16_t span = (int16_t)(uint16_t)((uint16_t)(next_r - *last_prev_row) << 3);
	int32_t prod = (int32_t)dv * below;
	uint16_t q = (uint16_t)prod;            /* divs overflow leaves the dividend */
	if (span != 0) {                        /* (a zero span would be a 68k divide trap) */
		int32_t qq = prod / span;
		if (qq >= -32768 && qq <= 32767)
			q = (uint16_t)qq;
	}
	return (uint16_t)(d7 - q);
}

static void actors_update_body(void)
{
	scooby_update();

	/* $00707E: Scooby's "behind" flag */
	if (zone_word(RAM16(ACTOR_X(1)), RAM16(ACTOR_Y(1))) & 0x8000)
		set8(ACTORS_BEHIND, 1);
	else
		clr8(ACTORS_BEHIND, 1);

	if (bit8(GAME_FLAGS_09E9, 3)) {          /* actor 2 carried next to Shaggy */
		RAM8(ACTORS_BEHIND) = (uint8_t)((RAM8(ACTORS_BEHIND) & 0xF0) | 3);
		RAM32_W(ACTOR_X(2), RAM32(ACTOR_X(0)));
		RAM32_W(ACTOR_Y(2), RAM32(ACTOR_Y(0)));
		bool right_half = S16(ACTOR_X(0)) >= 0xF3;
		RAM16(ACTOR_Y(2)) -= 0x14;
		RAM16(ACTOR_X(2)) += 0x14;
		RAM16(ACTOR_DEPTH_Y(2)) = right_half ? 0 : 0x80;
		RAM16(ACTOR_DEPTH_X(2)) = right_half ? 0x80 : 0;
		if (S16(ACTOR_X(2)) > 0x160)
			RAM16(ACTOR_X(2)) = 0x160;
		/* (the original also has a no-op "if x3 == $150 then x3 = $150") */
	}

	/* depth of every actor that follows the room's depth table */
	uint16_t last_prev_row = (uint16_t)(RAM16(MAP_WIDTH) * 2);   /* stale D4 */
	for (int i = 5; i >= 0; i--) {
		if (!bit8(ACTORS_DEPTH_ON, i) || bit8(GAME_FLAGS_09E9, 1))
			continue;
		uint16_t d = actor_depth(RAM16(ACTOR_Y(i)), &last_prev_row);
		if (RAM16(ACTOR_DEPTH_X(i)) != d) {
			RAM16(ACTOR_DEPTH_X(i)) = d;
			set8(ACTORS_DEPTH_NEW, i);
		}
		if (RAM16(ACTOR_DEPTH_Y(i)) != d) {
			RAM16(ACTOR_DEPTH_Y(i)) = d;
			set8(ACTORS_DEPTH_NEW, i);
		}
	}
	if (bit8(GAME_FLAGS_09E8, 3)) {          /* copy Shaggy's depth to actor $FF08B0 */
		uint16_t i2 = (uint16_t)(RAM16(0xFF08B0) * 2);
		RAM16(ACTOR_DEPTH_X(0) + (int16_t)i2) = RAM16(ACTOR_DEPTH_X(0));
		RAM16(ACTOR_DEPTH_Y(0) + (int16_t)i2) = RAM16(ACTOR_DEPTH_Y(0));
	}

	/* $00723A: actors 2-5.  `pos` is the 68k's A0: it normally points at ACTOR_X(i), but
	 * after a path step the original leaves it pointing into the path table, and the rest
	 * of the loop (and the following actors) then use that pointer.  Reproduced as is. */
	uint32_t pos = ACTOR_X(2), tgt = ACTOR_TARGET_X(2);
	for (int i = 2; i < 6; i++, pos += 4, tgt += 2) {
		if (bit8(GAME_FLAGS_09E9, 3) && i < 4)
			continue;
		if (!bit8(ACTORS_ACTIVE, i))
			continue;
		if (bit8(ACTORS_MOVING, i)) {
			/* step towards the target; the clamp compares longs, so the next actor's
			 * target word acts as the fraction of this one's */
			uint32_t step = bus_r32(pos + 0x38);
			int16_t t = (int16_t)bus_r16(tgt), c = (int16_t)bus_r16(pos);
			if (t != c) {
				if (t > c) {
					bus_w32(pos, bus_r32(pos) + step);
					if (!((int32_t)bus_r32(tgt) > (int32_t)bus_r32(pos)))
						bus_w32(pos, bus_r32(tgt));
				} else {
					bus_w32(pos, bus_r32(pos) - step);
					if (!((int32_t)bus_r32(tgt) < (int32_t)bus_r32(pos)))
						bus_w32(pos, bus_r32(tgt));
				}
			}
			step = bus_r32(pos + 0x4C);
			t = (int16_t)bus_r16(tgt + 0xC);
			c = (int16_t)bus_r16(pos + 0x18);
			if (t != c) {
				if (t > c) {
					bus_w32(pos + 0x18, bus_r32(pos + 0x18) + step);
					if (!((int32_t)bus_r32(tgt + 0xC) > (int32_t)bus_r32(pos + 0x18)))
						bus_w32(pos + 0x18, bus_r32(tgt + 0xC));
				} else {
					bus_w32(pos + 0x18, bus_r32(pos + 0x18) - step);
					if (!((int32_t)bus_r32(tgt + 0xC) < (int32_t)bus_r32(pos + 0x18)))
						bus_w32(pos + 0x18, bus_r32(tgt + 0xC));
				}
			}
		} else if (bit8(ACTORS_ON_PATH, i)) {
			if ((int8_t)--RAM8(ACTOR_PATH_DELAY(i)) < 0) {
				RAM8(ACTOR_PATH_DELAY(i)) = RAM8(ACTOR_PATH_RELOAD(i));
				pos = RAM32(ACTOR_PATH_PTR(i));
				for (;;) {
					int16_t off = S16(ACTOR_PATH_POS(i));
					RAM16(ACTOR_PATH_POS(i)) += 4;
					uint16_t y = bus_r16(pos + off + 2);
					uint16_t x = bus_r16(pos + off);
					if (!(x & 0x8000)) {
						RAM16(ACTOR_X(i)) = x;
						RAM16(ACTOR_Y(i)) = y;
						break;
					}
					if (x == 0xFFFD) {
						RAM16(ACTOR_ANIM(i)) = y;
						set8(ACTORS_ANIM_NEW, i);
						continue;
					}
					RAM16(ACTOR_PATH_POS(i)) = 0;
					if (x == 0xFFFE || bit8(ACTORS_PATH_ONCE, i)) {
						clr8(ACTORS_ON_PATH, i);
						clr8(ACTORS_PATH_ONCE, i);
						break;
					}
				}
			}
		}
		/* $007374 */
		if (bus_r16(pos) == bus_r16(tgt) && bus_r16(pos + 0x18) == bus_r16(tgt + 0xC))
			clr8(ACTORS_MOVING, i);
		if (!bit8(ACTORS_NO_ZONE, i)) {
			if (zone_word(bus_r16(pos), bus_r16(pos + 0x18)) & 0x8000)
				set8(ACTORS_BEHIND, i);
			else
				clr8(ACTORS_BEHIND, i);
		}
	}
}
