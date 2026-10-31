/* The CPU interface of the native build `scooby` (GitLab #24, docs/native.md): GPGX's machine
 * (VDP, Z80, YM2612/PSG, I/O, memory map) calls a 68000 through this API; here it is the native
 * runner (native.c), and no 68k core is compiled or linked (core/m68k/m68kcpu.c and s68kcpu.c
 * are left out of the gpgx_core_native library). The scan in port/tools/nativescan.sh checks the
 * linked program for the core's symbols.
 *
 * State: GPGX's m68k struct (registers, unpacked SR flags, the cycle clock, the interrupt lines,
 * the memory map). The functions below are the ones the core exported (m68kcpu.c with
 * gpgx-hle.patch); they keep their names because the machine and the HLE layer call them.
 * The Sega CD sub-CPU (s68k) does not exist: its entry points abort (cartridge games only).
 */
#include "native.h"
#include "shared.h"

void native_fatal(const char *what, unsigned pc, unsigned sr, unsigned sp, unsigned ret);

m68ki_cpu_core m68k;

/* ------------------------------------------------------------------ the HLE layer's hooks */
unsigned char *m68k_hle_bitmap;               /* unused: the runner dispatches every address */
int (*m68k_hle_handler)(unsigned int pc);     /* hle.c dispatch() */
unsigned int m68k_hle_skip_pc = 0xFFFFFFFF;   /* hle_call68k_orig is fatal in the native build */

unsigned int m68k_bus_read8(unsigned int a) { return native_read8(a); }
unsigned int m68k_bus_read16(unsigned int a) { return native_read16(a); }
unsigned int m68k_bus_read32(unsigned int a) { return native_read32(a); }
void m68k_bus_write8(unsigned int a, unsigned int v) { native_write8(a, v); }
void m68k_bus_write16(unsigned int a, unsigned int v) { native_write16(a, v); }
void m68k_bus_write32(unsigned int a, unsigned int v) { native_write32(a, v); }
unsigned int m68k_hle_get_sr(void) { return native_get_sr(); }
void m68k_hle_set_ccr(unsigned int v) { native_set_ccr(v); }
void m68k_hle_set_sr(unsigned int v) { native_set_sr(v); }
int m68k_step_one(void) { return native_step_one(); }

/* ------------------------------------------------------------------ the machine's CPU API */
void m68k_init(void)
{
#ifdef M68K_OVERCLOCK_SHIFT
	m68k.cycle_ratio = 1 << M68K_OVERCLOCK_SHIFT;
#endif
}

/* reset: supervisor mode, interrupts masked, SP and PC from the vector table */
void m68k_pulse_reset(void)
{
	m68k.stopped = 0;
	m68k.t1_flag = 0;
	m68k.int_mask = 0x0700;
	m68k.int_level = 0;
	m68k.sp[m68k.s_flag] = m68k.dar[15];
	m68k.s_flag = 4;
	m68k.dar[15] = m68k.sp[4];
	m68k.dar[15] = native_read32(0);
	m68k.pc = native_read32(4) & 0xFFFFFF;
	m68k.cycles += 40 * 7;
	m68k.refresh_cycles = ((m68k.cycles / (128 * 7)) * (128 * 7)) + 128 * 7;
}

void m68k_pulse_halt(void) { m68k.stopped |= 2; }
void m68k_clear_halt(void) { m68k.stopped &= ~2u; }
void m68k_run(unsigned int cycles) { native_run(cycles); }
void m68k_update_irq(unsigned int mask) { m68k.int_level |= mask << 8; }
void m68k_set_irq(unsigned int level) { m68k.int_level = level << 8; }
void m68k_set_irq_delay(unsigned int level) { native_set_irq_delay(level); }
/* the current instruction's extra time for cycle-exact VDP status reads: C blocks have none */
int m68k_cycles(void) { return 0; }

unsigned int m68k_get_reg(m68k_register_t r)
{
	switch (r) {
	case M68K_REG_D0: case M68K_REG_D1: case M68K_REG_D2: case M68K_REG_D3:
	case M68K_REG_D4: case M68K_REG_D5: case M68K_REG_D6: case M68K_REG_D7:
		return m68k.dar[r - M68K_REG_D0];
	case M68K_REG_A0: case M68K_REG_A1: case M68K_REG_A2: case M68K_REG_A3:
	case M68K_REG_A4: case M68K_REG_A5: case M68K_REG_A6: case M68K_REG_A7:
		return m68k.dar[8 + r - M68K_REG_A0];
	case M68K_REG_PC: return m68k.pc;
	case M68K_REG_SR: return native_get_sr();
	case M68K_REG_SP: return m68k.dar[15];
	case M68K_REG_USP: return m68k.s_flag ? m68k.sp[0] : m68k.dar[15];
	case M68K_REG_ISP: return m68k.s_flag ? m68k.dar[15] : m68k.sp[4];
	case M68K_REG_IR: return m68k.ir;
	default: return 0;
	}
}

void m68k_set_reg(m68k_register_t r, unsigned int v)
{
	switch (r) {
	case M68K_REG_D0: case M68K_REG_D1: case M68K_REG_D2: case M68K_REG_D3:
	case M68K_REG_D4: case M68K_REG_D5: case M68K_REG_D6: case M68K_REG_D7:
		m68k.dar[r - M68K_REG_D0] = v;
		return;
	case M68K_REG_A0: case M68K_REG_A1: case M68K_REG_A2: case M68K_REG_A3:
	case M68K_REG_A4: case M68K_REG_A5: case M68K_REG_A6: case M68K_REG_A7:
		m68k.dar[8 + r - M68K_REG_A0] = v;
		return;
	case M68K_REG_PC: m68k.pc = v & 0xFFFFFF; return;
	case M68K_REG_SR: native_set_sr(v); return;
	case M68K_REG_SP: m68k.dar[15] = v; return;
	case M68K_REG_USP:
		if (m68k.s_flag)
			m68k.sp[0] = v;
		else
			m68k.dar[15] = v;
		return;
	case M68K_REG_ISP:
		if (m68k.s_flag)
			m68k.dar[15] = v;
		else
			m68k.sp[4] = v;
		return;
	case M68K_REG_IR: m68k.ir = v & 0xFFFF; return;
	default: return;
	}
}

/* ------------------------------------------------------------------ no Sega CD */
#define NO_SCD(name, ...) void name(__VA_ARGS__) { native_fatal("Sega CD sub-CPU: " #name, 0, 0, 0, 0); }
m68ki_cpu_core s68k;
NO_SCD(s68k_init, void)
NO_SCD(s68k_pulse_reset, void)
NO_SCD(s68k_pulse_halt, void)
NO_SCD(s68k_clear_halt, void)
NO_SCD(s68k_pulse_wait, unsigned int address, unsigned int write_access)
NO_SCD(s68k_clear_wait, void)
NO_SCD(s68k_run, unsigned int cycles)
NO_SCD(s68k_update_irq, unsigned int mask)
NO_SCD(s68k_set_reg, m68k_register_t r, unsigned int v)
int s68k_cycles(void) { native_fatal("Sega CD sub-CPU: s68k_cycles", 0, 0, 0, 0); return 0; }
unsigned int s68k_get_reg(m68k_register_t r) { (void)r; native_fatal("Sega CD sub-CPU: s68k_get_reg", 0, 0, 0, 0); return 0; }
