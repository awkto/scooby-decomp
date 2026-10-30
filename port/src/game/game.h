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
