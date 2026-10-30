/* VDP helpers: VRAM / CRAM uploads, fills, DMA, register writes (GitLab #13).
 * Design and the hook for a native renderer: docs/subsystems/vdp.md.
 *
 * Every 68k routine here ends in a VDP control/data port write. The C versions do the same
 * port writes in the same order (difftest compares them), but the *semantic* operation
 * ("upload N words from 68k address X to VRAM Y", "fill N words of CRAM with V") goes through
 * one small layer first: vdp_op + vdp_set_hook(). A native renderer (#25) installs a hook,
 * keeps its own VRAM/CRAM and returns true to say "handled"; then no VDP port is touched.
 * Without a hook (the default) the ops are performed on the emulated VDP. */
#include "game.h"
#include "cc68k.h"
#include <stdlib.h>

/* ------------------------------------------------------------------ semantic hook */
static vdp_hook_fn g_hook;

void vdp_set_hook(vdp_hook_fn fn) { g_hook = fn; }

/* offer an operation to the native renderer; true = it did the work */
static bool vdp_hook(vdp_op_kind kind, vdp_mem mem, bool dma, uint32_t src, uint16_t value, uint16_t dst,
                     uint32_t words)
{
	if (!g_hook)
		return false;
	vdp_op op = { kind, mem, dma, src, value, dst, words };
	return g_hook(&op);
}

/* Self-test of the hook contract (SCOOBY_VDP_HOOKTEST=1): a "renderer" that re-does each
 * operation from its description alone, through the VDP data port (DMA uploads become CPU
 * writes). If the game plays and draws identically with and without it, the vdp_op
 * descriptions are complete. See docs/subsystems/vdp.md. */
static bool selftest_hook(const vdp_op *op)
{
	uint16_t code = op->mem == VDP_CRAM ? 0xC000 : 0x4000;
	vdp_ctrl_w((uint16_t)((op->dst & 0x3FFF) | code));
	vdp_ctrl_w((uint16_t)(op->dst >> 14));
	for (uint32_t i = 0; i < op->words; i++)
		vdp_data_w(op->kind == VDP_OP_COPY ? bus_r16(op->src + 2 * i) : op->value);
	return true;
}

__attribute__((constructor)) static void vdp_selftest_init(void)
{
	if (getenv("SCOOBY_VDP_HOOKTEST"))
		vdp_set_hook(selftest_hook);
}

/* ------------------------------------------------------------------ command words */
/* Command code bits of the first control word. The second word is addr >> 14 (address
 * bits 14-15), and the DMA start word adds $80. */
#define VDP_CMD_VRAM_READ  0x0000
#define VDP_CMD_VRAM_WRITE 0x4000
#define VDP_CMD_CRAM_WRITE 0xC000

/* ROM byte $010124: the game's shadow copy of VDP register 1 (display / VBlank IRQ / DMA
 * enable bits). DMA ORs in bit 4 (DMA enable) and restores the plain value afterwards. */
#define VDP_REG1_SHADOW 0x010124

/* Set the VDP address/code the way the game does it:
 *   word 1 = (addr & $3FFF) | code ; word 2 = addr >> 14 */
static void vdp_set_addr(uint16_t code, uint16_t addr)
{
	vdp_ctrl_w((uint16_t)((addr & 0x3FFF) | code));
	vdp_ctrl_w((uint16_t)(addr >> 14));
}

/* All the "set address" routines end with the flags of `lsr.w #6,(addr>>8)` (the SR was
 * saved right after it; the `move #$2700,sr` that follows is undone by the restore). */
static void addr_flags(hle_ctx *ctx, uint16_t addr) { lsr16(ctx, addr >> 8, 6); }

static vdp_mem mem_of(uint16_t code) { return code == VDP_CMD_CRAM_WRITE ? VDP_CRAM : VDP_VRAM; }

