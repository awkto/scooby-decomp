/* Title screen, menus, password screens, debug editors, sound test (GitLab #21).
 * Shared names and helpers of menus.c, menu_password.c, menu_debug.c and soundtest.c.
 * See docs/subsystems/menus.md for the map of the screens and how the code is split. */
#ifndef SCOOBY_MENUS_H
#define SCOOBY_MENUS_H
#include "game.h"
#include "cc68k.h"

/* ---- 68k helpers that other subsystems own: called through hle_call68k */
#define A_VDP_COPY       0x00992C   /* vdp_copy_to_vram: d0 = VRAM addr, a0 = src, d1 = words */
#define A_VDP_WRITE_WORD 0x009964   /* d0 = VRAM addr, d1 = word */
#define A_VDP_CLEAR      0x009C58   /* vdp_clear_vram: d0 = addr, d1 = words */
#define A_VDP_FILL       0x009BE4   /* vdp_fill_vram: d0 = addr, d1 = words, d2 = word */
#define A_VDP_DMA        0x0099F6   /* vdp_dma_vram: d0 = VRAM addr, d1 = words, a0 = source */
#define A_SOUND_PLAY     0x009762   /* sound_play: d0.l = sound id (saves d0/a0) */
#define A_SOUND_STOP_ALL 0x1F6F52
#define A_PALETTE_READ   0x009DE0   /* palette_read_cram (waits for VBlank) */
#define A_PALETTE_FADE   0x009E7E   /* a0 = target palette (waits for VBlank) */
#define A_WAIT_VBLANK    0x00974C
#define A_WAIT_VBLANKS   0x009742   /* d0.w + 1 frames */
#define A_SPRITES_E40C   0x00E40C   /* sprite subsystem: scaled sprite draw, step 1 */
#define A_SPRITES_F272   0x00F272   /*   ... step 2 */
#define A_TEXT_9F58      0x009F58   /* text subsystem: draw string a0 at (d1, d2) with bit 7 of the font base set */
#define A_TEXT_9F46      0x009F46   /* same, d1 preserved, a0 = string (moveml d1/a0) */
#define A_TEXT_9F30      0x009F30   /* center string a0 at row d2 (d1 = 1 keeps) */
#define A_TEXT_9F72      0x009F72   /* draw_text_plane_a */

/* ---- ROM tables / data */
#define ROM_PAD_CHEAT      0x010118   /* 11 bytes: A B A C A B Left Right A B Left (active high) */
#define ROM_ALPHABET       0x00871E   /* 32 password characters */
#define ROM_CURSOR_BLINK   0x008F14   /* 4 words: cursor sprite tile offsets */
#define ROM_TITLE_SAT      0x0085F8   /* 13-sprite title sprite table (0x68 bytes) */
#define ROM_MENU_SCREENS   0x0085DC   /* table: main menu actions (3 longs) */
#define ROM_MENU_SCREENS_P 0x0085E8   /* table: pause menu actions (4 longs) */
#define MENU_OPEN_ADDR     0x00851E   /* menu_open: build and run the title / pause menu */
#define ROM_STRIP_LOOP     0x008828   /* 39 words of the title strip loop */

/* ---- RAM names.  The menus use the sprite attribute buffer ram_sat_buf ($FF0008..) as scratch
 * while the title/menus run: no actors exist then, so nothing builds the sprite table. */
