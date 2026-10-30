/* Decompressors (issue #14): LZSS $0097AA and the PackBits-style RLE $009C64.
 * Formats: docs/formats.md. Subsystem notes: docs/subsystems/decomp.md.
 *
 * Both are leaf routines that save and restore their registers; only memory, the flags and
 * (RLE) two VDP register writes are visible. They are the hottest code of the game (the main
 * loop decompresses actor frames), so the 68k cycle cost is modelled per token, not charged
 * as a flat average: the layer then spreads it as the "Long-running functions" policy in
 * docs/porting.md describes. Callers: room.c and reset.c call lzss_run() directly; everything
 * else (title screen, sprite uploads, menus) reaches the HLE entry points through the 68k. */
#include "game.h"

/* read a byte of a stream: ROM or work RAM */
static uint8_t stream_u8(uint32_t a)
{
	return a >= 0xE00000 ? RAM8(a) : ROM8(a);
}

/* ------------------------------------------------------------------ LZSS */

#define RLE_FLAGS  0xFF09ED   /* ram_09ed: bit 3 = run VDP in shadow/highlight mode while decoding */
#define RING_SIZE  4096
#define RING_MASK  (RING_SIZE - 1)

/* MSB-first bit reader over a big-endian stream (the 68k keeps a 32-bit window and refills
 * 16 bits at a time; logically it is one continuous bitstream). Also tracks the 68k cycles
 * of the refills, which depend on how many bits are left in the window (the `bits` counter
 * mirrors the original's D1). */
typedef struct {
	uint32_t src;     /* next byte of the stream */
	uint32_t acc;     /* bit accumulator, next bit at bit 31 */
	int      have;    /* bits valid in acc */
	int      d1;      /* the original's refill counter */
	long     cycles;  /* cycles of the refill code */
} bitreader;

static uint32_t take_bits(bitreader *b, int n)
{
	uint32_t v = 0;
	for (int i = 0; i < n; i++) {
		if (!b->have) {
			b->acc = 0;
			for (int k = 0; k < 4; k++)
				b->acc = (b->acc << 8) | stream_u8(b->src++);
			b->have = 32;
		}
		v = (v << 1) | (b->acc >> 31);
		b->acc <<= 1;
		b->have--;
	}
	return v;
}

/* cycle cost of "move.w d1,d2 / subi.w #16,d2 / bge / [refill] / sub n from d1" for an
 * n-bit field: a 16-bit refill costs extra when fewer than 16 bits are buffered. */
static void field_cost(bitreader *b, int n, int shift_cycles_base)
{
	b->cycles += 4 + 8;                 /* move.w d1,d2 ; subi.w #$10,d2 */
	int d2 = b->d1 - 16;
	if (d2 >= 0) {
		b->cycles += 10;                /* bge taken */
	} else {
		d2 = -d2;
		b->d1 += 16;
		/* bge not taken 12, neg.w 4, addi.w 8, moveq 4, move.w (a0)+ 8, lsl.l d2,d3 8+2*d2,
		 * lsl.w #2 10, and.l (pc,d2) 20, or.l 8 */
		b->cycles += 12 + 4 + 8 + 4 + 8 + 8 + 2 * d2 + 10 + 20 + 8;
	}
	b->d1 -= n;
	b->cycles += shift_cycles_base;     /* the consuming shift/mask code, see lzss_core */
}

/* Decompress `src` into `out` through the ring buffer at `ring`; stores the output length
 * as a long at ring[0]. Returns the length; *cycles = 68k cycles of the original routine. */