/* ------------------------------------------------------------------ CPU-driven writes */
/* Copy `n` words from 68k address `src` (advanced past the data) to VRAM/CRAM at `dst`,
 * through the data port. n = 0 means 65536 (dbra loop on n-1). */
static void vdp_write_block(hle_ctx *ctx, uint16_t code, uint16_t dst, uint32_t n)
{
	if (!vdp_hook(VDP_OP_COPY, mem_of(code), false, ctx->a[0], 0, dst, n)) {
		vdp_set_addr(code, dst);
		for (uint32_t i = 0; i < n; i++)
			vdp_data_w(bus_r16(ctx->a[0] + 2 * i));
	}
	ctx->a[0] += 2 * n;
}

/* Write one word to VRAM/CRAM. */
static void vdp_write_one(uint16_t code, uint16_t dst, uint16_t value)
{
	if (!vdp_hook(VDP_OP_FILL, mem_of(code), false, 0, value, dst, 1)) {
		vdp_set_addr(code, dst);
		vdp_data_w(value);
	}
}

/* Fill n words (0 = 65536) of VRAM/CRAM at dst with `value`. */
static void vdp_fill_block(uint16_t code, uint16_t dst, uint32_t n, uint16_t value)
{
	if (!vdp_hook(VDP_OP_FILL, mem_of(code), false, 0, value, dst, n)) {
		vdp_set_addr(code, dst);
		for (uint32_t i = 0; i < n; i++)
			vdp_data_w(value);
	}
}

/* word count of a `dbra` loop whose counter is D1.w - 1: D1.w = 0 means 65536 */
static uint32_t loop_count(const hle_ctx *ctx) { return (uint16_t)(ctx->d[1] - 1) + 1u; }

/* $00992C vram_write_words: copy D1.w words from (A0)+ to VRAM address D0.w (PIO).
 * D0-D2 preserved, A0 advanced past the data, CCR = flags of the address shift (see
 * addr_flags). Cost: 30 cycles per word (move.w (a0)+,(vdp_data) + dbra). */
HLE_FUNC(0x00992C, vram_write_words, 0)
{
	uint16_t addr = (uint16_t)ctx->d[0];
	uint32_t n = loop_count(ctx);
	ctx->cycles = 140 + 30 * (int)n;
	addr_flags(ctx, addr);
	vdp_write_block(ctx, VDP_CMD_VRAM_WRITE, addr, n);
}

/* $009964 vram_write_word: write the word D1.w to VRAM address D0.w. All registers
 * preserved; CCR as in vram_write_words. */
HLE_FUNC(0x009964, vram_write_word, 60)
{
	uint16_t addr = (uint16_t)ctx->d[0];
	addr_flags(ctx, addr);
	vdp_write_one(VDP_CMD_VRAM_WRITE, addr, (uint16_t)ctx->d[1]);
}

/* $009996 vram_read_word (was sub_009996): D1.w = word at VRAM address D0.w. D0/D2
 * preserved, CCR as in vram_write_words. The read command has no code bits set. */
HLE_FUNC(0x009996, vram_read_word, 60)
{
	uint16_t addr = (uint16_t)ctx->d[0];
	addr_flags(ctx, addr);
	vdp_set_addr(VDP_CMD_VRAM_READ, addr);
	set_w(&ctx->d[1], vdp_data_r());
}

/* $0099C4 cram_write_word (was sub_0099C4; no static caller): write D1.w to CRAM byte
 * address D0.w. All registers preserved, CCR as in vram_write_words. */
HLE_FUNC(0x0099C4, cram_write_word, 60)
{
	uint16_t addr = (uint16_t)ctx->d[0];
	addr_flags(ctx, addr);
	vdp_write_one(VDP_CMD_CRAM_WRITE, addr, (uint16_t)ctx->d[1]);
}

/* $009BE4 vram_fill_words: fill D1.w words of VRAM at D0.w with D2.w (D1.w = 0 -> 65536).
 * D0/D1/D3 preserved; the SR is saved right after `ori.w #$4000,d0`, so the final CCR is
 * N=Z=V=C=0 and X = borrow of the earlier `subi.w #1,d1` (found by difftest). */
