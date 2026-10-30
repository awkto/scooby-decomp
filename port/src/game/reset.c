/* Reset: power-on, logos, title hand-off and game start (GitLab #12).
 * docs/subsystems/mainloop.md has the scene flow; this file follows it in address order.
 *
 * Reset ($000200) is one long piece of straight-line code with the main loop at its end.
 * It is ported as HLE_RANGEs that each stop in front of a call that can wait for VBlank
 * (palette_fade, wait_vblank(s), palette_read_cram, the title menu, scripts...; see
 * tools/py/blocking.py). The original jsr makes that call and the code after it is the
 * next range, so these C pieces never run while an interrupt is due. Every range runs
 * once per boot: difftest covers all calls on the capture route (power-on plus the soft
 * resets of "quit" in the pause menu).
 *
 * Cross-subsystem callees go through hle_call68k (they are C when their subsystem has
 * replaced them): vdp_copy_to_vram $00992C, vdp_clear_vram $009C58, vdp_clear_cram
 * $009C5E, vdp_dma_vram $0099F6, vdp_dma_cram $009B10, lzss_decompress $0097AA,
 * sound_init $1F6EA8, sound_play $009762, sound_cmd_stop_all $1F6F52, rng_next $0022D6.
 */
#include "game.h"
#include "cc68k.h"

#define VDP_COPY      0x00992C   /* vdp_copy_to_vram: d0 = VRAM addr, a0 = src, d1 = words */
#define VDP_CLEAR     0x009C58   /* vdp_clear_vram: d0 = addr, d1 = words */
#define VDP_CLEAR_CRAM 0x009C5E
#define VDP_DMA_VRAM  0x0099F6
#define VDP_DMA_CRAM  0x009B10
#define LZSS          0x0097AA   /* a0 = stream, a1 = ring buffer, a2 = output */
#define RNG_NEXT      0x0022D6

#define LZSS_RING     0xFF3000
#define DECOMP_BUF    0xFF4000
#define BUF_5000      0xFF5000
#define PAL_CUR       0xFF06F8   /* ram_pal_cur: 64-word CRAM image */
#define HSCROLL_A     0xFF07F8
#define HSCROLL_B     0xFF07FA
#define LOGO_FLAGS    0xFF09EB   /* ram_09eb: bit1 = no camera scroll in vblank_main (logos) */
#define BOOT_FLAGS    0xFF09ED   /* ram_09ed: bit0 game running, bit2 restored from password */

/* "move.w #imm,sr": the whole status register, CCR included */
static void set_sr(hle_ctx *ctx, uint16_t v) { ctx->sr = v; }

static void set_w_logic(hle_ctx *ctx, int reg, uint16_t v)
{
	set_w(&ctx->d[reg], v);
	cc_logic16(ctx, v);
}

/* moveq #v,dN */
static void moveq(hle_ctx *ctx, int reg, int8_t v)
{
	ctx->d[reg] = (uint32_t)(int32_t)v;
	cc_logic32(ctx, ctx->d[reg]);
}

/* Copy `rows` rows of `words` words from ROM a0 into a plane at VRAM `addr` (rows are
 * 64 cells = $80 bytes apart):  move.w #addr,d0 / lea src,a0 / move.w #words,d1 /
 * move.w #rows-1,d2 / - jsr vdp_copy_to_vram / addi.w #$80,d0 / dbra d2,- */
static void copy_map(hle_ctx *ctx, uint16_t addr, uint32_t src, uint16_t words, uint16_t rows)
{
	set_w_logic(ctx, 0, addr);
	ctx->a[0] = src;
	set_w_logic(ctx, 1, words);
	set_w_logic(ctx, 2, (uint16_t)(rows - 1));
	do {
		hle_call68k(ctx, VDP_COPY);
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 0x80, 16));
		set_w(&ctx->d[2], ctx->d[2] - 1);
	} while ((uint16_t)ctx->d[2] != 0xFFFF);
}

/* LZSS-decompress `src` into DECOMP_BUF and copy the result to VRAM 0:
 *   lea src,a0 / lea ram_lzss_ring,a1 / lea ram_decomp_buf,a2 / jsr lzss_decompress /
 *   movea.l a2,a0 / moveq #0,d0 / move.l (a1),d1 / lsr.w #1,d1 / jsr vdp_copy_to_vram
 * (the decompressor leaves the byte size in the first long of the ring buffer) */
