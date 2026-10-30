#include "overlay.h"
#include <ctype.h>

/* 5 rows of 3 bits, MSB = left column */
#define G(a, b, c, d, e) (uint16_t)((0##a << 12) | (0##b << 9) | (0##c << 6) | (0##d << 3) | 0##e)
/* rows written as octal digits of the 3-bit patterns: 7=111 5=101 2=010 4=100 1=001 6=110 3=011 */
static const struct { char c; uint16_t g; } font[] = {
	{ '0', G(7, 5, 5, 5, 7) }, { '1', G(2, 6, 2, 2, 7) }, { '2', G(7, 1, 7, 4, 7) },
	{ '3', G(7, 1, 7, 1, 7) }, { '4', G(5, 5, 7, 1, 1) }, { '5', G(7, 4, 7, 1, 7) },
	{ '6', G(7, 4, 7, 5, 7) }, { '7', G(7, 1, 1, 2, 2) }, { '8', G(7, 5, 7, 5, 7) },
	{ '9', G(7, 5, 7, 1, 7) }, { 'A', G(2, 5, 7, 5, 5) }, { 'B', G(6, 5, 6, 5, 6) },
	{ 'C', G(3, 4, 4, 4, 3) }, { 'D', G(6, 5, 5, 5, 6) }, { 'E', G(7, 4, 6, 4, 7) },
	{ 'F', G(7, 4, 6, 4, 4) }, { 'G', G(3, 4, 5, 5, 3) }, { 'H', G(5, 5, 7, 5, 5) },
	{ 'I', G(7, 2, 2, 2, 7) }, { 'J', G(1, 1, 1, 5, 2) }, { 'K', G(5, 5, 6, 5, 5) },
	{ 'L', G(4, 4, 4, 4, 7) }, { 'M', G(5, 7, 7, 5, 5) }, { 'N', G(6, 5, 5, 5, 5) },
	{ 'O', G(2, 5, 5, 5, 2) }, { 'P', G(6, 5, 6, 4, 4) }, { 'Q', G(2, 5, 5, 6, 3) },
	{ 'R', G(6, 5, 6, 5, 5) }, { 'S', G(3, 4, 2, 1, 6) }, { 'T', G(7, 2, 2, 2, 2) },
	{ 'U', G(5, 5, 5, 5, 7) }, { 'V', G(5, 5, 5, 5, 2) }, { 'W', G(5, 5, 7, 7, 5) },
	{ 'X', G(5, 5, 2, 5, 5) }, { 'Y', G(5, 5, 2, 2, 2) }, { 'Z', G(7, 1, 2, 4, 7) },
	{ '.', G(0, 0, 0, 0, 2) }, { ':', G(0, 2, 0, 2, 0) }, { '-', G(0, 0, 7, 0, 0) },
	{ '/', G(1, 1, 2, 4, 4) }, { '%', G(5, 1, 2, 4, 5) }, { '=', G(0, 7, 0, 7, 0) },
	{ '+', G(0, 2, 7, 2, 0) }, { '$', G(3, 6, 2, 3, 6) }, { '(', G(1, 2, 2, 2, 1) },
	{ ')', G(4, 2, 2, 2, 4) }, { ',', G(0, 0, 0, 2, 4) }, { '#', G(5, 7, 5, 7, 5) },
};

static uint16_t glyph(char c)
{
	c = (char)toupper((unsigned char)c);
	for (size_t i = 0; i < sizeof font / sizeof font[0]; i++)
		if (font[i].c == c)
			return font[i].g;
	return 0;
}

int overlay_text(uint16_t *px, int w, int h, int x0, int y0, uint16_t color, const char *s)
{
	int x = x0, y = y0, maxx = x0;
	for (; *s; s++) {
		if (*s == '\n') {
			x = x0;
			y += 7;
			continue;
		}
		uint16_t g = glyph(*s);
		for (int r = -1; r < 6; r++)
			for (int col = 0; col < 4; col++) {
				int X = x + col, Y = y + r;
				if (X < 0 || Y < 0 || X >= w || Y >= h)
					continue;
				int on = r >= 0 && r < 5 && col >= 0 && col < 3 && ((g >> ((4 - r) * 3 + (2 - col))) & 1);
				if (on)
					px[Y * w + X] = color;
				else
					px[Y * w + X] = (uint16_t)((px[Y * w + X] >> 2) & 0x39E7);  /* darken backdrop */
			}
		x += 4;
		if (x > maxx)
			maxx = x;
	}
	return maxx - x0;
}