#define MENU_CURSOR_TILE   0xFF0008   /* word: first tile of the cursor sprite art (VRAM tile index) */
#define MENU_ROOM          0xFF000A   /* word: room the game will start in (= ROOM_INDEX; the DEBUG ROOMS editor changes it) */
#define MENU_BAT_DEPTH_A   0xFF0010   /* word: scale depth of the title bat frame (counts down by $20), shares the byte below */
#define MENU_PAD_PREV      0xFF0010   /* byte: pad 1 as of the previous menu frame (edge detection) */
#define MENU_BAT_DEPTH_B   0xFF0012   /* word: scale depth of the second title piece */
#define PW_RAW             0xFF001E   /* 32 bytes: raw password bytes r[0..31] (ram_sat_buf+22) */
#define PW_SYM             0xFF003E   /* 50+ bytes: 5-bit password symbols (ram_sat_buf+54) */
#define CHEAT_PROGRESS     0xFF08BA   /* word: how many cheat keys have been matched */
#define TEXT_BUF           0xFF08BC   /* password text, 2 rows x 31 bytes (also the status-line buffer in play) */
#define PWTEXT_SIZE        62
#define MYSTERY_IDX_W      0xFF06AA
#define MENU_FLAGS         0xFF09ED   /* byte: MF_* */
#define   MF_GAME_STARTED    0x01     /* a game was started: the menu is the pause menu */
#define   MF_DEBUG           0x02     /* cheat code entered: pause menu has DEBUG ROOMS / OBJECTS */
#define   MF_FROM_PASSWORD   0x04     /* game started from a password */
#define   MF_BATS_DONE       0x10     /* title bat animation over / frame scalers use depth 0 */
#define SCREEN_FLAGS       0xFF09EB   /* byte: bit 1 no camera scroll in vblank_main; bit 3, 4, 7: see title_screen */
#define GAME_STATE         0xFF2A00   /* story-state block (docs/formats.md); first 29 bytes are in a password */
#define PAD_REF            0xFF09E4   /* byte: pad_read's reference for edge accumulation */
#define MUSIC_CUR          0xFF06A6   /* long: id of the current music */

/* pad bits (active low) */
#define PAD_U 0x01
#define PAD_D 0x02
#define PAD_L 0x04
#define PAD_R 0x08
#define PAD_B 0x10
#define PAD_C 0x20
#define PAD_A 0x40
#define PAD_S 0x80

/* ---- shared helpers */
/* sound_play(id) through the 68k wrapper (saves d0/a0) */
static inline void menu_sound(hle_ctx *ctx, uint32_t id)
{
	ctx->d[0] = id;
	hle_call68k(ctx, A_SOUND_PLAY);
}
/* vdp_dma_vram */
static inline void menu_dma(hle_ctx *ctx, uint16_t vram, uint16_t words, uint32_t src)
{
	set_w(&ctx->d[0], vram);
	set_w(&ctx->d[1], words);
	ctx->a[0] = src;
	hle_call68k(ctx, A_VDP_DMA);
}
/* movem.l d0-d7/a0-a6,-(sp) / bsr $E40C / bsr $F272 / movem.l (sp)+,...: redraw the cursor
 * sprites (scale depth zero) and restore every register */
static inline void menu_sprites_refresh(hle_ctx *ctx)
{
	hle_ctx save = *ctx;
	hle_call68k(ctx, A_SPRITES_E40C);
	hle_call68k(ctx, A_SPRITES_F272);
	uint16_t sr = ctx->sr;
	*ctx = save;
	ctx->sr = sr;           /* movem leaves the flags of the last callee */
}
/* edge test of the menu loops: button `bit` is down now and was up in `prev` */
static inline bool menu_pressed(uint8_t pad, uint8_t prev, uint8_t bit)
{
	return !(pad & bit) && (prev & bit);
}
/* the password screen and the sound test also accept the edge pad_read accumulated between
 * frames (a tap shorter than a frame): then the edge byte is cleared and pad_ref re-armed */
static inline bool menu_pressed_edge(uint8_t pad, uint8_t prev, uint8_t bit)
{
	if (pad & bit)
		return false;
	if ((prev & bit) || (RAM8(PAD1_EDGE) & bit)) {
		RAM8(PAD1_EDGE) = 0;
		RAM8(PAD_REF) = pad;
		return true;
	}
	return false;
}

/* menu_password.c: codec glue + password entry screen */
void password_show(hle_ctx *ctx);          /* $008660 */
void menu_refresh_planes(hle_ctx *ctx);    /* $008F1C */
void menu_title_sprites(hle_ctx *ctx);     /* $008F40 */

#endif