static void unpack_tiles(hle_ctx *ctx, uint32_t src)
{
	ctx->a[0] = src;
	ctx->a[1] = LZSS_RING;
	ctx->a[2] = DECOMP_BUF;
	hle_call68k(ctx, LZSS);
	ctx->a[0] = ctx->a[2];
	moveq(ctx, 0, 0);
	ctx->d[1] = bus_r32(ctx->a[1]);
	cc_logic32(ctx, ctx->d[1]);
	set_w(&ctx->d[1], lsrw(ctx, ctx->d[1], 1));
	hle_call68k(ctx, VDP_COPY);
}

/* move.w #$8000,d1 / moveq #0,d0 / jsr vdp_clear_vram: clear all 64 KiB of VRAM */
static void clear_vram(hle_ctx *ctx)
{
	set_w_logic(ctx, 1, 0x8000);
	moveq(ctx, 0, 0);
	hle_call68k(ctx, VDP_CLEAR);
}

/* ================================================================== power-on */

/* $000200 boot_hw_init: the standard Sega start-up (same code as most Genesis games).
 * Skipped when the I/O control ports are already set up (soft reset: "quit" jumps to the
 * reset vector). Table at $00028E: d5-d7, a0-a4, 24 VDP register values, the VRAM-fill
 * command, a 38-byte Z80 program, two more VDP register writes, the CRAM and VSRAM write
 * commands and 4 PSG mute bytes. Ends with every register 0, USP 0 and SR = $2700. */
HLE_RANGE(0x000200, boot_hw_init, 300000, 0x0002FA)
{
	uint32_t ctl = bus_r32(0xA10008);
	cc_logic32(ctx, ctl);
	if (!ctl) {
		uint16_t c3 = bus_r16(0xA1000C);
		cc_logic16(ctx, c3);
		ctl = c3;
	}
	if (ctl) {                                   /* already initialised: soft reset */
		hle_jump(ctx, 0x0002FA);
		return;
	}
	uint32_t t = 0x00028E;                       /* lea dat_00028E(pc),a5 */
	set_w(&ctx->d[5], ROM16(t));                 /* movem.w (a5)+,d5-d7: sign-extended */
	set_w(&ctx->d[6], ROM16(t + 2));
	set_w(&ctx->d[7], ROM16(t + 4));
	for (int r = 5; r <= 7; r++)
		ctx->d[r] = (uint32_t)(int32_t)(int16_t)ctx->d[r];
	for (int r = 0; r < 5; r++)                  /* movem.l (a5)+,a0-a4 */
		ctx->a[r] = ROM32(t + 6 + 4 * r);
	t += 26;
	const uint32_t z80_busreq = ctx->a[1], z80_reset = ctx->a[2], vdp_data = ctx->a[3], vdp_ctrl = ctx->a[4];

	/* TMSS: unlock the VDP if the version register has a non-zero hardware revision */
	uint8_t ver = bus_r8(z80_busreq - 0x10FF);
	set_b(&ctx->d[0], ver & 0xF);
	cc_logic8(ctx, ctx->d[0]);
	if (ver & 0xF)
		bus_w32(z80_busreq + 0x2F00, 0x53454741);    /* 'SEGA' -> $A14000 */

	set_w(&ctx->d[0], bus_r16(vdp_ctrl));         /* clear the VDP's pending command */
	ctx->d[0] = 0;
	ctx->a[6] = 0;
	hle_set_usp(0);
	/* 24 VDP registers: $8000|(n<<8)|value */
	for (int n = 0; n < 24; n++) {
		set_b(&ctx->d[5], ROM8(t++));
		bus_w16(vdp_ctrl, (uint16_t)ctx->d[5]);
		set_w(&ctx->d[5], ctx->d[5] + ctx->d[7]);
	}
	ctx->d[1] = (ctx->d[1] & 0xFFFF0000u) | 0xFFFF;
	bus_w32(vdp_ctrl, ROM32(t));                 /* VRAM fill DMA of 64 KiB with 0 */
	t += 4;
	bus_w16(vdp_data, 0);

	/* Z80: take the bus, release reset, load the 38-byte stub, reset it */
	bus_w16(z80_busreq, (uint16_t)ctx->d[7]);
	bus_w16(z80_reset, (uint16_t)ctx->d[7]);
	for (long guard = 0; (bus_r8(z80_busreq) & 1) && guard < 1000000; guard++)
		;
	for (int i = 0; i < 0x26; i++)
		bus_w8(ctx->a[0]++, ROM8(t++));
	ctx->d[2] = (ctx->d[2] & 0xFFFF0000u) | 0xFFFF;
	bus_w16(z80_reset, 0);
	bus_w16(z80_busreq, 0);
	bus_w16(z80_reset, (uint16_t)ctx->d[7]);

	/* clear the 64 KiB of work RAM: move.l d0,-(a6) with a6 = 0, $4000 times */
	for (uint32_t a = 0xFF0000; a < 0x1000000; a += 2)
		RAM16(a) = 0;
	ctx->a[6] = 0xFFFF0000u;
	ctx->d[6] = (ctx->d[6] & 0xFFFF0000u) | 0xFFFF;

	bus_w32(vdp_ctrl, ROM32(t));                 /* reg 1 = $04, reg 15 = $02 */
	t += 4;
	bus_w32(vdp_ctrl, ROM32(t));                 /* clear CRAM */
	t += 4;
	for (int i = 0; i < 32; i++)
		bus_w32(vdp_data, 0);
	bus_w32(vdp_ctrl, ROM32(t));                 /* clear VSRAM */
	t += 4;
	for (int i = 0; i < 20; i++)
		bus_w32(vdp_data, 0);
	for (int i = 0; i < 4; i++)                  /* PSG: silence the 4 channels */
		bus_w8(vdp_data + 0x11, ROM8(t++));
	bus_w16(z80_reset, 0);

	/* movem.l (a6),d0-d7/a0-a6: a6 = $FFFF0000 points at cleared RAM -> all zero */
	for (int r = 0; r < 8; r++)
		ctx->d[r] = 0;
	for (int r = 0; r < 7; r++)
		ctx->a[r] = 0;
	set_sr(ctx, 0x2700);
	hle_jump(ctx, 0x0002FA);
}

