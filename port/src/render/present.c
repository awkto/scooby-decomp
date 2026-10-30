/* Output stage filters (GitLab #25 step 3), see present.h. Pixels are RGBA (bytes R G B A). */
#include "present.h"

#include <string.h>

bool present_filter_parse(const char *name, present_filter *f)
{
	if (!strcmp(name, "nearest") || !strcmp(name, "sharp"))
		*f = PRESENT_NEAREST;
	else if (!strcmp(name, "smooth") || !strcmp(name, "linear"))
		*f = PRESENT_SMOOTH;
	else if (!strcmp(name, "crt"))
		*f = PRESENT_CRT;
	else
		return false;
	return true;
}

const char *present_filter_name(present_filter f)
{
	return f == PRESENT_SMOOTH ? "smooth" : f == PRESENT_CRT ? "crt" : "nearest";
}

int present_fit_scale(int w, int h, int out_w, int out_h)
{
	if (w <= 0 || h <= 0)
		return 1;
	int s = out_w / w < out_h / h ? out_w / w : out_h / h;
	return s < 1 ? 1 : s;
}

void present_rect(present_filter f, int w, int h, int out_w, int out_h, int r[4])
{
	if (f == PRESENT_SMOOTH && w > 0 && h > 0) {
		/* fill the window, keep the aspect ratio */
		if ((long)out_w * h <= (long)out_h * w) {
			r[2] = out_w;
			r[3] = (int)((long)out_w * h / w);
		} else {
			r[3] = out_h;
			r[2] = (int)((long)out_h * w / h);
		}
	} else {
		int s = present_fit_scale(w, h, out_w, out_h);
		r[2] = w * s;
		r[3] = h * s;
	}
	r[0] = (out_w - r[2]) / 2;
	r[1] = (out_h - r[3]) / 2;
}

static inline uint32_t mix(uint32_t a, uint32_t b, unsigned wb /* 0..256 */)
{
	unsigned wa = 256 - wb;
	uint32_t r = ((a & 0xFF) * wa + (b & 0xFF) * wb) >> 8;
	uint32_t g = (((a >> 8) & 0xFF) * wa + ((b >> 8) & 0xFF) * wb) >> 8;
	uint32_t bl = (((a >> 16) & 0xFF) * wa + ((b >> 16) & 0xFF) * wb) >> 8;
	return r | g << 8 | bl << 16 | 0xFF000000u;
}

static inline uint32_t scale_rgb(uint32_t c, unsigned mr, unsigned mg, unsigned mb /* x/256 */)
{
	unsigned r = ((c & 0xFF) * mr) >> 8, g = (((c >> 8) & 0xFF) * mg) >> 8, b = (((c >> 16) & 0xFF) * mb) >> 8;
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	return r | g << 8 | b << 16 | 0xFF000000u;
}

void present_scale(const uint32_t *src, int w, int h, int pitch, int k, present_filter f, uint32_t *dst)
{
	int ow = w * k;
	if (f != PRESENT_CRT || k < 2) {
		for (int y = 0; y < h; y++) {
			uint32_t *d = dst + (size_t)y * k * ow;
			const uint32_t *s = src + (size_t)y * pitch;
			for (int x = 0; x < w; x++)
				for (int i = 0; i < k; i++)
					d[x * k + i] = s[x];
			for (int i = 1; i < k; i++)
				memcpy(d + (size_t)i * ow, d, (size_t)ow * 4);
		}
		return;
	}
	/* CRT: beam profile per sub-row (bright centre, dark gap), horizontal blend towards the
	 * neighbour on the side of the sub-pixel, aperture mask over output columns */
	unsigned beam[16];
	for (int r = 0; r < k && r < 16; r++) {
		int t = (2 * r + 1) * 256 / (2 * k) - 128;          /* -128..128 across the line */
		int b = 300 - (t * t * 210) / (128 * 128);          /* 1.17 at the centre, 0.35 at the edge */
		beam[r] = (unsigned)(b < 0 ? 0 : b);
	}
	static const unsigned mask[3][3] = { { 256, 222, 222 }, { 222, 256, 222 }, { 222, 222, 256 } };
	for (int y = 0; y < h; y++) {
		const uint32_t *s = src + (size_t)y * pitch;
		for (int r = 0; r < k; r++) {
			uint32_t *d = dst + ((size_t)y * k + r) * ow;
			unsigned bm = beam[r < 16 ? r : 15];
			for (int X = 0; X < ow; X++) {
				int x = X / k, sub = X % k;
				/* distance of the sub-pixel from the source pixel centre, -0.5..0.5 */
				int off = (2 * sub + 1) * 128 / k - 128;
				uint32_t c = s[x];
				if (off < 0 && x > 0)
					c = mix(c, s[x - 1], (unsigned)(-off) / 2);
				else if (off > 0 && x < w - 1)
					c = mix(c, s[x + 1], (unsigned)off / 2);
				const unsigned *m = mask[X % 3];
				d[X] = scale_rgb(c, m[0] * bm >> 8, m[1] * bm >> 8, m[2] * bm >> 8);
			}
		}
	}
}
