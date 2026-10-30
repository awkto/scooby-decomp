/* Widescreen (GitLab #26), presentation only: docs/subsystems/widescreen.md.
 *
 * With the native renderer, a room is shown through a wider view (398 x 224 for 16:9): the room
 * band (lines 16-167) shows more of the room around the camera, clamped to the room's edges, with
 * black bars where the room ends; the name band, text boxes and the verb bar stay 4:3, centred.
 * Screens that are not a plain room view stay 4:3, centred in the same wide frame (the per-screen
 * rules are the table `ws_rules` in widescreen.c).
 *
 * Nothing here writes to the emulated machine: the extra columns come from the game's own RAM
 * copies of the room's name tables (the planes in VRAM only hold the 4:3 window), the camera from
 * the frame's scroll values. The game's state is the same as in 4:3 at every frame (wstest). */
#ifndef SCOOBY_WIDESCREEN_H
#define SCOOBY_WIDESCREEN_H
#include <stdbool.h>

#include "render.h"

/* "16:9", "21:9", "W:H", a width in pixels ("400"), "auto" (the window's aspect), "4:3" / "off".
 * Returns false on a bad spec. */
bool ws_configure(const char *spec);
bool ws_enabled(void);
void ws_set_enabled(bool on);         /* F10; keeps the configured width */
void ws_toggle(void);
int ws_width(void);                   /* output width of the wide frame (0 when off) */
/* "auto": the window's output size; the wide frame takes its aspect (256..640 px) */
void ws_set_window_aspect(int out_w, int out_h);
void ws_set_stats(bool on);           /* print counters at exit (also SCOOBY_WS_STATS=1) */

/* ---- renderer side (render_emu.c) */
/* line 0 of a frame: decide wide or 4:3 for this frame and build the view */
void ws_frame_begin(const render_vdp *v);
/* is `line` drawn through the wide view? (a room line of a wide frame) */
bool ws_line_wide(const render_vdp *v, const render_band *band, int line);
render_view *ws_view(void);
/* the bars of the room band: output pixels [0, l) and [r, width) are outside the room */
void ws_bars(int *l, int *r);
/* a wide room line was drawn: does its 4:3 part differ from the 4:3 line? (counter) */
void ws_count_centre(bool differs);
/* the frame is done (counters; the layout becomes the one the mouse maps through) */
void ws_frame_end(void);

/* ---- mouse (main.c / mouse.c): frame pixel <-> game (4:3 screen) pixel of the last frame */
void ws_frame_to_game(int fx, int fy, int *gx, int *gy);
void ws_game_to_frame(int gx, int gy, int *fx, int *fy);

/* counters (printed at exit with stats on) */
void ws_report(void);

#endif