/* $0002FA region_check: the console region (version register bits 6-7, mapped through
 * the 4 letters at $000436) must be one of the header's region letters at $0001F0.
 * Ours says "JUE", so the check always passes. Otherwise the 68k draws the lockout
 * screen ($000334) or hangs ($0003FE, region code without a letter). */
HLE_RANGE(0x0002FA, region_check, 300, 0x000690, 0x000334, 0x0003FE)
{
	cc_logic16(ctx, vdp_status_r());
	ctx->d[0] = 0;
	cc_logic32(ctx, 0);
	set_b(&ctx->d[0], bus_r8(0xA10001));
	/* lsr.b #6,d0: X = C = bit 5 */
	uint8_t v = (uint8_t)ctx->d[0];
	unsigned xc = (v >> 5) & 1 ? CC_X | CC_C : 0;
	v >>= 6;
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, xc | (v ? 0 : CC_Z));
	set_b(&ctx->d[0], v & 3);
	cc_logic8(ctx, ctx->d[0]);
	ctx->a[0] = 0x000436;
	set_b(&ctx->d[0], ROM8(0x000436 + (int16_t)ctx->d[0]));
	cc_logic8(ctx, ctx->d[0]);
	if (!(ctx->d[0] & 0xFF)) {
		hle_jump(ctx, 0x0003FE);
		return;
	}
	ctx->a[0] = 0x0001F0;
	set_w(&ctx->d[1], 0xF);
	cc_logic16(ctx, 0xF);
	do {
		cc_sub(ctx, ctx->d[0], ROM8(ctx->a[0]), 8, false);
		if ((uint8_t)ctx->d[0] == ROM8(ctx->a[0])) {
			hle_jump(ctx, 0x000690);
			return;
		}
		ctx->a[0]++;
		set_w(&ctx->d[1], ctx->d[1] - 1);
	} while ((uint16_t)ctx->d[1] != 0xFFFF);
	hle_jump(ctx, 0x000334);
}

/* $00069A factory_test: a test fixture on controller port 2 is recognised by its
 * TH/TR handshake (with TH=1 bits 1-2 read 11, with TH=0 they read 00; a pad reads
 * differently). Then the ROM checksum is tested, the result shown and reported on port 2,
 * and the CPU halts. With a normal pad (or none) it returns at once. The test itself
 * (from $0006F2) stays 68k: the C only makes the decision. */
