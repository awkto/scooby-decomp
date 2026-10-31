/* The native runner (GitLab #24): C blocks dispatched by 68k address instead of a 68000
 * core. See native.c and docs/native.md. Only hle.c, gx.c and the native core use this. */
#ifndef SCOOBY_NATIVE_H
#define SCOOBY_NATIVE_H
#include <stdbool.h>

extern int native_active;               /* the native runner drives the game (no 68k stepping) */
extern unsigned long native_blocks;     /* C blocks it ran */

void native_run(unsigned int cycles);   /* m68k_run: C blocks until the slice is used up */
int native_step_one(void);              /* one C block (nested calls from C) */
void native_set_irq_delay(unsigned level);
int native_slice_left(void);            /* 68k cycles left in the current slice */

unsigned native_get_sr(void);
void native_set_ccr(unsigned v);
void native_set_sr(unsigned v);

unsigned native_read8(unsigned a);
unsigned native_read16(unsigned a);
unsigned native_read32(unsigned a);
void native_write8(unsigned a, unsigned v);
void native_write16(unsigned a, unsigned v);
void native_write32(unsigned a, unsigned v);

#endif
