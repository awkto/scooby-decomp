/* Decompiled game functions callable from other C code.
 * Every function here is also an HLE_FUNC (it replaces the 68k routine at its address)
 * and takes the 68k register context, so C->C calls keep the original register contract. */
#ifndef SCOOBY_GAME_H
#define SCOOBY_GAME_H
#include "hle.h"

/* io.c */
void z80_bus_request(hle_ctx *ctx);    /* $1F6D7A */
void z80_bus_release(hle_ctx *ctx);    /* $1F6D8E */
void pad_read(hle_ctx *ctx);           /* $009D34 */

/* vdp.c */
void vram_write_words(hle_ctx *ctx);   /* $00992C */
void vram_write_word(hle_ctx *ctx);    /* $009964 */
void vram_fill_words(hle_ctx *ctx);    /* $009BE4 */

/* util.c */
void str_copy(hle_ctx *ctx);           /* $005B56 */

/* room.c (room/scene loader, issue #18; see docs/subsystems/room.md) */
void room_load(hle_ctx *ctx);          /* $00758A */
void room_draw_view(hle_ctx *ctx);     /* $007950 */
void font_tiles_load(hle_ctx *ctx);    /* $007A2A */
void room_setup_actors(hle_ctx *ctx);  /* $007A8C */
void game_state_init(hle_ctx *ctx);    /* $007F22 */
void object_table_init(hle_ctx *ctx);  /* $007F66 */
void pad_direction(hle_ctx *ctx);      /* $00750A */
void zone_probe_cursor(hle_ctx *ctx);  /* $0073D6 */
void zone_probe_point(hle_ctx *ctx);   /* $007476 */
void overlay_apply(hle_ctx *ctx);      /* $007D54 */
void objects_update(hle_ctx *ctx);     /* $007C04 */

/* RAM variables (see docs/porting.md for naming) */
#define PAD1_CUR    0xFF09E0   /* byte: pad 1 state, SACBRLDU, active low */
#define PAD2_CUR    0xFF09E1
#define PAD1_PREV   0xFF09E2
#define PAD2_PREV   0xFF09E3
#define PAD1_LATCH  0xFF09E4   /* compared against to accumulate edge bits */
#define PAD2_LATCH  0xFF09E5
#define PAD1_EDGE   0xFF09E6   /* byte: accumulated changed bits (cleared by the consumer) */
#define PAD2_EDGE   0xFF09E7

/* ---- mainloop.c (#12): interrupt hooks, frame timing, main loop (docs/subsystems/mainloop.md) */
void vblank_light(hle_ctx *ctx);       /* $00A4A4 */
void vblank_main(hle_ctx *ctx);        /* $00A65C */
void vblank_title_fade(hle_ctx *ctx);  /* $00A38C */
void title_anim(hle_ctx *ctx);         /* $00879E */
void hblank_nop(hle_ctx *ctx);         /* $01006A */
/* reset.c (#12): boot and scene flow (the HLE_RANGE pieces of Reset are not callable) */
void factory_test(hle_ctx *ctx);       /* $00069A */
void game_reset(hle_ctx *ctx);         /* $00A27A */
void logo5_frame(hle_ctx *ctx);        /* $000BC8 */