HLE_FUNC(0x00069A, factory_test, 200)
{
	uint32_t saved_sp = ctx->a[7];
	/* movem.l d0-d7/a0-a6,-(sp) */
	for (int r = 14; r >= 0; r--)
		push32(ctx, r < 8 ? ctx->d[r] : ctx->a[r - 8]);
	uint32_t entry_d0 = ctx->d[0];
	bus_w8(0xA10019, 0x00);
	bus_w8(0xA1000B, 0x39);
	bus_w8(0xA10005, 0x00);
	uint8_t b = bus_r8(0xA10005);                /* bset #3,(io_data2) */
	cc_btst(ctx, b, 3);
	bus_w8(0xA10005, b | 0x08);
	b = bus_r8(0xA10005);                        /* bset #0,(io_data2) */
	cc_btst(ctx, b, 0);
	bus_w8(0xA10005, b | 0x01);
	set_b(&ctx->d[0], bus_r8(0xA10005) & 6);
	cc_sub(ctx, ctx->d[0], 6, 8, false);
	bool fixture = (ctx->d[0] & 0xFF) == 6;
	if (fixture) {
		b = bus_r8(0xA10005);                    /* bclr #0,(io_data2) */
		cc_btst(ctx, b, 0);
		bus_w8(0xA10005, b & ~0x01);
		set_b(&ctx->d[0], bus_r8(0xA10005) & 6);
		cc_sub(ctx, ctx->d[0], 0, 8, false);
		fixture = (ctx->d[0] & 0xFF) == 0;
	}
	if (fixture) {
		hle_jump(ctx, 0x0006F2);                 /* the test screen (never returns) */
		return;
	}
	/* movem.l (sp)+,d0-d7/a0-a6: all registers as on entry (flags from the last cmpi) */
	ctx->d[0] = entry_d0;
	ctx->a[7] = saved_sp;
}

/* $00A27A game_reset: reset the game variables (also the tail of game_init).
 * Ends with the shared palettes 0-1 (ROM $032480) copied to ram_pal_target. */
HLE_FUNC(0x00A27A, game_reset, 900)
{
	static const uint32_t clear_w[] = { 0xFF06EC, 0xFF069E, 0xFF0800, 0xFF06AC };
	for (unsigned i = 0; i < sizeof clear_w / sizeof *clear_w; i++)
		RAM16(clear_w[i]) = 0;
	RAM16(0xFF065A) = 0xFFFF;
	RAM16(0xFF0670) = 0xFFFF;
	RAM8(0xFF0ABE) = 0;
	RAM8(PAD1_CUR) = 0xFF;                       /* nothing pressed (active low) */
	RAM8(PAD1_PREV) = 0xFF;
	RAM8(PAD2_CUR) = 0xFF;
	RAM8(PAD2_PREV) = 0xFF;
	RAM16(0xFF080C) = 4;
	RAM32_W(0xFF0692, 0);                        /* ram_room_data_ptr */
	RAM16(0xFF06CA) = 0;                         /* camera */
	RAM16(0xFF06CE) = 0;
	RAM16(0xFF0688) = 0;
	RAM16(0xFF0804) = 0;                         /* status meters */
	RAM16(0xFF0806) = 0;
	static const uint32_t clear_b[] = { VBLANK_FLAGS, 0xFF09DE, 0xFF09E8, 0xFF09E9, 0xFF09EA, 0xFF09EB,
	                                    0xFF0AB3, 0xFF0AC9, SCRIPT_FLAGS2, 0xFF0AB1, 0xFF0AB2 };
	for (unsigned i = 0; i < sizeof clear_b / sizeof *clear_b; i++)
		RAM8(clear_b[i]) = 0;
	RAM8(0xFF09EE) = ROM8(0x010124);
	static const uint32_t clear_w2[] = { HSCROLL_A, HSCROLL_B, 0xFF07FC, 0xFF07FE, 0xFF06AE };
	for (unsigned i = 0; i < sizeof clear_w2 / sizeof *clear_w2; i++)
		RAM16(clear_w2[i]) = 0;
	RAM8(VBL_COUNTDOWN_B) = 0;
	RAM8(0xFF0ACB) = 0xFF;
	/* lea ram_pal_target,a0 / lea shared_palettes,a1 / move.w #$1F,d0 / - move.w (a1)+,(a0)+ / dbra */
	ctx->a[0] = 0xFF0778;
	ctx->a[1] = 0x032480;
	for (int i = 0; i < 32; i++) {
		uint16_t w = ROM16(ctx->a[1]);
		RAM16(ctx->a[0]) = w;
		cc_logic16(ctx, w);
		ctx->a[0] += 2;
		ctx->a[1] += 2;
	}
	ctx->d[0] = (ctx->d[0] & 0xFFFF0000u) | 0xFFFF;
}

