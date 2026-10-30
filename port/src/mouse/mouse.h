/* Mouse control for the native host (GitLab #23), host side.
 *
 * Turns mouse events into the game's own input: it opens the game's pointer mode and
 * keeps its crosshair on the mouse, selects verbs / clicks objects by injecting the pad
 * presses the original flow expects, and walks Shaggy to clicked floor positions with
 * the pathfinder in port/src/game/walkpath.c.  Game knowledge lives in
 * port/src/game/mousehook.c; this module is SDL-free so the --script language can drive
 * it (main.c feeds it SDL events).  docs/mouse.md has the controls and the design.
 *
 * Coordinates are game screen pixels (0..255 x 0..223) unless stated otherwise. */
#ifndef SCOOBY_MOUSE_H
#define SCOOBY_MOUSE_H
#include <stdbool.h>
#include <stdint.h>

enum { MOUSE_LEFT = 1, MOUSE_RIGHT = 2, MOUSE_MIDDLE = 3 };

void mouse_set_enabled(bool on);              /* --no-mouse: everything below is a no-op */
bool mouse_enabled(void);

/* events */
void mouse_move(int gx, int gy);              /* inside the game image */
void mouse_leave(void);                       /* pointer left the game image */
void mouse_click(int button);                 /* a press (acted on at once; release is implicit) */
void mouse_wheel(int steps);                  /* >0 up, <0 down */

/* Once per emulated frame, before it runs: advances the click/walk state machine, writes
 * the game's pointer RAM and returns the pad mask for the frame (pad_in plus injected
 * buttons).  pad_in = keyboard/gamepad/script buttons; any of them hands control back to
 * the pad (the mouse goes passive until it is moved again). */
unsigned mouse_frame(unsigned pad_in);

/* host cursor, drawn into the RGB565 game frame (hidden where the game's own crosshair
 * already marks the mouse position) */
void mouse_draw(uint16_t *px, int w, int h);
void mouse_draw_rgba(uint32_t *px, int w, int h);   /* same, RGBA buffer */

/* window pixel -> game pixel for an integer-scaled, centred (letterboxed) image:
 * out_w x out_h = renderer output size in pixels, tex_w x tex_h = game image.
 * Returns false when (wx, wy) is outside the image. */
bool mouse_map_window(int wx, int wy, int out_w, int out_h, int tex_w, int tex_h, int *gx, int *gy);

/* status for scripts/overlay: short text ("idle", "walk", ...) */
const char *mouse_status(void);

#endif
