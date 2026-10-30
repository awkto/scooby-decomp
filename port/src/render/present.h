/* Output stage (GitLab #25 step 3): the game frame (RGBA, 256/320 x 224) -> the window.
 * SDL-free so it can be tested headless; main.c uploads the result and lets SDL place it.
 *   nearest  whole-number scale, letterboxed (the default, sharp pixels)
 *   smooth   "sharp bilinear": whole-number prescale here, then SDL scales the rest of the way
 *            to fill the window with linear filtering (no shimmer, only the outer pixel
 *            edges are blended)
 *   crt      whole-number scale with a scanline beam profile, a slight horizontal blend
 *            between neighbouring pixels and an RGB aperture mask
 * The game logic never sees any of this. */
#ifndef SCOOBY_PRESENT_H
#define SCOOBY_PRESENT_H
#include <stdbool.h>
#include <stdint.h>

typedef enum { PRESENT_NEAREST, PRESENT_SMOOTH, PRESENT_CRT } present_filter;

bool present_filter_parse(const char *name, present_filter *f);
const char *present_filter_name(present_filter f);

/* the whole-number scale that fits a w x h frame into an out_w x out_h window (>= 1) */
int present_fit_scale(int w, int h, int out_w, int out_h);

/* where the frame goes in the window: x, y, width, height (smooth: fills the window, keeping
 * the aspect ratio; the others: the whole-number scale, centred) */
void present_rect(present_filter f, int w, int h, int out_w, int out_h, int r[4]);

/* scale src (w x h, `pitch` pixels per row) by k into dst (w*k x h*k, packed) */
void present_scale(const uint32_t *src, int w, int h, int pitch, int k, present_filter f, uint32_t *dst);

#endif