HLE_FUNC(0x009BE4, vram_fill_words, 0)
{
	uint16_t addr = (uint16_t)ctx->d[0];
	uint32_t n = loop_count(ctx);
	ctx->cycles = 140 + 26 * (int)n;
	cc_set(ctx, CC_X, (uint16_t)ctx->d[1] == 0 ? CC_X : 0);
	cc_logic16(ctx, (addr & 0x3FFF) | VDP_CMD_VRAM_WRITE);
	vdp_fill_block(VDP_CMD_VRAM_WRITE, addr, n, (uint16_t)ctx->d[2]);
}

/* $009C1E vdp_fill_cram (reached only by the `bra` of $009C5E): same as vram_fill_words
 * for CRAM. The `ori.w #$c000,d0` that precedes the SR save sets N, so CCR = N | X(borrow). */
HLE_FUNC(0x009C1E, vdp_fill_cram, 0)
{
	uint16_t addr = (uint16_t)ctx->d[0];
	uint32_t n = loop_count(ctx);
	ctx->cycles = 140 + 26 * (int)n;
	cc_set(ctx, CC_X, (uint16_t)ctx->d[1] == 0 ? CC_X : 0);
	cc_logic16(ctx, (addr & 0x3FFF) | VDP_CMD_CRAM_WRITE);
	vdp_fill_block(VDP_CMD_CRAM_WRITE, addr, n, (uint16_t)ctx->d[2]);
}

/* $009C58 vdp_clear_vram / $009C5E vdp_clear_cram: `clr.w d2` then fall into the fill.
 * D2.w is left 0. */
HLE_FUNC(0x009C58, vdp_clear_vram, 0)
{
	set_w(&ctx->d[2], 0);
	vram_fill_words(ctx);
	ctx->cycles += 4;
}

HLE_FUNC(0x009C5E, vdp_clear_cram, 0)
{
	set_w(&ctx->d[2], 0);
	vdp_fill_cram(ctx);
	ctx->cycles += 4;
}

/* $00972E vdp_reg_set (was sub_00972E): write VDP register D0.w with the byte D1.b:
 *   d0 = (d0 << 8) | $8000 | (d1 & $FF) ; move.w d0,(vdp_ctrl)
 * D0.w becomes the command word, D1.w is masked to a byte, CCR = flags of that move
 * (always N). */
HLE_FUNC(0x00972E, vdp_reg_set, 40)
{
	uint16_t cmd = (uint16_t)((ctx->d[0] << 8) | 0x8000 | (ctx->d[1] & 0xFF));
	set_w(&ctx->d[0], cmd);
	set_w(&ctx->d[1], ctx->d[1] & 0xFF);
	cc_logic16(ctx, cmd);
	vdp_ctrl_w(cmd);
}

/* $009716 / $009722 plane_a_clear / plane_b_clear: fill the 64x32-word nametables at
 * $C000 (plane A) and $E000 (plane B) with 0 by `bra`-ing into vdp_clear_vram. */
HLE_FUNC(0x009716, plane_a_clear, 0)
{
	set_w(&ctx->d[0], 0xC000);
	set_w(&ctx->d[1], 0x800);
	vdp_clear_vram(ctx);
	ctx->cycles += 12;
}

HLE_FUNC(0x009722, plane_b_clear, 0)
{
	set_w(&ctx->d[0], 0xE000);
	set_w(&ctx->d[1], 0x800);
	vdp_clear_vram(ctx);
	ctx->cycles += 12;
}

/* ------------------------------------------------------------------ DMA from the 68k bus */
/* DMA cost without the stall: bus request/release, 8 control words and the bookkeeping
 * of the original $009A3C/$009B10 (the stall itself is the nested time of hle_dma_*). */
