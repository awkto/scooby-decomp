/* Sprite / actor animation subsystem (GitLab #15): animation scripts, frame decoding,
 * depth scaling, tile DMA and the sprite attribute table.
 * Data formats: docs/formats.md "Actor sprites"; functions: docs/subsystems/sprite.md. */
#ifndef SCOOBY_SPRITE_H
#define SCOOBY_SPRITE_H
#include "walk.h"

/* ---- functions (each is also an HLE_FUNC replacing the 68k routine at its address) */
void actors_init(hle_ctx *ctx);        /* $00801A  reset all actor slots, Shaggy/Scooby sets */
void sprite_build(hle_ctx *ctx);       /* $00A984  build the SAT in SAT_BUF (pointer, UI, actors) */
void actor_dma(hle_ctx *ctx);          /* $00AF24  VBlank: one actor tile DMA, SAT DMA, timers */
void actor_animate(hle_ctx *ctx);      /* $00B0B8  step the animation scripts, decode frames */
void frame_pack_48x80(hle_ctx *ctx);   /* $00D3F0  chunky -> tiles, unscaled, 48x80 */
void frame_pack_80x48(hle_ctx *ctx);   /* $00D42C  chunky -> tiles, unscaled, 80x48 */
void frame_scale_48x80(hle_ctx *ctx);  /* $00D468  chunky -> tiles, depth-scaled, 48x80 */
void frame_scale_80x48(hle_ctx *ctx);  /* $00DA6A  chunky -> tiles, depth-scaled, 80x48 */

/* ---- per-slot arrays (slot 0 Shaggy, 1 Scooby, 2..5 room actors) */
#define ACTOR_ANIMSET(i)     (0xFF0470 + 4 * (i))  /* l: animation set (ROM) */
/* ACTOR_ANIM(i) $FF0488+2i is in walk.h: animation number */
#define ACTOR_SCRIPT_POS(i)  (0xFF0494 + 2 * (i))  /* w: offset (in the set) of the next script command */
#define ACTOR_FRAME(i)       (0xFF04A0 + 2 * (i))  /* w: offset (in the set) of the frame being decoded */
#define ACTOR_HOT_X(i)       (0xFF04AC + 2 * (i))  /* w: depth-scaled hot spot x of that frame */
#define ACTOR_HOT_Y(i)       (0xFF04B8 + 2 * (i))  /* w: depth-scaled hot spot y */
#define ACTOR_SHOWN_HOT_X(i) (0xFF04C4 + 2 * (i))  /* w: hot spot of the frame on screen (latched) */
#define ACTOR_SHOWN_HOT_Y(i) (0xFF04D0 + 2 * (i))
#define ACTOR_DELAY(i)       (0xFF050C + 2 * (i))  /* w: frames per animation frame (script op 4) */
#define ACTOR_SRC(i)         (0xFF0540 + 4 * (i))  /* l: RLE stream to decode / then the DMA source */
#define ACTOR_SHOWN_FRAME(i) (0xFF0558 + 4 * (i))  /* l: frame header on screen (byte 0 < 0: none) */
#define ACTOR_NEXT_FRAME(i)  (0xFF0570 + 4 * (i))  /* l: frame header decoded, waiting for its DMA */
#define ACTOR_DMA_WORDS(i)   (0xFF0588 + 2 * (i))  /* w: tile DMA length of that frame */
#define ACTOR_SHOWN_DEPTH_X(i) (0xFF05C4 + 2 * (i)) /* w: depth of the frame on screen (latched) */
#define ACTOR_SHOWN_DEPTH_Y(i) (0xFF05D0 + 2 * (i))
#define ACTOR_TIMER(i)       (0xFF0618 + 2 * (i))  /* w: frames until the script continues (<=0: now) */
#define SCALE_DEPTH_X        0xFF0624              /* w: depth passed to the frame scaler */
#define SCALE_DEPTH_Y        0xFF0626

/* bit sets, bit i = slot i */
#define ACTORS_TILES_READY   0xFF0AB6  /* tiles decoded into the slot buffer, DMA pending */
#define ACTORS_NEED_SCALE    0xFF0AB9  /* RLE stream known, depth scaling still to do */
#define ACTORS_DRAWN         0xFF0AC4  /* drawn into the SAT this frame */
#define ACTORS_DRAWN_PREV    0xFF0AC5  /* ... last frame */
#define ACTORS_DMA_DONE      0xFF0AC1  /* frame latched for display after its DMA (cleared by the DMA) */
#define ACTORS_FOLLOW_SYNC   0xFF0AC6  /* follow mode: frame shown, wait for the partner */
#define ACTORS_DRAW_TODO     0xFF0ABA  /* scratch of sprite_build: slots not drawn yet */
#define ACTORS_FROZEN        0xFF0ABF  /* frozen by a script: not drawn */
#define FOLLOW_SLOT          0xFF08B0  /* w: slot of the actor attached to Shaggy (follow mode) */

#define SAT_BUF              0xFF0008  /* 80 x 8 bytes, DMA'd to VRAM $D800 */
#define SLOT_VRAM_TBL        0x0FC860  /* w[6]: VRAM address of each slot's tiles */
#define SLOT_BUFFER_TBL      0x0FC9F4  /* l[6]: RAM staging buffer of each slot */
#define FOLLOW_ANIM_TBL      0x0FC760  /* w[]: partner animation for each Shaggy animation */
#define CHUNKY_BUF           0xFF6000  /* chunky (1 byte per pixel) frame, input of the scalers */

#endif
