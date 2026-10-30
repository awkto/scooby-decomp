#include "gx.h"
#include "shared.h"

extern void vdp_hle_get_internal(int out[6]);   /* gpgx-hle.patch */

uint8_t *gx_work_ram(void) { return work_ram; }
uint8_t *gx_rom(void) { return cart.rom; }
uint8_t *gx_vram(void) { return vram; }
uint8_t *gx_cram(void) { return cram; }
uint8_t *gx_vsram(void) { return vsram; }
uint8_t *gx_vdp_reg(void) { return reg; }
uint8_t *gx_zram(void) { return zram; }
uint8_t *gx_io_reg(void) { return io_reg; }
uint16_t *gx_input_pad(void) { return input.pad; }

uint32_t *gx_m68k_dar(void) { return (uint32_t *)m68k.dar; }
uint32_t gx_m68k_pc(void) { return m68k.pc; }
void gx_m68k_set_pc(uint32_t pc) { m68k.pc = pc; }
/* Musashi: sp[0] = USP, sp[4] = SSP; the active one lives in dar[15] */
uint32_t gx_m68k_other_sp(void) { return m68k.sp[m68k.s_flag ? 0 : 4]; }
int gx_m68k_cycles(void) { return m68k.cycles; }
void gx_m68k_clear_irq(void) { m68k.int_level = 0; }

uint32_t gx_dma_length(void) { return dma_length; }
void gx_vdp_latch(int out[6]) { vdp_hle_get_internal(out); }

void gx_flush_dma(void)
{
	int guard = 0;
	while (dma_length && guard++ < 100000) {
		uint16 st = status;
		status &= ~8;                /* pretend active display: per-line chunks */
		mcycles_vdp = m68k.cycles;
		vdp_dma_update(m68k.cycles);
		status = (status & ~8) | (st & 8);
	}
}

uint8_t gx_zstate(void) { return zstate; }
uint32_t gx_zbank(void) { return zbank; }
void gx_z80_freeze(void) { Z80.cycles = 0xF0000000u; }

/* VDP data port writes made by C code are "untimed": the 68k FIFO wait model in
 * vdp_68k_data_w only works while the CPU stays within ~2 lines of mcycles_vdp, which a
 * C loop of hundreds of writes inside one m68k_run slice violates (it then reads past
 * fifo_timing[] and stalls the CPU for seconds). Marking the write as happening in
 * blanking (status bit 3) skips only that FIFO accounting; the C function's cost is
 * charged through its cycle count instead. Not used while a DMA fill is pending (the
 * data write starts the fill, whose rate depends on bit 3). */
static int g_untimed, g_saved_vb;

void gx_vdp_untimed_begin(void)
{
	int l[6];
	vdp_hle_get_internal(l);
	g_untimed = !l[4];
	if (g_untimed) {
		g_saved_vb = status & 8;
		status |= 8;
	}
}

void gx_vdp_untimed_end(void)
{
	if (g_untimed)
		status = (status & ~8) | g_saved_vb;
	g_untimed = 0;
}

/* sandbox: let the VDP line base follow the CPU (the frame loop normally does this) */
void gx_vdp_follow_cpu(void)
{
	while (m68k.cycles >= (int)(mcycles_vdp + MCYCLES_PER_LINE))
		mcycles_vdp += MCYCLES_PER_LINE;
}