#define DMA_SETUP_CYCLES 1000
/* what a hooked upload is charged per word instead of the real stall (68k->VDP DMA moves
 * a word in ~5 CPU cycles during blanking) */
#define DMA_HOOKED_CYCLES_PER_WORD 5

/* One DMA transfer of `words` words from 68k address `src` to VRAM/CRAM address `dst`
 * ($009A3C / $009B10 body). The length registers are 16 bits, so one transfer can't cross
 * the 128 KiB source wrap; vdp_dma_vram splits at 64 KiB for that.
 *   reg 1  |= $10 (DMA enable)        regs $13/$14 length (words)
 *   regs $15-$17 source address >> 1  reg $17 bits 6/7 = 0: 68k bus -> VDP
 *   ctrl words: (dst & $3FFF) | code, then $80 | (dst >> 14): this one starts the DMA
 *   the 68k is halted for the transfer; then reg 1 goes back to the plain shadow value.
 * The Z80 bus is requested around it (the Z80 would otherwise read ROM mid-transfer).
 * A0 is advanced by the bytes moved; D0-D3 and the SR are preserved. */
static void dma_chunk(hle_ctx *ctx, uint16_t code, uint32_t src, uint16_t dst, uint16_t words)
{
	uint16_t sr = ctx->sr;
	uint8_t reg1 = ROM8(VDP_REG1_SHADOW);
	hle_dma_begin();
	z80_bus_request(ctx);
	vdp_ctrl_w(0x8110 | reg1);
	vdp_ctrl_w(0x9300 | (words & 0xFF));
	vdp_ctrl_w(0x9400 | (words >> 8));
	vdp_ctrl_w(0x9500 | ((src >> 1) & 0xFF));
	vdp_ctrl_w(0x9600 | ((src >> 9) & 0xFF));
	vdp_ctrl_w(0x9700 | ((src >> 17) & 0xFF));
	vdp_ctrl_w((dst & 0x3FFF) | code);
	vdp_ctrl_w(0x80 | (dst >> 14));              /* second word starts the DMA */
	hle_dma_complete();                          /* the original polls status bit 1 here */
	vdp_ctrl_w(0x8100 | reg1);
	z80_bus_release(ctx);
	hle_dma_end();
	ctx->a[0] = src + 2u * words;
	ctx->sr = sr;
}

/* Upload hook point: ONE DMA transfer (no splitting). If a native renderer takes it, no
 * VDP port is touched and the CPU is charged a nominal per-word stall. */
static void dma_upload(hle_ctx *ctx, uint16_t code, uint32_t src, uint16_t dst, uint16_t words)
{
	if (words && vdp_hook(VDP_OP_COPY, mem_of(code), true, src, 0, dst, words)) {
		ctx->a[0] = src + 2u * words;
		ctx->cycles += DMA_HOOKED_CYCLES_PER_WORD * words;
		return;
	}
	dma_chunk(ctx, code, src, dst, words);
}

/* $009A3C vdp_dma_setup: one VRAM DMA, A0 = source, D0.w = VRAM destination, D1.w = words.
 * Registers D0-D3 and the whole SR are preserved. */
HLE_FUNC(0x009A3C, vdp_dma_setup, DMA_SETUP_CYCLES)
{
	dma_upload(ctx, VDP_CMD_VRAM_WRITE, ctx->a[0], (uint16_t)ctx->d[0], (uint16_t)ctx->d[1]);
}

/* $009B10 vdp_dma_cram: the same into CRAM (D0.w = CRAM byte address; 64 words = the whole
 * palette). The palette routines call it with D0 = 0, D1 = $40, A0 = ram_pal_cur. */
HLE_FUNC(0x009B10, vdp_dma_cram, DMA_SETUP_CYCLES)
{
	dma_upload(ctx, VDP_CMD_CRAM_WRITE, ctx->a[0], (uint16_t)ctx->d[0], (uint16_t)ctx->d[1]);
}

