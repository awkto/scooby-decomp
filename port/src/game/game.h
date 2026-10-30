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

#endif
