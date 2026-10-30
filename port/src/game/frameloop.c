/* Frame-loop model: the raster/beam-dependent VBlank and HBlank code as C keyed to the frame
 * loop instead of the beam (GitLab #25, groundwork for the CPU-less build #24).
 * docs/subsystems/render.md ("Frame pacing"), docs/remaining68k.md section 5.
 *
 * These replacements form the opt-in HLE group "native": --renderer native (or
 * SCOOBY_RENDERER=native) activates it, and then --hle all includes them.
 *
 *   $01006C hblank_verb_bar  the HBlank hook of gameplay. The original busy-waits on the VDP
 *            status and the HV counter until line 167 and then switches planes A/B, the
 *            v-scroll, the h-scroll and the window register for the bottom 56 lines (the
 *            verb/inventory bar). The C version only records that band for the renderer
 *            (render_band: from line 168, planes at $A000, scroll from ram_hscroll_verb, window
 *            from ram_verb_bar_window). It touches no VDP port, so GPGX's own picture shows the
 *            world planes there; the native renderer draws the band. No beam, no busy-wait.
 *   $005C8E hud_dma          VBlank: upload the inventory/HUD on request (VBF_HUD_DMA) if the
 *            raster is in lines $E1-$F6. The raster test reads the frame-loop model.
 *   $00AF24 actor_dma        (sprite.c) one actor tile DMA per frame if the raster is in
 *            $E000-$F000; same model.
 */
#include "game.h"
#include "cc68k.h"
#include "frameloop.h"
#include "emu.h"
#include "gx.h"
#include "render/render_emu.h"

#include <stdio.h>
#include <stdlib.h>

#define RAM_HSCROLL_VERB      0xFF0800   /* w: h-scroll of the verb-bar planes */
#define RAM_HBLANK_HV         0xFF08B2   /* w: HV counter when the split was made (never read) */
#define RAM_VERB_BAR_WINDOW   0xFF0AD1   /* b: register $12 value of the bottom band */

#define VERB_BAR_FIRST_LINE   168        /* the 68k switches at line 167, H >= $50: GPGX draws
                                          * line 168 with the new registers (rendertest) */
#define VERB_BAR_HV           0xA750u    /* what the original stores in RAM_HBLANK_HV: V $A7, H $50 */

/* ------------------------------------------------------------------ HV model */
static bool g_hv_live;

void frameloop_set_hv_live(bool on) { g_hv_live = on; }

uint16_t frameloop_hv(void)
{
	return g_hv_live ? bus_r16(0xC00008) : FRAMELOOP_VBLANK_HV;
}

/* ------------------------------------------------------------------ the verb-bar band */
void verb_bar_band(render_band *b)
{
	b->line = VERB_BAR_FIRST_LINE;
	b->reg_plane_a = 0x28;                      /* $8228: plane A name table at $A000 */
	b->reg_plane_b = 0x05;                      /* $8405: plane B name table at $A000 */
	b->reg_window_v = RAM8(RAM_VERB_BAR_WINDOW);   /* $9200 | ram_verb_bar_window */
	b->hscroll_a = b->hscroll_b = RAM16(RAM_HSCROLL_VERB);   /* $DC00 / $DC02 */
	b->vscroll_a = b->vscroll_b = 0;            /* VSRAM 0 / 1 */
}

/* $01006C hblank_verb_bar (HBlank hook, gameplay and the mystery intro): record the bottom
 * band of this frame for the renderer. The original's register effects are invisible to its
 * callers (the HBlank vector saves d0-a6 and rte restores SR); its only RAM effect is the HV
 * value in RAM_HBLANK_HV, which nothing reads. Charge: the original spends the time from the
 * HBlank interrupt (line 163) to line 167 H $50 spinning: 2743 cycles on average (Lobby,
 * SCOOBY_RENDER_SPLIT=model measures it), about 5.6 lines. */
HLE_FUNC_GROUP(0x01006C, hblank_verb_bar, 2740, "native")
{
	render_band b;
	verb_bar_band(&b);
	render_emu_set_band(&b);
	RAM16(RAM_HBLANK_HV) = VERB_BAR_HV;
}

/* $005C8E hud_dma (VBlank, from vblank_main): if VBF_HUD_DMA is requested and the raster is in
 * lines $E1..$F6, clear the request and fall through into inventory_upload ($005CB6).
 *     btst #5,(ram_vblank_flags) / bne / rts
 *     move.w (vdp_hv),d0 / lsr.w #8,d0 / cmpi.w #$E0,d0 / ble rts / cmpi.w #$F6,d0 / bgt rts
 *     bclr #5,(ram_vblank_flags)            ... inventory_upload
 * The HV value comes from the frame-loop model (frameloop_hv). D0 = the V counter and the flags
 * of the compare on the no-upload path; X = bit 7 of the HV word (lsr). */
HLE_FUNC_GROUP(0x005C8E, hud_dma, 30, "native")
{
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 5);
	if (!(RAM8(VBLANK_FLAGS) & VBF_HUD_DMA))
		return;
	uint16_t hv = frameloop_hv();
	set_w(&ctx->d[0], lsr16(ctx, hv, 8));
	uint16_t v = (uint16_t)ctx->d[0];
	if ((int16_t)cmpw(ctx, v, 0xE0) <= 0)
		return;
	if ((int16_t)cmpw(ctx, v, 0xF6) > 0)
		return;
	cc_btst(ctx, RAM8(VBLANK_FLAGS), 5);
	RAM8(VBLANK_FLAGS) &= (uint8_t)~VBF_HUD_DMA;
	ctx->cycles = 60;
	hle_call68k(ctx, 0x005CB6);                 /* falls through into inventory_upload */
}

/* ------------------------------------------------------------------ split model check */
/* SCOOBY_RENDER_SPLIT=model, with the 68k hblank_verb_bar running: when it starts, record the
 * band from the model; the native renderer then draws the bottom band from the model while
 * GPGX draws it from the real mid-frame register writes, and rendertest compares the two.
 * Also measures the original's cost (entry to rts) for the charge above. */
static long g_split_n;
static double g_split_cycles;
static int g_split_t0 = -1;

static void split_hook(int type, int width, unsigned addr, unsigned value)
{
	if (type != EMU_HOOK_EXEC)
		return;
	if (addr == 0x01006C && g_split_t0 < 0) {
		render_band b;
		verb_bar_band(&b);
		render_emu_set_band(&b);
		g_split_t0 = gx_m68k_cycles();
	} else if (addr == 0x010116 && g_split_t0 >= 0) {         /* its rts */
		int d = gx_m68k_cycles() - g_split_t0;
		if (d > 0) {
			g_split_cycles += d / 7.0;
			g_split_n++;
		}
		g_split_t0 = -1;
	}
}

static void split_report(void)
{
	if (g_split_n)
		fprintf(stderr, "split model: hblank_verb_bar ran %ld times, average %.0f 68k cycles\n",
		        g_split_n, g_split_cycles / g_split_n);
}

void frameloop_split_model_check(bool on)
{
	static bool reg;
	if (on) {
		emu_add_hook(split_hook);
		if (!reg)
			atexit(split_report);
		reg = true;
	} else {
		emu_remove_hook(split_hook);
	}
}