/* $0099F6 vdp_dma_vram: DMA D1.w words (the whole of D1.l is cleared first) from A0 to VRAM
 * D0.w. A source range that crosses a 64 KiB boundary is cut there and done as two
 * transfers. D0-D5 preserved (A0 is advanced), CCR = flags of the last compare / add. */
HLE_FUNC(0x0099F6, vdp_dma_vram, 300)
{
	uint32_t src = ctx->a[0];
	uint32_t words = ctx->d[1] & 0xFFFF;
	uint16_t dst = (uint16_t)ctx->d[0];
	uint32_t boundary = (src + 0x10000) & 0xFFFF0000u;      /* next 64 KiB boundary */
	uint32_t end = cc_add(ctx, src, words << 1, 32);        /* add.l d5,d4 (sets X) */
	cc_sub(ctx, end, boundary, 32, false);                  /* cmp.l d3,d4 (X unchanged) */

	/* the native renderer sees the whole logical upload, not the pieces */
	if (words && vdp_hook(VDP_OP_COPY, VDP_VRAM, true, src, 0, dst, words)) {
		if ((int32_t)end > (int32_t)boundary) {
			uint16_t first = (uint16_t)((boundary - src) >> 1);
			cc_sub(ctx, words, first, 16, true);                 /* sub.w d1,d5 */
			uint32_t d1 = lslw(ctx, first, 1);                   /* lsl.w #1,d1 */
			cc_add(ctx, dst, d1, 16);                            /* add.w d1,d0 */
		}
		ctx->a[0] = src + 2 * words;
		ctx->cycles += 2 * DMA_SETUP_CYCLES + DMA_HOOKED_CYCLES_PER_WORD * words;
		return;
	}
	if ((int32_t)end <= (int32_t)boundary) {                /* ble: fits in one transfer */
		dma_chunk(ctx, VDP_CMD_VRAM_WRITE, src, dst, (uint16_t)words);
		ctx->cycles += DMA_SETUP_CYCLES;
		return;
	}
	/* first piece: up to the boundary */
	uint16_t first = (uint16_t)((boundary - src) >> 1);
	dma_chunk(ctx, VDP_CMD_VRAM_WRITE, src, dst, first);
	cc_sub(ctx, words, first, 16, true);                    /* sub.w d1,d5: remaining words */
	uint16_t rest = (uint16_t)(words - first);
	uint32_t d1 = lslw(ctx, first, 1);                      /* lsl.w #1,d1: bytes (16-bit!) */
	uint16_t dst2 = (uint16_t)cc_add(ctx, dst, d1, 16);     /* add.w d1,d0 */
	dma_chunk(ctx, VDP_CMD_VRAM_WRITE, ctx->a[0], dst2, rest);
	ctx->cycles += 2 * DMA_SETUP_CYCLES;
}

/* ------------------------------------------------------------------ text on plane A */
/* Plane A is the 64x32-word nametable at $C000; a tile entry = character + ram_font_base.
 * All routines below draw a zero-terminated string at A0, column D1, row D2. */
#define PLANE_A       0xC000
#define FONT_BASE     0xFF068C   /* word: tile base added to each character (bit 15 = priority) */
#define TEXT_CHAR_CYCLES 290     /* per character: vram_write_word (227) + loop overhead */

/* $009F72 draw_text_plane_a: write the characters of the string at A0 as tile entries to
 * plane A at (D1 column, D2 row), one VRAM word each through vram_write_word.
 *   A0 ends after the terminator. D0/D1/D2 come back sign-extended from their saved LOW
 *   WORDS (`movem.w (sp)+` extends): D0 = $FFFFC000, D1/D2 = the entry words.
 *   CCR: the last instruction that matters is `move.b (a0)+,d1` reading the 0: Z, no N/V/C;
 *   X = carry of the last `addq.w #2,d0` (or of the address arithmetic for an empty string). */