/* $00A248 game_init stays 68k: it seeds the RNG ($45C3F2D1) and steps it once with the HV
 * counter as the range; the HV value ends up in the X flag (rng_next's last lsr), so its
 * result depends on the exact CPU time of the read, which C code cannot reproduce (see
 * docs/porting.md, "Timing-dependent reads"). It falls into game_reset, which is C. */

/* ================================================================== boot sequence */

/* $0008CE boot_init: hooks, sound driver, title music, game_init, clear VRAM/CRAM, VDP
 * registers 0-18 from $010123, then logo 1 (12x4 map $023226 at $C594, tiles LZSS
 * $023286) up to its fade-in (palette $0236A0) with interrupts enabled. */
HLE_RANGE(0x0008CE, boot_init, 20000, 0x0009AC)
{
	RAM32_W(VBLANK_HOOK, 0x00A65C);              /* vblank_main */
	RAM32_W(HBLANK_HOOK, 0x01006C);              /* hblank_verb_bar */
	/* sound_init(driver $1BD1D0, $1BE31F, $1BE496, $1C70C5): Z80 driver + data pointers */
	push32(ctx, 0x1C70C5);
	push32(ctx, 0x1BE496);
	push32(ctx, 0x1BE31F);
	push32(ctx, 0x1BD1D0);
	hle_call68k(ctx, 0x1F6EA8);
	ctx->a[7] += 16;
	ctx->d[0] = 0x2F;
	RAM32_W(0xFF06A6, 0x2F);                     /* ram_cur_music */
	cc_logic32(ctx, 0x2F);
	hle_call68k(ctx, 0x009762);                  /* sound_play($2F): title music */
	hle_call68k(ctx, 0x00A248);                  /* game_init */
	set_w_logic(ctx, 0, 0);
	set_w_logic(ctx, 1, 0x8000);
	hle_call68k(ctx, VDP_CLEAR);
	set_w_logic(ctx, 1, 0x40);
	hle_call68k(ctx, VDP_CLEAR_CRAM);
	/* VDP registers 18..0 from the table at $010123 */
	set_w_logic(ctx, 0, 0x12);
	ctx->a[1] = 0x010123;
	do {
		uint16_t r = (uint16_t)ctx->d[0];
		/* move.w d0,d1 / ori.b #$80,d1 / lsl.w #8,d1 / or.b (a1,d0.w),d1 */
		set_w(&ctx->d[1], (uint16_t)(((r | 0x80) & 0xFF) << 8) | ROM8(0x010123 + (int16_t)r));
		cc_logic8(ctx, ctx->d[1]);
		vdp_ctrl_w((uint16_t)ctx->d[1]);
		cc_logic16(ctx, ctx->d[1]);
		set_w(&ctx->d[0], ctx->d[0] - 1);
	} while ((uint16_t)ctx->d[0] != 0xFFFF);
	RAM32_W(HBLANK_HOOK, 0x01006A);              /* hblank_nop */
	/* logo 1 */
	copy_map(ctx, 0xC594, 0x023226, 12, 4);
	unpack_tiles(ctx, 0x023286);
	ctx->a[0] = 0x0236A0;
	set_sr(ctx, 0x2000);
	hle_jump(ctx, 0x0009AC);                     /* jsr palette_fade */
}

/* $0009B2: hold logo 1 for 51 frames */
HLE_RANGE(0x0009B2, logo1_hold, 20, 0x0009B6)
{
	set_w_logic(ctx, 0, 0x32);
	hle_jump(ctx, 0x0009B6);                     /* jsr wait_vblanks */
}

/* Logo 1 shine: rotate CRAM image words 2..10 by one (word 10 -> 2):
 *   movea.l a2,a1 / move.w (a1),d1 / move.w #7,d0 / - move.w -(a1),2(a1) / dbra d0,- /
 *   move.w d1,2(a1)  then jsr wait_vblank */