#define VBLANK_HOOK       0xFF0000   /* long: code pointer called by the VBlank vector */
#define HBLANK_HOOK       0xFF0004   /* long: code pointer called by the HBlank vector */
#define VBLANK_FLAGS      0xFF09DC   /* byte, VBF_* */
#define   VBF_WAITING       0x01     /* set by wait_vblank, cleared by every VBlank handler */
#define   VBF_OBJ_TIMERS    0x04     /* vblank_main runs the object timers (sub_007C04) */
#define   VBF_POINTER       0x08     /* main loop: pointer (verb/cursor) mode, else walk mode */
#define   VBF_INVENTORY     0x10     /* verb bar shows the inventory instead of the verbs */
#define   VBF_HUD_DMA       0x20     /* request: VBlank uploads the HUD/inventory (sub_005C8E) */
#define   VBF_FRAME_WORK    0x40     /* vblank_main does the game-side frame work (+ re-entry guard) */
#define   VBF_SCRIPT        0x80     /* a room/event script is running: main loop skips input */
#define VBLANK_FLAGS_PREV 0xFF09DD   /* copy of VBLANK_FLAGS from the previous main-loop pass */
#define SCRIPT_FLAGS2     0xFF0ACA   /* byte */
#define   SF2_START_PRESSED 0x01     /* set by VBlank while Start is held (skip cutscene/text) */
#define VBL_COUNTDOWN_B   0xFF0AC7   /* byte: decremented every VBlank until it is negative */
#define FRAME_COUNTDOWN   0xFF08AE   /* word: decremented every VBlank until negative (pacing) */
#define TITLE_STRIP_INDEX 0xFF000C   /* word: next word of the $008828 title strip loop */
#define TITLE_STRIP_TIMER 0xFF000E   /* word: frames until the strip scrolls again */
#define TITLE_FADE_DELAY  0xFF0016   /* word: frames until the next title "lightning" flash; <0 while fading */
#define TITLE_FADE_STEP   0xFF0018   /* word: byte offset into the fade table $00A4E8 (12..0) */
#define TITLE_FADE_TICK   0xFF001A   /* word: frames until the next fade step */
#define TITLE_SCROLL_DIR  0xFF001C   /* byte: 0 = TITLE_SCROLL counting up, 1 = down */
#define TITLE_SCROLL      0xFF001D   /* byte: title sprite tile offset, ping-pongs 0..$40 */
#define TITLE_PIECE_VRAM  0xFF0810   /* word: plane offset of the flashed title piece */
#define TITLE_PIECE_W     0xFF0812   /* word: its width in words */
#define TITLE_PIECE_H     0xFF0814   /* word: its height - 1 */

/* ---- room / scene state (room.c; docs/formats.md "Rooms") */
#define OBJECTS          0xFF1200   /* object table: 0x1A-byte records, see docs/subsystems/room.md */
#define OBJ_SIZE         0x1A
#define OBJ_LAST         0xFF06F4   /* word: index of the last object (count - 1) */
#define ROOM_INDEX       0xFF06AC   /* word: current room number (first room = 2; 1 = "inventory") */
#define MYSTERY_IDX      0xFF06AA
#define MYSTERY_HDR      0xFF068E   /* long: active mystery header pointer */
#define ROOM_DATA_PTR    0xFF0692   /* long: per-row table after the zone map */
#define ROOM_ENTRY_TBL   0xFF0696   /* long: 8 x (x,y) entry points, tile coords */
#define ROOM_SPAWN_TBL   0xFF069A   /* long: 8 x spawn longs (-1 terminated) + 2 waypoints */
#define ROOM_ENTRY_OFS   0xFF069E   /* word: entry point number * 4 (set by script before loading) */
#define ROOM_SPAWN_COUNT 0xFF06A0   /* word: number of valid spawn longs (0..8) */
#define ROOM_NAME_PTR    0xFF06D2   /* long */
#define MAP_WIDTH        0xFF06C6   /* word, cells */
#define MAP_HEIGHT       0xFF06C8
#define CAM_X            0xFF06CA   /* word: camera x in pixels (multiple of 8 after a load) */
#define CAM_Y            0xFF06CE
#define SHAGGY_X         0xFF04DC   /* 16.16 */
#define SHAGGY_Y         0xFF04F4
#define LZSS_RING        0xFF3000
#define DECOMP_BUF       0xFF4000
#define PLANEB_TMP       0xFF7000
#define PLANEA_TMP       0xFF8C00
#define PLANEB_NAMES     0xFFA800
#define PLANEA_NAMES     0xFFC400
#define ZONE_MAP         0xFFE000
#define FONT_BASE        0xFF068C

#endif
