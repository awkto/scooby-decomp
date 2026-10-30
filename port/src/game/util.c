/* Small memory helpers. */
#include "game.h"

/* $005B56: copy the NUL-terminated string at A0 to A1 (both advanced past the NUL).
 *   move.b (a0)+,(a1)+ / bne.w $5B56 / rts
 * Final CCR: N=V=C=0, Z=1 (from moving the NUL), X unchanged. */
HLE_FUNC(0x005B56, str_copy, 0)
{
	uint8_t b;
	int n = 0;
	do {
		b = bus_r8(ctx->a[0]++);
		bus_w8(ctx->a[1]++, b);
		n++;
	} while (b);
	cc_logic8(ctx, b);
	ctx->cycles = 30 + 22 * n;
}