static void logo1_rotate(hle_ctx *ctx)
{
	ctx->a[1] = ctx->a[2];
	set_w(&ctx->d[1], RAM16(ctx->a[1]));
	set_w(&ctx->d[0], 7);
	for (int i = 0; i < 8; i++) {
		ctx->a[1] -= 2;
		RAM16(ctx->a[1] + 2) = RAM16(ctx->a[1]);
	}
	ctx->d[0] = (ctx->d[0] & 0xFFFF0000u) | 0xFFFF;
	RAM16(ctx->a[1] + 2) = (uint16_t)ctx->d[1];
	cc_logic16(ctx, ctx->d[1]);
	hle_jump(ctx, 0x0009DA);
}

/* $0009BC: 8 shine steps (d2), each: rotate, wait_vblank, upload the palette, 2 more frames */
HLE_RANGE(0x0009BC, logo1_shine, 300, 0x0009DA)
{
	set_w_logic(ctx, 2, 7);
	ctx->a[2] = PAL_CUR + 20;
	logo1_rotate(ctx);
}

/* $0009E0: upload the rotated palette (64 words to CRAM 0) */
HLE_RANGE(0x0009E0, logo1_shine_upload, 300, 0x0009F4)
{
	ctx->a[0] = PAL_CUR;
	set_w_logic(ctx, 1, 0x40);
	moveq(ctx, 0, 0);
	hle_call68k(ctx, VDP_DMA_CRAM);
	moveq(ctx, 0, 2);
	hle_jump(ctx, 0x0009F4);                     /* jsr wait_vblanks */
}

/* $0009FA: next shine step, or hold 101 frames */
HLE_RANGE(0x0009FA, logo1_shine_next, 300, 0x0009DA, 0x000A02)
{
	set_w(&ctx->d[2], ctx->d[2] - 1);
	if ((uint16_t)ctx->d[2] != 0xFFFF) {
		logo1_rotate(ctx);
		return;
	}
	set_w_logic(ctx, 0, 0x64);
	hle_jump(ctx, 0x000A02);                     /* jsr wait_vblanks */
}

/* $000A0E logo 2 (H40): 40x28 map $0278E0, tiles $023720, palette $0281A0 */
HLE_RANGE(0x000A0E, logo2, 20000, 0x000A62)
{
	set_sr(ctx, 0x2700);
	clear_vram(ctx);
	vdp_ctrl_w(0x8C81);                          /* H40 */
	copy_map(ctx, 0xC000, 0x0278E0, 40, 28);
	moveq(ctx, 0, 0);
	ctx->a[0] = 0x023720;
	set_w_logic(ctx, 1, 0x20E0);
	hle_call68k(ctx, VDP_COPY);
	ctx->a[0] = 0x0281A0;
	set_sr(ctx, 0x2000);
	hle_jump(ctx, 0x000A62);                     /* jsr palette_fade */
}

/* $000A68: hold logo 2 for 301 frames */
HLE_RANGE(0x000A68, logo2_hold, 20, 0x000A6C)
{
	set_w_logic(ctx, 0, 300);
	hle_jump(ctx, 0x000A6C);
}

/* $000A78 logo 3 (H32): maps $0219C0 (plane A $C480) and $021B40 (plane B $E480),
 * tiles LZSS $021E40, palette $02241A; plane B starts 256 px to the left */
HLE_RANGE(0x000A78, logo3, 20000, 0x000B08)
{
	clear_vram(ctx);
	vdp_ctrl_w(0x8C00);                          /* H32 */
	cc_btst(ctx, RAM8(LOGO_FLAGS), 1);
	RAM8(LOGO_FLAGS) |= 2;                       /* vblank_main: no camera scroll */
	RAM16(HSCROLL_A) = 0;
	cc_logic16(ctx, 0);
	copy_map(ctx, 0xC480, 0x0219C0, 0x20, 6);
	set_w_logic(ctx, 0, 0xE480);
	ctx->a[0] = 0x021B40;
	set_w_logic(ctx, 1, 0x180);
	hle_call68k(ctx, VDP_COPY);
	unpack_tiles(ctx, 0x021E40);
	ctx->a[0] = 0x02241A;
	RAM16(HSCROLL_B) = 0xFF00;
	cc_logic16(ctx, 0xFF00);
	set_sr(ctx, 0x2000);
	hle_jump(ctx, 0x000B08);                     /* jsr palette_fade */
}

