/* The native runner (GitLab #24): the game's code runs as C blocks dispatched by 68k address,
 * with no 68000 instruction emulation. docs/native.md.
 *
 * What is left of "the CPU" is plain C state: the register file and SR (in the layout GPGX's
 * m68k struct has, so the savestate code and the HLE layer keep working), the cycle clock the
 * rest of the machine (VDP, Z80, YM2612) is scheduled against, and the interrupt lines. The
 * machine still calls m68k_run(cycles) once per raster slice; here that is
 *
 *     take a pending interrupt (vector -> C trampoline), then
 *     while the slice has time:  run the C block registered at PC (hle.c dispatch);
 *                                every C block charges the 68k cycles the original costs
 *
 * A PC with no C block is fatal: nothing is interpreted, and nothing falls back silently.
 * The wait primitives are C blocks that idle to the end of the slice (hle_idle), so the
 * VBlank interrupt arrives at the same raster line as it would in the original's spin loop.
 *
 * Two builds use this file:
 *  - scoobyport --native (the 68k core is linked, never stepped): gpgx-native.patch routes the
 *    core's m68k_run / m68k_step_one / m68k_set_irq_delay / SR access here.
 *  - scooby (SCOOBY_NO_68K): no 68k core is compiled at all; native_core.c defines the m68k
 *    state and the core's API on top of the functions below.
 */
#include "native.h"
#include "shared.h"

/* GPGX's headers remap stdio to libretro's VFS: messages go through hle.c */
void native_fatal(const char *what, unsigned pc, unsigned sr, unsigned sp, unsigned ret);

/* from the HLE layer (hle.c) */
extern int (*m68k_hle_handler)(unsigned int pc);
int hle_nest_depth(void);

int native_active;               /* int: also declared by gpgx-native.patch */
unsigned long native_blocks;      /* C blocks run by the native runner (statistics) */
static bool g_check_irq;           /* SR or the interrupt lines changed during a block */

/* ------------------------------------------------------------------ bus (the CPU's view) */
/* Same semantics as the 68k core's m68ki_read/write: the memory map of GPGX (ROM, RAM, I/O,
 * VDP and Z80 handlers) and the bus-event hook. */
static inline cpu_memory_map *map_of(unsigned a) { return &m68k.memory_map[(a >> 16) & 0xFF]; }

unsigned native_read8(unsigned a)
{
	a &= 0xFFFFFF;
	cpu_memory_map *m = map_of(a);
	unsigned v = m->read8 ? m->read8(a) : READ_BYTE(m->base, a & 0xFFFF);
#ifdef HOOK_CPU
	if (cpu_hook)
		cpu_hook(HOOK_M68K_R, 1, a, v);
#endif
	return v;
}

unsigned native_read16(unsigned a)
{
	a &= 0xFFFFFF;
	cpu_memory_map *m = map_of(a);
	unsigned v = m->read16 ? m->read16(a) : *(uint16 *)(m->base + (a & 0xFFFF));
#ifdef HOOK_CPU
	if (cpu_hook)
		cpu_hook(HOOK_M68K_R, 2, a, v);
#endif
	return v;
}

unsigned native_read32(unsigned a)
{
	a &= 0xFFFFFF;
	cpu_memory_map *m = map_of(a);
	unsigned v = (m->read16 ? m->read16(a) : *(uint16 *)(m->base + (a & 0xFFFF))) << 16;
	unsigned b = (a + 2) & 0xFFFFFF;
	m = map_of(b);
	v |= m->read16 ? m->read16(b) : *(uint16 *)(m->base + (b & 0xFFFF));
#ifdef HOOK_CPU
	if (cpu_hook)
		cpu_hook(HOOK_M68K_R, 4, a, v);
#endif
	return v;
}

void native_write8(unsigned a, unsigned v)
{
	a &= 0xFFFFFF;
#ifdef HOOK_CPU
	if (cpu_hook)
		cpu_hook(HOOK_M68K_W, 1, a, v & 0xFF);
#endif
	cpu_memory_map *m = map_of(a);
	if (m->write8)
		m->write8(a, v & 0xFF);
	else
		WRITE_BYTE(m->base, a & 0xFFFF, v);
}

void native_write16(unsigned a, unsigned v)
{
	a &= 0xFFFFFF;
#ifdef HOOK_CPU
	if (cpu_hook)
		cpu_hook(HOOK_M68K_W, 2, a, v & 0xFFFF);
#endif
	cpu_memory_map *m = map_of(a);
	if (m->write16)
		m->write16(a, v & 0xFFFF);
	else
		*(uint16 *)(m->base + (a & 0xFFFF)) = (uint16)v;
}

void native_write32(unsigned a, unsigned v)
{
	a &= 0xFFFFFF;
#ifdef HOOK_CPU
	if (cpu_hook)
		cpu_hook(HOOK_M68K_W, 4, a, v);
#endif
	/* as the 68000: high word first */
	cpu_memory_map *m = map_of(a);
	if (m->write16)
		m->write16(a, v >> 16);
	else
		*(uint16 *)(m->base + (a & 0xFFFF)) = (uint16)(v >> 16);
	unsigned b = (a + 2) & 0xFFFFFF;
	m = map_of(b);
	if (m->write16)
		m->write16(b, v & 0xFFFF);
	else
		*(uint16 *)(m->base + (b & 0xFFFF)) = (uint16)v;
}

