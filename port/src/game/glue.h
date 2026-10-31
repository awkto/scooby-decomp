/* Glue between C blocks (GitLab #24, docs/native.md "Glue").
 *
 * The C ranges of the earlier waves stop in front of a jsr/bsr to code that can wait for VBlank
 * and resume at the instruction after it; the call itself, the branch back to a loop head, the
 * rts and the pushes around such calls were left to the 68k. In the native build every one of
 * them is a C block too. They are written as what they are:
 *
 *   GLUE_CALL(0x000D54, 6, 0x00974C)    jsr wait_vblank at $D54 (6 bytes): push $D5A, go to $974C
 *   GLUE_GOTO(0x000D68, 0x000D38)       bra $D38
 *   GLUE_RTS(0x002404)                  rts
 *
 * They are HLE_RANGEs (difftest runs the original instruction up to the declared exit, the
 * callee entry with the return address pushed) and change no register or flag, like the
 * instructions they stand for. The callee and the code at the return address are C blocks. */
#ifndef SCOOBY_GLUE_H
#define SCOOBY_GLUE_H
#include "hle.h"

#define GLUE_PASTE(a, b) a##b
#define GLUE_NAME(kind, ADDR) GLUE_PASTE(kind, ADDR)
/* one more expansion level, so that the pasted name reaches HLE_RANGE/HLE_FUNC (which paste it) */
#define GLUE_HLE_RANGE(...) HLE_RANGE(__VA_ARGS__)
#define GLUE_HLE_FUNC(...) HLE_FUNC(__VA_ARGS__)

/* jsr/bsr TARGET at ADDR, LEN bytes long: the call returns to ADDR + LEN */
#define GLUE_CALL(ADDR, LEN, TARGET)                                                    \
	GLUE_HLE_RANGE(ADDR, GLUE_NAME(glue_call_, ADDR), 18, HLE_EXIT_SP(TARGET, -4))    \
	{                                                                                   \
		push32(ctx, (ADDR) + (LEN));                                                    \
		hle_jump(ctx, TARGET);                                                          \
	}

/* bra / jmp TARGET at ADDR */
#define GLUE_GOTO(ADDR, TARGET)                                                         \
	GLUE_HLE_RANGE(ADDR, GLUE_NAME(glue_goto_, ADDR), 10, TARGET)                     \
	{                                                                                   \
		hle_jump(ctx, TARGET);                                                          \
	}

/* rts at ADDR (the layer pops the return address of an HLE_FUNC that does not jump) */
#define GLUE_RTS(ADDR)                                                                  \
	GLUE_HLE_FUNC(ADDR, GLUE_NAME(glue_rts_, ADDR), 16) {}

/* jsr/bsr from C to a routine that may wait (it is entered at top level, never nested):
 * push the return address RET and continue at TARGET. Use as the last statement of a block. */
static inline void glue_call(hle_ctx *ctx, uint32_t target, uint32_t ret)
{
	push32(ctx, ret);
	hle_jump(ctx, target);
}

/* movem.l REGS,-(sp) / movem.l (sp)+,REGS with REGS as the 68k register mask (bit 0 = D0 ..
 * bit 15 = A7, the order of the pop): the same bytes on the stack as the 68000 leaves there */
static inline void movem_push_l(hle_ctx *ctx, unsigned mask)
{
	for (int r = 15; r >= 0; r--)
		if (mask & (1u << r))
			push32(ctx, r < 8 ? ctx->d[r] : ctx->a[r - 8]);
}

static inline void movem_pop_l(hle_ctx *ctx, unsigned mask)
{
	for (int r = 0; r < 16; r++)
		if (mask & (1u << r)) {
			uint32_t v = pop32(ctx);
			if (r < 8)
				ctx->d[r] = v;
			else
				ctx->a[r - 8] = v;
		}
}

/* movem.l (sp),REGS: reload without popping */
static inline void movem_peek_l(hle_ctx *ctx, unsigned mask)
{
	uint32_t sp = ctx->a[7];
	for (int r = 0; r < 16; r++)
		if (mask & (1u << r)) {
			uint32_t v = bus_r32(sp);
			sp += 4;
			if (r < 8)
				ctx->d[r] = v;
			else
				ctx->a[r - 8] = v;
		}
}

/* movem.w: words; the pop sign-extends into the whole register (data registers too) */
static inline void movem_push_w(hle_ctx *ctx, unsigned mask)
{
	for (int r = 15; r >= 0; r--)
		if (mask & (1u << r))
			push16(ctx, (uint16_t)(r < 8 ? ctx->d[r] : ctx->a[r - 8]));
}

static inline void movem_pop_w(hle_ctx *ctx, unsigned mask)
{
	for (int r = 0; r < 16; r++)
		if (mask & (1u << r)) {
			uint32_t v = (uint32_t)(int32_t)(int16_t)pop16(ctx);
			if (r < 8)
				ctx->d[r] = v;
			else
				ctx->a[r - 8] = v;
		}
}

#define MOVEM_D(n) (1u << (n))
#define MOVEM_A(n) (1u << (8 + (n)))
#define MOVEM_ALL  0x7FFFu   /* d0-d7/a0-a6 */

#endif