HLE_FUNC(0x009F72, draw_text_plane_a, 40)
{
	uint16_t col = (uint16_t)ctx->d[1], row = (uint16_t)ctx->d[2];
	uint32_t d2 = lslw(ctx, row, 6);                    /* row * 64 */
	uint32_t d1 = lslw(ctx, col, 1);                    /* column * 2 */
	uint32_t addr = cc_add(ctx, PLANE_A, d1, 16);
	addr = cc_add(ctx, addr, d2, 16);
	unsigned x = (ctx->sr & CC_X) ? 1 : 0;
	for (;;) {
		uint8_t ch = bus_r8(ctx->a[0]++);
		if (!ch)
			break;
		set_w(&ctx->d[0], addr);
		set_w(&ctx->d[1], ch + RAM16(FONT_BASE));
		vram_write_word(ctx);
		x = (addr + 2) > 0xFFFF;
		addr = (addr + 2) & 0xFFFF;
		ctx->cycles += TEXT_CHAR_CYCLES;
	}
	ctx->d[0] = (uint32_t)(int32_t)(int16_t)PLANE_A;
	ctx->d[1] = (uint32_t)(int32_t)(int16_t)col;
	ctx->d[2] = (uint32_t)(int32_t)(int16_t)row;
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, (x ? CC_X : 0) | CC_Z);
}

/* $009F58 draw_text_plane_a_hi: the same with bit 15 (tile priority) set in the font base for
 * the duration. CCR = flags of restoring the font base word (`move.w (sp)+,(font_base)`). */
HLE_FUNC(0x009F58, draw_text_plane_a_hi, 40)
{
	uint16_t saved = RAM16(FONT_BASE);
	RAM16(FONT_BASE) = saved | 0x8000;
	draw_text_plane_a(ctx);
	RAM16(FONT_BASE) = saved;
	cc_logic16(ctx, saved);
}

/* $009F46 draw_title_text_plane_a (was sub_009F46): draw the fixed string at $02FFF4 in
 * column 0 of row D2 (high priority), then, falling into $009F58, the caller's string
 * (A0, D1 column) the same way. D1/A0 are saved around the first one. */
HLE_FUNC(0x009F46, draw_text_labelled, 40)
{
	uint32_t d1 = ctx->d[1], a0 = ctx->a[0];
	ctx->d[1] = 0;
	ctx->a[0] = 0x02FFF4;
	draw_text_plane_a_hi(ctx);
	ctx->d[1] = d1;
	ctx->a[0] = a0;
	ctx->cycles += 60;
	draw_text_plane_a_hi(ctx);
}

/* $009F30 draw_text_centered (was sub_009F30): column = (32 - strlen) / 2 (16-bit,
 * unsigned shift), then draw_text_plane_a. A0 is restored before the draw. */
HLE_FUNC(0x009F30, draw_text_centered, 80)
{
	uint16_t d1 = 1;
	uint32_t p = ctx->a[0];
	do {
		d1--;
	} while (bus_r8(p++) != 0);
	cc_sub(ctx, d1, 0, 16, false);   /* flags irrelevant: the draw resets them */
	ctx->d[1] = ((uint16_t)(d1 + 0x20)) >> 1;   /* moveq #1,d1 cleared the upper word */
	ctx->cycles += 10 * (int)(p - ctx->a[0]);
	draw_text_plane_a(ctx);
}

/* ------------------------------------------------------------------ word-wrapped text boxes */
/* The dialogue / status line text goes into window planes next to the verb bar. Both routines
 * wrap a string longer than 30 characters at the last space (or just before a '-') within its
 * first 31 characters and put the pieces on consecutive rows. They are long transliterations
 * of exact register/flag behaviour (every caller's leftovers are reproduced).
 * Word-sized 68k arithmetic on Dn is written with these helpers so X/N/Z/V/C come out right. */
