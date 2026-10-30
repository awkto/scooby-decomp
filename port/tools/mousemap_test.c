/* Unit test of the window -> game pixel mapping used for the mouse (letterboxed integer
 * scaling, as SDL_RenderSetLogicalSize + SDL_RenderSetIntegerScale draw it). */
#include "mouse.h"
#include <stdio.h>

static int fails;
static void check(int wx, int wy, int ow, int oh, int tw, int th, int ok, int ex, int ey)
{
	int gx = -1, gy = -1;
	int r = mouse_map_window(wx, wy, ow, oh, tw, th, &gx, &gy);
	if (r != ok || (ok && (gx != ex || gy != ey))) {
		printf("FAIL map(%d,%d in %dx%d, image %dx%d) = %d (%d,%d), want %d (%d,%d)\n",
		       wx, wy, ow, oh, tw, th, r, gx, gy, ok, ex, ey);
		fails++;
	}
}

int main(void)
{
	/* default window: --scale 3 -> 960x672, image 256x224 drawn 3x = 768x672 at x 96 */
	check(96, 0, 960, 672, 256, 224, 1, 0, 0);
	check(95, 0, 960, 672, 256, 224, 0, 0, 0);          /* left letterbox */
	check(96 + 768 - 1, 671, 960, 672, 256, 224, 1, 255, 223);
	check(96 + 768, 300, 960, 672, 256, 224, 0, 0, 0);  /* right letterbox */
	check(96 + 3 * 100 + 2, 3 * 50 + 1, 960, 672, 256, 224, 1, 100, 50);
	/* 1920x1080 fullscreen: scale 4 (1024x896), offsets (448, 92) */
	check(448, 92, 1920, 1080, 256, 224, 1, 0, 0);
	check(448 + 4 * 128 + 3, 92 + 4 * 112 + 3, 1920, 1080, 256, 224, 1, 128, 112);
	check(447, 500, 1920, 1080, 256, 224, 0, 0, 0);
	check(1000, 91, 1920, 1080, 256, 224, 0, 0, 0);     /* top letterbox */
	/* H40 mode image (320 wide) in the default window: exact 3x fit, no letterbox */
	check(959, 671, 960, 672, 320, 224, 1, 319, 223);
	printf("%s mousemap_test (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
