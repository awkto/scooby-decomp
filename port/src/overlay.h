/* Tiny 3x5 bitmap font for the debug overlay (drawn into an RGB565 buffer). */
#ifndef SCOOBY_OVERLAY_H
#define SCOOBY_OVERLAY_H
#include <stddef.h>
#include <stdint.h>

/* draw text at (x,y) with a dark backdrop; '\n' starts a new line. Returns width in px. */
int overlay_text(uint16_t *px, int w, int h, int x, int y, uint16_t color, const char *s);

#endif