/* $000B0E: plane B slides in from -256 to +48, 4 px per frame */
HLE_RANGE(0x000B0E, logo3_slide, 20, 0x000B12)
{
	set_w_logic(ctx, 0, 0xFF00);
	hle_jump(ctx, 0x000B12);                     /* jsr wait_vblank */
}

HLE_RANGE(0x000B18, logo3_slide_step, 40, 0x000B12, 0x000B2C)
{
	set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 4, 16));
	RAM16(HSCROLL_B) = (uint16_t)ctx->d[0];
	cc_logic16(ctx, ctx->d[0]);
	if (cmpw(ctx, ctx->d[0], 0x30) < 0) {
		hle_jump(ctx, 0x000B12);
		return;
	}
	set_w_logic(ctx, 0, 0xC8);                   /* then hold 201 frames */
	hle_jump(ctx, 0x000B2C);
}

/* $000B38 logo 4: 26x9 map $02249A at $C406, tiles LZSS $02266E, palette $0231A6 */
HLE_RANGE(0x000B38, logo4, 20000, 0x000BA0)
{
	set_sr(ctx, 0x2700);
	clear_vram(ctx);
	cc_btst(ctx, RAM8(LOGO_FLAGS), 1);
	RAM8(LOGO_FLAGS) &= (uint8_t)~2;
	copy_map(ctx, 0xC406, 0x02249A, 0x1A, 9);
	unpack_tiles(ctx, 0x02266E);
	ctx->a[0] = 0x0231A6;
	set_sr(ctx, 0x2000);
	hle_jump(ctx, 0x000BA0);                     /* jsr palette_fade */
}

/* $000BA6: hold logo 4 for 301 frames */
HLE_RANGE(0x000BA6, logo4_hold, 20, 0x000BAA)
{
	set_w_logic(ctx, 0, 300);
	hle_jump(ctx, 0x000BAA);
}

/* $000BC8 logo5_frame: show animation frame d3 of logo 5: 5 rows of 20 words from
 * ram_decomp_buf + 200*d3 to the plane at $C410 */
HLE_FUNC(0x000BC8, logo5_frame, 600)
{
	ctx->a[0] = DECOMP_BUF;
	set_w(&ctx->d[0], ctx->d[3]);
	ctx->d[0] = (ctx->d[0] & 0xFFFF) * 0xC8;
	ctx->a[0] += (int16_t)ctx->d[0];
	set_w(&ctx->d[1], 0x14);
	set_w(&ctx->d[2], 4);
	set_w(&ctx->d[0], 0xC410);
	do {
		hle_call68k(ctx, VDP_DMA_VRAM);
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 0x80, 16));
		set_w(&ctx->d[2], ctx->d[2] - 1);
	} while ((uint16_t)ctx->d[2] != 0xFFFF);
}

/* $000BB6 logo 5: tiles LZSS $0287C8, the 15 animation frames (LZSS $028220 ->
 * ram_decomp_buf) and the final picture (LZSS $02864C -> $FF5000); frame 0, palette $02967A */
HLE_RANGE(0x000BB6, logo5, 40000, 0x000C48)
{
	set_sr(ctx, 0x2700);
	clear_vram(ctx);
	unpack_tiles(ctx, 0x0287C8);
	ctx->a[0] = 0x028220;
	hle_call68k(ctx, LZSS);
	ctx->a[2] = BUF_5000;
	ctx->a[0] = 0x02864C;
	hle_call68k(ctx, LZSS);
	moveq(ctx, 3, 0);
	logo5_frame(ctx);
	set_sr(ctx, 0x2000);
	ctx->a[0] = 0x02967A;
	hle_jump(ctx, 0x000C48);                     /* jsr palette_fade */
}

/* $000C4E: play the animation 4 times (d4), 15 frames each (d3), 3 VBlanks per frame */
HLE_RANGE(0x000C4E, logo5_anim, 30, 0x000C54)
{
	moveq(ctx, 4, 3);
	moveq(ctx, 3, 0);
	moveq(ctx, 0, 2);
	hle_jump(ctx, 0x000C54);                     /* jsr wait_vblanks */
}