static uint32_t lzss_core(uint32_t src, uint32_t ring, uint32_t out, long *cycles)
{
	bitreader b = { .src = src + 4, .d1 = 0x20 };
	/* entry: movem.l 128, bsr.w 18, sub_0098F8 (4+4+12+4+12+16) */
	b.cycles = 128 + 18 + 52;
	b.acc = 0;
	for (int k = 0; k < 4; k++)
		b.acc = (b.acc << 8) | stream_u8(src + k);
	b.have = 32;

	uint32_t r = 1;            /* ring write index (D7) */
	uint32_t o = out;
	for (;;) {
		/* flag bit: subq.w #1,d1 / bpl, refilling a whole long when the counter runs out */
		b.d1 = (int16_t)(b.d1 - 1);
		b.cycles += 4;
		if (b.d1 < 0) {
			b.d1 = 31;
			b.cycles += 12 + 8 + 12;    /* bpl not taken, move.w #$1f, move.l (a0)+,d0 */
		} else {
			b.cycles += 10;
		}
		b.cycles += 8;                  /* add.l d0,d0 */
		if (take_bits(&b, 1)) {
			/* literal: 8 bits */
			b.cycles += 8;              /* bcc not taken */
			field_cost(&b, 8, 4 + 24 + 4 + 8 + 14 + 4 + 8 + 10);
			uint8_t v = (uint8_t)take_bits(&b, 8);
			RAM8(o++) = v;
			RAM8(ring + r) = v;
			r = (r + 1) & RING_MASK;
			continue;
		}
		b.cycles += 10;                 /* bcc taken */
		field_cost(&b, 12, 8 + 4 + 32 + 4 + 8);   /* move.w #12,d2 sub rol move andi */
		uint32_t pos = take_bits(&b, 12);
		if (pos == 0) {
			b.cycles += 10;             /* beq taken */
			break;
		}
		b.cycles += 12;                 /* beq not taken */
		field_cost(&b, 4, 4 + 16 + 4 + 8 + 4 + 4);
		uint32_t len = take_bits(&b, 4) + 2;
		for (uint32_t i = 0; i < len; i++) {
			uint8_t v = RAM8(ring + ((pos + i) & RING_MASK));
			RAM8(o++) = v;
			RAM8(ring + r) = v;
			r = (r + 1) & RING_MASK;
		}
		/* per byte 72 (+10 ble taken, 8 on the last), then bra.w 10 */
		b.cycles += 72 * len + 10 * (len - 1) + 8 + 10;
	}
	uint32_t size = o - out;
	RAM32_W(ring, size);
	/* suba.l 6, move.l a2,(a1) 12, movem.l (sp)+ 132, rts 16 */
	b.cycles += 6 + 12 + 132 + 16;
	*cycles = b.cycles;
	return size;
}

/* $0097AA lzss_decompress: a0 = stream (the bit stream starts right at a0; room data has a
 * u32 packed length in front, which the caller skips), a1 = ring buffer, a2 = output. All registers preserved. Leaves (a1).l = output size.
 * Flags: the last flag-setting instruction is `move.l a2,(a1)` (N/Z of the size, V=C=0);
 * X is 0 (the last X-setting instruction is `sub.w d2,d1` after a `lsl.w`, never a borrow). */
HLE_FUNC(0x0097AA, lzss_decompress, 0)
{
	long cyc;
	uint32_t size = lzss_core(ctx->a[0], ctx->a[1], ctx->a[2], &cyc);
	cc_logic32(ctx, size);
	cc_set(ctx, CC_X, 0);
	/* The per-instruction model above is exact in instruction count (difftest: 60/60 states)
	 * but the emulated original runs a steady 2.2-3.1 % (avg 2.7 %) longer than the 68000
	 * timing tables give; the factor brings the charge to the measured cost. */
	ctx->cycles = (int)(cyc * 1027 / 1000);
}

/* Direct call from C with the original register contract (a0 = stream, a1 = ring, a2 =
 * output; flags as the original). Unlike the HLE entry it ADDS the original's cycle cost to
 * ctx->cycles; callers that charge a measured total (room.c) save and restore ctx->cycles. */
void lzss_call(hle_ctx *ctx)
{
	int before = ctx->cycles;
	lzss_decompress(ctx);
	ctx->cycles += before;
}

/* ------------------------------------------------------------------ RLE */