#define STRLEN             0x00A236   /* strlen: A0 = cstring -> D0.w (68k, called nested) */
#define STATUS_LINE_COUNT  0xFF080A   /* word: characters (A054) / lines*300 (9FA2) of the last text */
#define UI_TILE_BASE       0xFF08B8   /* word: blank tile of the window planes */
#define VERB_BAR_WINDOW    0xFF0AD1   /* byte: value of VDP reg $12 (window v-position) for the HBlank */
#define TEXT_CHAR_CYCLES2  290        /* per character of the wrapped writers */

static void d_addw(hle_ctx *c, int r, uint32_t v) { set_w(&c->d[r], cc_add(c, c->d[r], v, 16)); }
static void d_subw(hle_ctx *c, int r, uint32_t v) { set_w(&c->d[r], cc_sub(c, c->d[r], v, 16, true)); }

/* `move.b (a0)+,d1 / andi.w #$ff,d1 / add.w font_base,d1 / bsr vram_write_word(d0,d1) /
 * addq.w #2,d0 / dbra Dcnt`: (Dcnt.w + 1) characters, Dcnt.w ends as $FFFF */
static void text_run(hle_ctx *ctx, int cnt)
{
	uint32_t n = (uint16_t)ctx->d[cnt] + 1u;
	for (uint32_t i = 0; i < n; i++) {
		set_w(&ctx->d[1], bus_r8(ctx->a[0]++));
		d_addw(ctx, 1, RAM16(FONT_BASE));
		vram_write_word(ctx);
		d_addw(ctx, 0, 2);
		ctx->cycles += TEXT_CHAR_CYCLES2;
	}
	set_w(&ctx->d[cnt], 0xFFFF);
}

/* the scan at $009FC4 / $00A086: D1 = number of characters between a space (or the char before
 * a '-') and the 31st character, counted back from A0+30; A1 left on that character */
static void wrap_scan(hle_ctx *ctx)
{
	ctx->d[1] = 0;
	ctx->a[1] = ctx->a[0] + 31;
	for (int guard = 0; guard < 0x20000; guard++) {
		ctx->a[1]--;
		if (bus_r8(ctx->a[1]) == 0x20 || bus_r8(ctx->a[1] + 1) == 0x2D)
			break;
		d_addw(ctx, 1, 1);
	}
	/* D1 = 30 - D1 (characters that stay on this row), D2 -= D1, D5 = D1 */
	d_subw(ctx, 1, 30);
	set_w(&ctx->d[1], cc_sub(ctx, 0, ctx->d[1], 16, true));       /* neg.w d1 */
	d_subw(ctx, 2, ctx->d[1]);
	set_w(&ctx->d[5], ctx->d[1]);
}

/* $009FA2 draw_text_wrapped (was sub_009FA2): draw the string at A0 at plane address
 * $AA82 + D0 * 128 + D1 * 2, wrapping at 30 characters. Leaves D0.w = number of rows used,
 * $FF080A = rows * 300 (a pause length). D2-D4 preserved; D1, D5, A0, A1 are leftovers
 * (reproduced). */
