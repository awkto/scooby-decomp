/* Native renderer <-> emulator bridge (GitLab #25). See docs/subsystems/render.md.
 *
 * The emulated build still runs GPGX's VDP (it owns the VDP state, its timing and its status
 * bits). The bridge hooks GPGX's line renderer (tools/patches/gpgx-render.patch) and draws the
 * same lines with the native renderer, from the same live VDP state, into its own RGBA frame:
 *   --renderer gpgx    GPGX's frame is shown (default)
 *   --renderer native  the native frame is shown, and the "native" HLE group is active: the
 *                      verb-bar split is drawn from a band the C hblank_verb_bar records instead
 *                      of mid-frame VDP register writes (game/frameloop.c).
 * SCOOBY_RENDERER=native|gpgx sets the default for every program (scoobyport, frameprof...).
 * SCOOBY_RENDERCMP=N[,DIR]: compare the native frame with GPGX's (render_emu.c, rendertest.sh). */
#ifndef SCOOBY_RENDER_EMU_H
#define SCOOBY_RENDER_EMU_H
#include <stdbool.h>
#include <stdint.h>

#include "render.h"

typedef enum { RENDERER_GPGX = 0, RENDERER_NATIVE = 1 } renderer_kind;

/* select the renderer; called by the programs after emu_init and BEFORE hle_configure (the
 * native one activates the "native" HLE group). Returns false on an unknown name. */
bool render_emu_select(const char *name);
renderer_kind render_emu_kind(void);
/* reads SCOOBY_RENDERER / SCOOBY_RENDERCMP / SCOOBY_RENDER_SPLIT (emu_init calls it) */
void render_emu_init(void);

/* the verb-bar band for the frame being drawn (frame loop model, game/frameloop.c);
 * cleared at the end of every frame */
void render_emu_set_band(const render_band *b);

/* called by emu_run_frame after the core has produced a frame */
void render_emu_frame_end(long frame);

/* the native frame (RGBA, pitch RENDER_MAX_W); NULL if the native renderer is not running */
const uint32_t *render_emu_frame(int *w, int *h);

/* compare statistics (SCOOBY_RENDERCMP): frames compared / mismatching; printed at exit */
void render_emu_report(void);

#endif
