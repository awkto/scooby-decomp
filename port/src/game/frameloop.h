/* Frame-loop model for the native renderer (GitLab #25, groundwork for #24).
 * docs/subsystems/render.md "Frame pacing" and docs/porting.md "HV counter reads". */
#ifndef SCOOBY_FRAMELOOP_H
#define SCOOBY_FRAMELOOP_H
#include <stdbool.h>
#include <stdint.h>

#include "render/render.h"

/* The HV counter as the VBlank-side code sees it in the frame-loop model: the VBlank hook
 * runs at the start of the vertical blank, so every "is there still time for a DMA in this
 * blank" test passes (line $E1, H 0). With frameloop_set_hv_live(true) (difftest, whose sandbox
 * freezes the V counter on both sides) it is the real counter. */
#define FRAMELOOP_VBLANK_HV 0xE100u
uint16_t frameloop_hv(void);
void frameloop_set_hv_live(bool on);

/* the verb-bar band the HBlank split shows, from the game's RAM (hblank_verb_bar's values) */
void verb_bar_band(render_band *b);

/* SCOOBY_RENDER_SPLIT=model: record the band whenever the 68k hblank_verb_bar starts, so the
 * native renderer draws the bottom band from the model while GPGX shows the real split */
void frameloop_split_model_check(bool on);

#endif