HLE_FUNC(0x009FA2, draw_text_wrapped, 200)
{
	uint32_t sv2 = ctx->d[2], sv3 = ctx->d[3], sv4 = ctx->d[4];
	set_w(&ctx->d[4], ctx->d[0]);
	set_w(&ctx->d[4], lslw(ctx, ctx->d[4], 7));
	set_w(&ctx->d[1], lslw(ctx, ctx->d[1], 1));
	d_addw(ctx, 4, ctx->d[1]);
	hle_call68k(ctx, STRLEN);
	set_w(&ctx->d[2], ctx->d[0]);
	set_w(&ctx->d[3], 0xAA82);
	d_addw(ctx, 3, ctx->d[4]);
	ctx->d[4] = 1;                                           /* rows used so far */
	while ((int16_t)ctx->d[2] > 30) {
		wrap_scan(ctx);
		set_w(&ctx->d[0], ctx->d[3]);
		d_subw(ctx, 5, 1);
		set_w(&ctx->d[1], ctx->d[1] & 0xFFFE);
		text_run(ctx, 5);
		d_addw(ctx, 3, 0x80);
		d_addw(ctx, 4, 1);
	}
	if ((uint16_t)ctx->d[2] != 0) {                          /* the last, short row */
		set_w(&ctx->d[5], ctx->d[2]);
		d_subw(ctx, 2, 1);
		set_w(&ctx->d[0], ctx->d[3]);
		set_w(&ctx->d[5], ctx->d[5] & 0xFFFE);
		text_run(ctx, 2);
	}
	set_w(&ctx->d[0], ctx->d[4]);
	uint32_t prod = (uint32_t)(uint16_t)ctx->d[4] * 300u;    /* mulu.w #300,d4 */
	ctx->d[4] = prod;
	RAM16(STATUS_LINE_COUNT) = (uint16_t)prod;
	cc_logic16(ctx, prod);
	ctx->d[2] = sv2;
	ctx->d[3] = sv3;
	ctx->d[4] = sv4;
}

/* $00A054 draw_status_line (was sub_00A054): clear the 64x16 words at plane $D000 with the
 * blank tile (1024 single-word writes), then draw the string at A0 centred-ish in rows from
 * $D042, wrapping like draw_text_wrapped but right-aligned (each row starts at
 * $D042 + 30 - (len & ~1)). Sets reg $12 (window position) from the row count and
 * $FF080A = characters * 16. All registers are leftovers (reproduced). */
HLE_FUNC(0x00A054, draw_status_line, 200)
{
	set_w(&ctx->d[2], 0x3FF);
	set_w(&ctx->d[1], RAM16(UI_TILE_BASE));
	set_w(&ctx->d[0], 0xD000);
	for (uint32_t i = 0; i < 1024; i++) {
		vram_write_word(ctx);
		d_addw(ctx, 0, 2);
		ctx->cycles += 272;
	}
	set_w(&ctx->d[2], 0xFFFF);
	hle_call68k(ctx, STRLEN);
	set_w(&ctx->d[2], ctx->d[0]);
	RAM16(STATUS_LINE_COUNT) = (uint16_t)ctx->d[2];
	set_w(&ctx->d[3], 0xD042);
	ctx->d[4] = 2;                                           /* rows used so far + 1 */
	while ((int16_t)ctx->d[2] > 30) {
		wrap_scan(ctx);
		set_w(&ctx->d[0], ctx->d[3]);
		d_subw(ctx, 5, 1);
		set_w(&ctx->d[1], ctx->d[1] & 0xFFFE);
		d_addw(ctx, 0, 0x1E);
		d_subw(ctx, 0, ctx->d[1]);
		text_run(ctx, 5);
		d_addw(ctx, 3, 0x40);
		d_addw(ctx, 4, 1);
	}
	if ((uint16_t)ctx->d[2] != 0) {
		set_w(&ctx->d[5], ctx->d[2]);
		d_subw(ctx, 2, 1);
		set_w(&ctx->d[0], ctx->d[3]);
		set_w(&ctx->d[5], ctx->d[5] & 0xFFFE);
		d_addw(ctx, 0, 0x1E);
		d_subw(ctx, 0, ctx->d[5]);
		text_run(ctx, 2);
	}
	set_w(&ctx->d[1], ctx->d[4]);
	RAM8(VERB_BAR_WINDOW) = (uint8_t)ctx->d[1];
	set_w(&ctx->d[0], 0x12);
	vdp_reg_set(ctx);
	ctx->cycles += 40;
	set_w(&ctx->d[4], RAM16(STATUS_LINE_COUNT));
	set_w(&ctx->d[4], lslw(ctx, ctx->d[4], 4));
	RAM16(STATUS_LINE_COUNT) = (uint16_t)ctx->d[4];
	cc_logic16(ctx, ctx->d[4]);
}
