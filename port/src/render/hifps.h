/* High frame rate presentation (GitLab #27), presentation only: docs/subsystems/hifps.md.
 *
 * The game logic stays at 59.92 Hz and is not told anything. With high_fps = H, the host shows
 * n = round(H x T) pictures per logic frame (T = 1 / 59.92 s): the whole frame R(k) at k x T, as
 * at 60 Hz, and n - 1 in-between pictures evenly spaced before it, drawn from R(k)'s VDP state
 * with the moving parts put back toward where R(k-1) showed them:
 *   - the h-scroll / v-scroll of the room band's lines (the camera; per h-scroll table entry),
 *   - the actors' sprites, matched by owner (actor slot) + anchor (the hot spot's screen
 *     position), which sprite_build announces when it writes them;
 * big steps snap (a scroll step over 64 px, an anchor step over 48 px, a room change, a
 * discontinuity). Not moved: the name band, text boxes, the verb bar, UI sprites (pointer,
 * cursor, inventory highlight, masking sprites), palettes, tiles. A frame where nothing moves
 * (or that cannot be interpolated) keeps showing R(k-1) until R(k) is due, exactly as at 60 Hz.
 *
 * Nothing here writes to the emulated machine: the in-between pictures are rendered in frame
 * mode (render_frame's contract) from a host-side copy of the VDP state taken at line 0. */
#ifndef SCOOBY_HIFPS_H
#define SCOOBY_HIFPS_H
#include <stdbool.h>
#include <stdint.h>

#include "render.h"

/* ---- game side: sprite_build ($00A984, sprite.c) announces each actor's SAT entries */
/* the camera (world px of the room view's top left, $FF06CA / $FF06CE) the sprites are placed with */
void hifps_spr_begin(int cam_x, int cam_y);
/* slot 0..5 owns SAT entries [first, first + count); (wx, wy) = its world position, 16.16
 * (the hot spot: drawn at screen (wx >> 16) - cam_x, (wy >> 16) - cam_y + 16) */
void hifps_spr_actor(int slot, int first, int count, int32_t wx, int32_t wy);
void hifps_spr_end(int total);

/* ---- host */
void hifps_set_rate(int hz);          /* 0 = off; needs the native renderer */
int hifps_rate(void);
int hifps_steps(void);                /* n = round(rate x T): pictures per logic frame (1 = off) */
bool hifps_active(void);              /* a rate > 60 and the native renderer */
/* render every frame's in-betweens right away (scripted / batch runs: counters, dumps) */
void hifps_set_eager(bool on);
void hifps_reset(void);               /* discontinuity (state load): no interpolation into the next frame */

/* ---- renderer bridge (render_emu.c) */
void hifps_line0(const render_vdp *live);      /* line 0 of the display: snapshot */
/* the frame is done; img = the whole frame shown (pitch RENDER_MAX_W). Also writes the whole
 * frame's hash to SCOOBY_FRAME_HASH_LOG (with high_fps on or off) */
void hifps_frame_end(long frame, const render_band *band, const uint32_t *img, int w, int h);

/* ---- presentation: picture j of n (1..n-1) between R(k-1) and the newest frame R(k), pitch
 * RENDER_MAX_W. NULL: nothing to show (no frames yet). `moved` (may be NULL): true if the
 * picture was interpolated, false if it is R(k-1) held. */
const uint32_t *hifps_frame(int j, int n, int *w, int *h, bool *moved);

void hifps_report(void);              /* counters (stderr) */

#endif
