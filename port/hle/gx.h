/* Bridge to Genesis Plus GX internals. GPGX's headers (shared.h -> libretro osd.h) remap
 * stdio to libretro VFS wrappers, so only gx.c includes them; everything else uses this. */
#ifndef SCOOBY_GX_H
#define SCOOBY_GX_H
#include <stdint.h>

/* memories (GPGX layout: 68k RAM/ROM/VRAM keep 16-bit words host-endian) */
uint8_t *gx_work_ram(void);
uint8_t *gx_rom(void);
uint8_t *gx_vram(void);          /* 64 KiB */
uint8_t *gx_cram(void);          /* 0x80 bytes, packed 9-bit colours */
uint8_t *gx_vsram(void);         /* 0x80 bytes */
uint8_t *gx_vdp_reg(void);       /* 0x20 bytes */
uint8_t *gx_zram(void);          /* 8 KiB */
uint8_t *gx_io_reg(void);        /* 0x10 bytes */
uint16_t *gx_input_pad(void);    /* input.pad[], at least 2 entries */

/* 68k */
uint32_t *gx_m68k_dar(void);     /* D0-D7, A0-A7 (A7 = active SP) */
uint32_t gx_m68k_pc(void);
void gx_m68k_set_pc(uint32_t pc);
uint32_t gx_m68k_other_sp(void); /* inactive stack pointer */
void gx_m68k_set_usp(uint32_t v);      /* move An,usp */
int gx_m68k_cycles(void);        /* master clocks (7 per 68k cycle) */
void gx_vdp_fifo_clamp(void);    /* FIFO read-out stamps <= CPU clock (after a rewind) */
void gx_m68k_set_cycles(int c);  /* rewind the CPU clock (hle.c, after nested calls) */
void gx_m68k_clear_irq(void);
void gx_m68k_resync_irq(void);   /* int level = what the VDP asserts (after holding it off) */

/* VDP / Z80 */
uint32_t gx_dma_length(void);
void gx_vdp_latch(int out[6]);   /* addr, code, pending, addr_latch, dmafill, cached_write */
void gx_flush_dma(void);         /* complete a pending DMA right now (sandbox) */
uint8_t gx_zstate(void);
uint32_t gx_zbank(void);
void gx_z80_freeze(void);        /* make bus requests not run the Z80 (sandbox) */
void gx_vdp_untimed_begin(void); /* bracket a VDP data port write made by C code */
void gx_vdp_untimed_end(void);
void gx_vdp_follow_cpu(void);
unsigned gx_vdp_line_base(void);         /* mcycles_vdp */
void gx_vdp_set_line_base(unsigned v);    /* sandbox: advance the VDP line base to the CPU time */

#endif