HLE_RANGE(0x000C5A, logo5_anim_step, 700, 0x000C54, 0x000C90)
{
	logo5_frame(ctx);
	set_w(&ctx->d[3], cc_add(ctx, ctx->d[3], 1, 16));
	if (cmpw(ctx, ctx->d[3], 0xF) != 0) {
		moveq(ctx, 0, 2);
		hle_jump(ctx, 0x000C54);
		return;
	}
	set_w(&ctx->d[4], ctx->d[4] - 1);
	if ((uint16_t)ctx->d[4] != 0xFFFF) {
		moveq(ctx, 3, 0);
		moveq(ctx, 0, 2);
		hle_jump(ctx, 0x000C54);
		return;
	}
	/* final picture: 25x15 words from $FF5000 at $C288, hold 201 frames */
	set_w_logic(ctx, 0, 0xC288);
	ctx->a[0] = BUF_5000;
	set_w_logic(ctx, 1, 0x19);
	set_w_logic(ctx, 2, 0xE);
	do {
		hle_call68k(ctx, VDP_COPY);
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 0x80, 16));
		set_w(&ctx->d[2], ctx->d[2] - 1);
	} while ((uint16_t)ctx->d[2] != 0xFFFF);
	set_w_logic(ctx, 0, 0xC8);
	hle_jump(ctx, 0x000C90);                     /* jsr wait_vblanks */
}

/* $000C9C: logos done; clear VRAM and enter the title screen (bsr sub_008196) */
HLE_RANGE(0x000C9C, logos_done, 20000, 0x000CB4)
{
	set_sr(ctx, 0x2700);
	clear_vram(ctx);
	set_sr(ctx, 0x2700);
	set_sr(ctx, 0x2000);
	hle_jump(ctx, 0x000CB4);
}

/* $000CB8: a game was started from the menu: stop the music, reset the game variables,
 * then load the mystery (bsr sub_007F66) */
HLE_RANGE(0x000CB8, game_start, 2000, 0x000CC8)
{
	hle_call68k(ctx, 0x1F6F52);                  /* sound_cmd_stop_all */
	set_sr(ctx, 0x2700);
	hle_call68k(ctx, 0x00A27A);                  /* game_reset */
	hle_jump(ctx, 0x000CC8);
}

/* $000CD0: run the mystery's start-up scripts with VBF_SCRIPT set (script_run_frame) */
HLE_RANGE(0x000CD0, game_start_scripts, 60, 0x000CE8)
{
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 7);
	RAM8(VBLANK_FLAGS) |= VBF_SCRIPT;
	cc_btst(ctx, RAM8(SCRIPT_FLAGS2), 1);
	RAM8(SCRIPT_FLAGS2) &= (uint8_t)~2;
	cc_btst(ctx, RAM8(SCRIPT_FLAGS2), 0);
	RAM8(SCRIPT_FLAGS2) &= (uint8_t)~1;
	hle_jump(ctx, 0x000CE8);
}

/* $000CEC: the second start-up script follows the first: a6 points at its length word */
HLE_RANGE(0x000CEC, game_start_script2, 60, 0x000D04)
{
	cc_btst(ctx, RAM8(SCRIPT_FLAGS2), 1);
	RAM8(SCRIPT_FLAGS2) &= (uint8_t)~2;
	cc_btst(ctx, RAM8(SCRIPT_FLAGS2), 0);
	RAM8(SCRIPT_FLAGS2) &= (uint8_t)~1;
	set_w(&ctx->d[0], bus_r16(ctx->a[6]));       /* move.w (a6)+,d0 */
	cc_logic16(ctx, ctx->d[0]);
	ctx->a[6] += 2;
	ctx->a[5] = ctx->a[6];                       /* script pc */
	ctx->a[6] += (int16_t)ctx->d[0];             /* script end */
	hle_jump(ctx, 0x000D04);
}

/* $000D08: scripts done. A game restored from a password starts in its saved room
 * ($FF0A90, loaded by sub_002138); then fade in (sub_00219A at $000D30). */
HLE_RANGE(0x000D08, game_start_room, 60, 0x000D2C, 0x000D30)
{
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 7);
	RAM8(VBLANK_FLAGS) &= (uint8_t)~VBF_SCRIPT;
	cc_btst(ctx, RAM8(BOOT_FLAGS), 2);
	if (!(RAM8(BOOT_FLAGS) & 4)) {
		hle_jump(ctx, 0x000D30);
		return;
	}
	RAM16(0xFF069E) = 0;
	cc_logic16(ctx, 0);
	RAM16(0xFF06AC) = RAM16(0xFF0A90);           /* ram_room_index = saved room */
	cc_logic16(ctx, RAM16(0xFF06AC));
	hle_jump(ctx, 0x000D2C);
}