/* ------------------------------------------------------------------ status register */
/* GPGX keeps the flags unpacked (Musashi layout): x/c in bit 8, n/v in bit 7, z inverted. */
unsigned native_get_sr(void)
{
	return m68k.t1_flag | (m68k.s_flag << 11) | m68k.int_mask |
	       ((m68k.x_flag & 0x100) >> 4) | ((m68k.n_flag & 0x80) >> 4) |
	       ((!m68k.not_z_flag) << 2) | ((m68k.v_flag & 0x80) >> 6) | ((m68k.c_flag & 0x100) >> 8);
}

void native_set_ccr(unsigned v)
{
	m68k.x_flag = ((v >> 4) & 1) << 8;
	m68k.n_flag = ((v >> 3) & 1) << 7;
	m68k.not_z_flag = !((v >> 2) & 1);
	m68k.v_flag = ((v >> 1) & 1) << 7;
	m68k.c_flag = (v & 1) << 8;
}

static void set_s_flag(unsigned s)      /* s = 4 (supervisor) or 0; swaps the stack pointers */
{
	m68k.sp[m68k.s_flag] = m68k.dar[15];
	m68k.s_flag = s;
	m68k.dar[15] = m68k.sp[m68k.s_flag];
}

/* the interrupt is taken after the current block (the 68000 checks after the instruction) */
void native_set_sr(unsigned v)
{
	v &= 0xA71F;
	if (v & 0x8000)
		native_fatal("trace mode is not supported", m68k.pc, v, m68k.dar[15], 0);
	m68k.t1_flag = 0;
	m68k.int_mask = v & 0x0700;
	native_set_ccr(v);
	set_s_flag((v >> 11) & 4);
	g_check_irq = true;
}

/* ------------------------------------------------------------------ interrupts */
/* autovectored interrupt (the VDP's VBlank level 6 / HBlank level 4): the exception frame
 * (PC, SR) on the supervisor stack, mask = level, PC = vector; the vectors point at the
 * VBlank/HBlank trampolines, which are C blocks (mainloop) */
static void take_interrupt(unsigned level)
{
	m68k.stopped &= 6;           /* STOP ends; HALT/WAIT stay */
	if (m68k.stopped)
		return;
	unsigned sr = native_get_sr();
	m68k.t1_flag = 0;
	set_s_flag(4);
	m68k.int_mask = level << 8;
	vdp_68k_irq_ack(level);
	unsigned pc = native_read32((24 + level) << 2);
	if (!pc)
		pc = native_read32(15 << 2);
	m68k.dar[15] -= 4;
	native_write32(m68k.dar[15], m68k.pc);
	m68k.dar[15] -= 2;
	native_write16(m68k.dar[15], sr);
	m68k.pc = pc & 0xFFFFFF;
}

/* Musashi's interrupt latency table: 44 cycles + 6..15 wait states of the E clock */
static const int irq_cycles[10] = { 50, 59, 58, 57, 56, 55, 54, 53, 52, 51 };

static void check_irq(void)
{
	g_check_irq = false;
	if (hle_nest_depth() > 0)
		return;              /* never inside a C function's nested calls */
	if (m68k.int_level > m68k.int_mask) {
		int c0 = m68k.cycles;
		take_interrupt(m68k.int_level >> 8);
		m68k.cycles = c0 + irq_cycles[(c0 / 7) % 10] * 7;
	}
}

void native_set_irq_delay(unsigned level)
{
	m68k.int_level = level << 8;
	g_check_irq = true;
}

/* ------------------------------------------------------------------ running C blocks */
static void no_block(unsigned pc)
{
	native_fatal("no C block", pc, native_get_sr(), m68k.dar[15] & 0xFFFFFF,
	             native_read32(m68k.dar[15] & 0xFFFFFE) & 0xFFFFFF);
}

/* run the C block at PC once and charge its cycles; returns the 68k cycles charged */
static int run_block(void)
{
	unsigned pc = m68k.pc & 0xFFFFFF;
#ifdef HOOK_CPU
	/* the execution event the 68k core would give for the instruction at pc (frame profilers,
	 * state capture and the interpreted-instruction profile work unchanged) */
	if (cpu_hook)
		cpu_hook(HOOK_M68K_E, 0, pc, 0);
#endif
	int c = pc < 0x400000 ? m68k_hle_handler(pc) : -1;
	if (c < 0)
		no_block(pc);
	native_blocks++;
#ifdef M68K_OVERCLOCK_SHIFT
	m68k.cycles += (int)(((long long)c * 7 * m68k.cycle_ratio) >> M68K_OVERCLOCK_SHIFT);
#else
	m68k.cycles += c * 7;
#endif
	return c;
}

void native_run(unsigned int cycles)
{
	if (m68k.cycles >= (int)cycles)
		return;
	check_irq();
	if (m68k.stopped) {
		m68k.cycles = cycles;
		return;
	}
	m68k.cycle_end = cycles;
	while (m68k.cycles < (int)cycles) {
		run_block();
		if (g_check_irq)
			check_irq();
	}
	m68k.cycle_end = 0;
}

/* one C block, for a nested call made by C code (hle_call68k); no interrupts */
int native_step_one(void)
{
	run_block();
	return 1;
}

/* the cycles left in the current slice (an idling block charges them: hle_idle) */
int native_slice_left(void)
{
	if (!m68k.cycle_end || (int)m68k.cycle_end <= m68k.cycles)
		return 0;
	return ((int)m68k.cycle_end - m68k.cycles + 6) / 7;
}
