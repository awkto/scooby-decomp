/* Sandbox for running one function in isolation and comparing machine state
 * (used by port/tools/difftest). */
#ifndef SCOOBY_SANDBOX_H
#define SCOOBY_SANDBOX_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
	uint8_t width;      /* 1, 2, 4 */
	uint32_t addr;
	uint32_t value;
} sb_event;

typedef struct sb_snap {
	uint32_t d[8], a[8], other_sp;
	uint16_t sr;
	uint32_t pc;
	uint8_t ram[0x10000];      /* natural 68k byte order */
	uint8_t vram[0x10000];     /* GPGX layout (compared raw) */
	uint8_t cram[0x80], vsram[0x80], vreg[0x20];
	int vdp_latch[6];          /* addr, code, pending, addr_latch, dmafill, cached_write */
	uint32_t dma_length;
	uint8_t zram[0x2000];
	uint8_t zstate;
	uint32_t zbank;
	uint8_t io[0x10];
	sb_event *ev;              /* port writes in order (owned) */
	int nev;
} sb_snap;

typedef struct {
	uint32_t ignore_regs;      /* bit i: D(i) for i<8, A(i-8) for i>=8 */
	uint16_t ignore_sr;        /* SR bits to ignore (e.g. 0x1F = all flags) */
	bool strict_stack;         /* also compare RAM below the entry SP */
	bool quiet;
	int max_report;            /* max mismatching items printed per category */
} sb_cmp_opts;

/* Enter sandbox mode on the current (just loaded) machine state: interrupts are held off,
 * the Z80 is frozen, and VDP DMA completes instantly after every write. */
void sb_begin(void);
void sb_end(void);

/* Run the 68k from the current PC (a function entry: return address at (SP)) until it
 * returns. Returns false if it did not return within max_insns or took an address error.
 * *cycles receives the 68k cycles used, *min_sp the lowest SP seen. */
bool sb_run_68k(unsigned long max_insns, unsigned long *insns, unsigned long *cycles, uint32_t *min_sp);
/* Same for a code range (HLE_RANGE): run until the PC is one of the 0-terminated exits
 * with SP == the entry SP, after at least one instruction. exits == NULL: sb_run_68k. */
bool sb_run_68k_until(const uint32_t *exits, unsigned long max_insns, unsigned long *insns, unsigned long *cycles,
                      uint32_t *min_sp);

/* Continue from the current state (no instruction needed) until the return condition of
 * a function entered with SP = sp0 and return address ret (exits == NULL), or until an
 * exit of a range with SP = sp0. Used after a C function that tail-jumps into 68k code. */
bool sb_run_68k_to(uint32_t sp0, uint32_t ret, const uint32_t *exits, unsigned long max_insns);

/* start / stop recording port writes into snap->ev */
void sb_record_begin(void);
void sb_snapshot(sb_snap *s);    /* also takes ownership of the recorded event list */
void sb_snap_free(sb_snap *s);

/* Compare a (reference) with b. Returns number of mismatching categories. The RAM range
 * [dead_lo, dead_hi) (scratch stack below entry SP) is skipped unless strict_stack. */
int sb_compare(const sb_snap *a, const sb_snap *b, uint32_t dead_lo, uint32_t dead_hi,
               const sb_cmp_opts *o, FILE *out);

#endif
