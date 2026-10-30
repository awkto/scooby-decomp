/* 68000 condition-code helpers for decompiled code that must reproduce the exact flags
 * (difftest compares X N Z V C). Used by mainloop.c / reset.c (#12). */
#ifndef SCOOBY_CC68K_H
#define SCOOBY_CC68K_H
#include "hle.h"

/* SUB/SUBQ/SUBI/CMP (cmp: X unchanged) of `s` from `d` at `bits` width; returns d - s */
static inline uint32_t cc_sub(hle_ctx *c, uint32_t d, uint32_t s, int bits, bool set_x)
{
	uint32_t m = bits == 8 ? 0xFFu : bits == 16 ? 0xFFFFu : 0xFFFFFFFFu, sign = (m >> 1) + 1;
	d &= m;
	s &= m;
	uint32_t r = (d - s) & m;
	unsigned f = 0;
	if (r & sign) f |= CC_N;
	if (!r) f |= CC_Z;
	if ((d ^ s) & (d ^ r) & sign) f |= CC_V;
	if (s > d) f |= CC_C | (set_x ? CC_X : 0);
	cc_set(c, CC_N | CC_Z | CC_V | CC_C | (set_x ? CC_X : 0), f);
	return r;
}

/* ADD/ADDQ/ADDI at `bits` width; returns d + s */
static inline uint32_t cc_add(hle_ctx *c, uint32_t d, uint32_t s, int bits)
{
	uint32_t m = bits == 8 ? 0xFFu : bits == 16 ? 0xFFFFu : 0xFFFFFFFFu, sign = (m >> 1) + 1;
	d &= m;
	s &= m;
	uint32_t r = (d + s) & m;
	unsigned f = 0;
	if (r & sign) f |= CC_N;
	if (!r) f |= CC_Z;
	if (~(d ^ s) & (d ^ r) & sign) f |= CC_V;
	if (r < d) f |= CC_C | CC_X;
	cc_set(c, CC_N | CC_Z | CC_V | CC_C | CC_X, f);
	return r;
}

/* BTST/BSET/BCLR: Z = !(old bit) */
static inline void cc_btst(hle_ctx *c, uint32_t v, int bit) { cc_set(c, CC_Z, (v >> bit) & 1 ? 0 : CC_Z); }

/* 68k-style word compare "cmp.w src,dst" + signed branch helpers */
static inline int16_t cmpw(hle_ctx *c, uint32_t dst, uint32_t src)
{
	cc_sub(c, dst, src, 16, false);
	return (int16_t)((int16_t)dst < (int16_t)src ? -1 : (int16_t)dst > (int16_t)src);
}

/* LSL.W/LSR.W #n (1..8): X=C = last bit shifted out, V=0 */
static inline uint32_t lslw(hle_ctx *c, uint32_t v, int n)
{
	v &= 0xFFFF;
	uint32_t r = (v << n) & 0xFFFF;
	unsigned f = ((v >> (16 - n)) & 1) ? CC_C | CC_X : 0;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, f | (r & 0x8000 ? CC_N : 0) | (r ? 0 : CC_Z));
	return r;
}

static inline uint32_t lsrw(hle_ctx *c, uint32_t v, int n)
{
	v &= 0xFFFF;
	uint32_t r = v >> n;
	unsigned f = ((v >> (n - 1)) & 1) ? CC_C | CC_X : 0;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, f | (r ? 0 : CC_Z));
	return r;
}

/* NEG.W / NEG.L: X=C = (result != 0), V = (operand == most negative) */
static inline uint32_t neg16(hle_ctx *c, uint32_t v)
{
	v &= 0xFFFF;
	uint32_t r = (0u - v) & 0xFFFF;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, (r ? CC_X | CC_C : CC_Z) | (r & 0x8000 ? CC_N : 0) |
	       (v == 0x8000 ? CC_V : 0));
	return r;
}
static inline uint32_t neg32(hle_ctx *c, uint32_t v)
{
	uint32_t r = 0u - v;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, (r ? CC_X | CC_C : CC_Z) | (r & 0x80000000u ? CC_N : 0) |
	       (v == 0x80000000u ? CC_V : 0));
	return r;
}

/* LSL.L #n (1..8): X=C = last bit shifted out, V=0 */
static inline uint32_t lsll(hle_ctx *c, uint32_t v, int n)
{
	uint32_t r = v << n;
	unsigned f = ((v >> (32 - n)) & 1) ? CC_C | CC_X : 0;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, f | (r & 0x80000000u ? CC_N : 0) | (r ? 0 : CC_Z));
	return r;
}

/* DIVU.W src,Dn on *d (GPGX semantics): quotient in the low word, remainder in the high
 * word; on overflow (quotient > $FFFF) Dn is unchanged and V=N=1. X unchanged, C=0.
 * A zero divisor traps on the 68000; callers must not divide by zero. */
static inline void divu16(hle_ctx *c, uint32_t *d, uint32_t src)
{
	src &= 0xFFFF;
	if (!src)
		return;
	uint32_t q = *d / src, r = *d % src;
	if (q > 0xFFFF) {
		cc_set(c, CC_V | CC_N | CC_C, CC_V | CC_N);
		return;
	}
	*d = (r << 16) | q;
	cc_set(c, CC_N | CC_Z | CC_V | CC_C, (q & 0x8000 ? CC_N : 0) | (q ? 0 : CC_Z));
}

/* MULS.W: 32-bit signed product of the low words; N Z, V=C=0, X unchanged */
static inline uint32_t muls16(hle_ctx *c, uint32_t a, uint32_t b)
{
	uint32_t r = (uint32_t)((int32_t)(int16_t)a * (int32_t)(int16_t)b);
	cc_set(c, CC_N | CC_Z | CC_V | CC_C, (r & 0x80000000u ? CC_N : 0) | (r ? 0 : CC_Z));
	return r;
}

#endif