/* $009C64 rle_decompress: a0 = stream (u32 unpacked size, then tokens), a2 = output.
 * d0-d2/a0-a2 preserved. See docs/formats.md for the token format.
 *
 * Side effects reproduced exactly (difftest compares them):
 *  - if $FF09ED bit 3: VDP reg 12 := $08 (shadow/highlight) while decoding, and always
 *    reg 12 := $00 at the end (two control-port writes);
 *  - a run is written as whole longs after aligning a2 to an even address, then a2 is
 *    rewound, so up to 3 bytes beyond the run's end get the run value (harmless: the next
 *    token overwrites them, but after the LAST token they stay in memory);
 *  - the end test is `a2 == a1` where a1 = a2_start + (int16)size.
 * Flags at exit come from `move.w #$8c00,(vdp_ctrl)`: N=1 Z=V=C=0, X = X of the last run
 * token (`lsr.w #2,d0`: bit 1 of the remaining count) or the entry X if there was none. */
HLE_FUNC(0x009C64, rle_decompress, 0)
{
	uint32_t a0 = ctx->a[0], a2 = ctx->a[2];
	long cyc = 16;                               /* btst.b #3,abs.l */
	if (RAM8(RLE_FLAGS) & 8) {
		vdp_ctrl_w(0x8C08);
		cyc += 8 + 20;
	} else {
		cyc += 10;
	}
	cyc += 56 + 12 + 12;                         /* movem.l, move.l (a0)+,d0, lea */
	uint32_t a1 = a2 + (uint32_t)(int32_t)(int16_t)((stream_u8(a0 + 2) << 8) | stream_u8(a0 + 3))   /* d0.w = low word of the long */;
	a0 += 4;
	unsigned x = (ctx->sr & CC_X) ? 1 : 0;
	for (long guard = 0; guard < 0x400000; guard++) {
		unsigned c = stream_u8(a0++);
		cyc += 4 + 8;                            /* moveq, move.b (a0)+,d0 */
		if (!(c & 0x80)) {
			/* literal run of c+1 bytes */
			cyc += 8;                            /* bmi not taken */
			for (unsigned i = 0; i <= c; i++) {
				RAM8(a2++) = stream_u8(a0++);
				cyc += 12 + (i == c ? 14 : 10);  /* move.b (a0)+,(a2)+ ; dbra */
			}
		} else {
			cyc += 10 + 8 + 4 + 4 + 8;           /* bmi taken, move.b, neg.b, move.w, ror.w */
			uint8_t v = stream_u8(a0++);
			unsigned n = (0x100 - c) & 0xFF;     /* count - 1 */
			if (a2 & 1) {
				cyc += 8 + 8 + 4;                /* bcc not taken, move.b d1,(a2)+, subq.w */
				RAM8(a2++) = v;
				n = (n - 1) & 0xFFFF;
			} else {
				cyc += 10;
			}
			/* move.b, lsl.w #8, or.b, move.w, swap, move.w, move.w d0,d2, lsr.w #2, andi, eori */
			cyc += 4 + 22 + 4 + 4 + 4 + 4 + 4 + 10 + 8 + 8;
			x = (n >> 1) & 1;                    /* lsr.w #2,d0: last bit out = bit 1 */
			unsigned longs = (n >> 2) + 1;       /* dbra loop, count = (n>>2) + 1 (word) */
			unsigned back = (n & 3) ^ 3;
			for (unsigned i = 0; i < longs; i++) {
				for (int k = 0; k < 4; k++)
					RAM8(a2 + k) = v;
				a2 += 4;
				cyc += 12 + (i + 1 == longs ? 14 : 10);
			}
			a2 -= back;
			cyc += 8;                            /* suba.w d2,a2 */
		}
		cyc += 6;                                /* cmpa.l a2,a1 */
		if (a2 == a1) {
			cyc += 8;                            /* bne not taken */
			break;
		}
		cyc += 10;
	}
	cyc += 60;                                   /* movem.l (sp)+ */
	vdp_ctrl_w(0x8C00);
	cyc += 20 + 16 + 4;                          /* move.w #imm,abs.l ; rts ; +4 measured (difftest) */
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, CC_N | (x ? CC_X : 0));
	ctx->cycles = (int)cyc;
}

/* Direct call from C (see lzss_call): the original registers/flags/VDP writes, cost added. */
void rle_call(hle_ctx *ctx)
{
	int before = ctx->cycles;
	rle_decompress(ctx);
	ctx->cycles += before;
}
