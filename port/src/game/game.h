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

#endif
