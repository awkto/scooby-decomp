/* HLE (high-level emulation) layer: replace a 68k function with C.
 *
 *   #include "hle.h"
 *
 *   // $009D34: read both pads (decompiled from src/scooby.s loc_009D34)
 *   HLE_FUNC(0x009D34, pad_read, 120)
 *   {
 *       RAM16(0xFF09E2) = RAM16(0xFF09E0);
 *       ...
 *   }
 *
 * When the CPU is about to execute the registered address, the C function runs instead,
 * with the 68k registers in `ctx`; afterwards the layer performs the `rts` (pops the
 * return address) unless the function called hle_jump(). The third argument is the 68k
 * cycle cost charged to the CPU (difftest prints the measured cost of the original).
 *
 * Rules for replacement functions (checked by port/tools/difftest):
 *   - final D0-D7/A0-A7, SR (incl. X N Z V C), work RAM, VRAM/CRAM/VSRAM, VDP registers and
 *     latches, Z80 RAM, I/O registers, and the ordered list of writes to VDP ports / Z80 area /
 *     I/O area must equal what the original 68k code produces. Timing need not match.
 *   - all memory access goes through the accessors below (RAM8/16/32, ROM*, bus_*, vdp_*);
 *     never cache pointers into emulator memory.
 *   - RAM below the entry SP that the original used as scratch stack is not compared.
 */
#ifndef SCOOBY_HLE_H
#define SCOOBY_HLE_H
#include <stdbool.h>
#include <stdint.h>

typedef struct hle_ctx {
	uint32_t d[8];      /* D0-D7 */
	uint32_t a[8];      /* A0-A7 (a[7] = active stack pointer) */
	uint16_t sr;        /* status register: T.S..III...XNZVC */
	uint32_t pc;        /* entry address; becomes the resume address after hle_jump() */
	int jumped;         /* set by hle_jump(): do not perform the implicit rts */
	int cycles;         /* 68k cycles to charge; preset to the HLE_FUNC value, may be changed */
	const struct hle_def *def;
} hle_ctx;

typedef void (*hle_fn)(hle_ctx *ctx);

typedef struct hle_def {
	uint32_t addr;      /* 68k entry address */
	const char *name;
	hle_fn fn;
	int cycles;         /* 68k cycles charged per call (approximate original cost) */
	const char *file;
	/* HLE_RANGE only: 0-terminated list of exits where the replaced code range can leave
	 * (the C function must hle_jump() to one of them). Each entry is an address, optionally
	 * with a stack delta (HLE_EXIT_SP): the SP at that exit must be the entry SP + delta.
	 * NULL for an ordinary function (exit = rts). */
	const uint32_t *exits;
	/* opt-in group (HLE_FUNC_GROUP), NULL for ordinary replacements: such a function is only
	 * enabled by "all" while its group is active (hle_set_group). Used for C versions that need
	 * a host feature, e.g. "native" = the native renderer draws the verb-bar split (#25). */
	const char *group;
} hle_def;

/* Range exit with a declared stack delta (docs/porting.md, "Ranges that push or pop"):
 * the range leaves at ADDR with SP = entry SP + DELTA. DELTA is even, -256..+254: negative
 * when the range pushed (move.w d7,-(sp) before a blocking call), positive when it popped
 * what an earlier range pushed (move.w (sp)+,d7 / rts). A plain address = delta 0.
 * The delta lives in the top byte of the 32-bit entry (68k addresses are 24-bit). */
#define HLE_EXIT_SP(ADDR, DELTA) \
	((uint32_t)(ADDR) | ((uint32_t)(uint8_t)(int8_t)((DELTA) / 2) << 24))
static inline uint32_t hle_exit_addr(uint32_t x) { return x & 0xFFFFFF; }
static inline int hle_exit_sp_delta(uint32_t x) { return (int8_t)(x >> 24) * 2; }
/* does (pc, sp) match one of the 0-terminated exits of a range entered with SP = sp0? */
static inline bool hle_exit_match(const uint32_t *exits, uint32_t pc, uint32_t sp, uint32_t sp0)
{
	for (; *exits; exits++)
		if (hle_exit_addr(*exits) == (pc & 0xFFFFFF) &&
		    sp == sp0 + (uint32_t)(int32_t)hle_exit_sp_delta(*exits))
			return true;
	return false;
}

/* ------------------------------------------------------------------ registration */
void hle_register_def(const hle_def *def);
/* runtime registration (name may be NULL) */
void hle_register(uint32_t addr, hle_fn fn);
void hle_register_named(uint32_t addr, const char *name, hle_fn fn, int cycles);

#define HLE_FUNC(ADDR, NAME, CYCLES)                                                    \
	void NAME(hle_ctx *ctx);                                                            \
	static const hle_def hle_def_##NAME = { (ADDR), #NAME, NAME, (CYCLES), __FILE__, 0, 0 }; \
	__attribute__((constructor)) static void hle_ctor_##NAME(void)                      \
	{ hle_register_def(&hle_def_##NAME); }                                              \
	void NAME(hle_ctx *ctx)

/* HLE_FUNC_GROUP(addr, name, cycles, "group"): an HLE_FUNC that belongs to an opt-in group
 * (see hle_def.group, hle_set_group). It starts disabled; "all" enables it only while the group
 * is active, "+name" always does. */
#define HLE_FUNC_GROUP(ADDR, NAME, CYCLES, GROUP)                                       \
	void NAME(hle_ctx *ctx);                                                            \
	static const hle_def hle_def_##NAME = { (ADDR), #NAME, NAME, (CYCLES), __FILE__, 0, \
	                                        (GROUP) };                                  \
	__attribute__((constructor)) static void hle_ctor_##NAME(void)                      \
	{ hle_register_def(&hle_def_##NAME); }                                              \
	void NAME(hle_ctx *ctx)

/* HLE_RANGE(addr, name, cycles, exit1, exit2, ...): replace a code RANGE that is not a
 * subroutine - a piece of straight-line code inside Reset, one pass of a main loop, the
 * part of a function between two blocking calls. The C function runs when the CPU reaches
 * addr and must end with hle_jump(ctx, exitN) with the stack pointer as it was on entry
 * (or entry + delta for an exit declared as HLE_EXIT_SP(addr, delta)).
 * difftest runs the original from addr until it first reaches one of the exits with the
 * matching SP (after at least one instruction), so a loop head may be its own exit.
 * Typical use: stop right BEFORE a jsr/bsr to a function that waits for VBlank, let the
 * original 68k instruction make that call, and register the code after it as the next
 * range (see docs/porting.md, "Waiting for VBlank"). */
#define HLE_RANGE(ADDR, NAME, CYCLES, ...)                                              \
	void NAME(hle_ctx *ctx);                                                            \
	static const uint32_t hle_exits_##NAME[] = { __VA_ARGS__, 0 };                      \
	static const hle_def hle_def_##NAME = { (ADDR), #NAME, NAME, (CYCLES), __FILE__,   \
	                                        hle_exits_##NAME, 0 };                      \
	__attribute__((constructor)) static void hle_ctor_##NAME(void)                      \
	{ hle_register_def(&hle_def_##NAME); }                                              \
	void NAME(hle_ctx *ctx)

/* Cycle accounting (docs/porting.md, "Long-running functions", #31): the 68k time a C
 * function costs = ctx->cycles (its own share) + the cycles its nested 68k calls ran
 * (the layer rewinds the clock after nested calls and adds them to the charge). Charge
 * the ORIGINAL's cost. If the function was entered with the VBlank interrupt unmasked
 * and costs more than ~1000 cycles, the layer spends it in slices at a trap address with
 * interrupts taken in between (as the original would have been interrupted); with
 * interrupts masked it is charged at once (the original loses those VBlanks too). */
/* 68k cycles run by nested calls so far in the current C function (outermost level) */
int hle_nested_cycles(void);
/* VDP DMA started from C (#13). A 68k->VDP DMA stalls the CPU for its whole length. The
 * frame loop advances a DMA line by line, so C that starts one must bracket the sequence:
 *   hle_dma_begin();  ...ctrl-port writes that start the DMA...;  hle_dma_complete();
 *   ...restore reg 1, release the Z80...;  hle_dma_end();
 * The transfer then completes at once (as for nested 68k code, #31) and the stall time is
 * part of the nested time the layer charges when the C function returns. In difftest
 * (sandbox) the calls are harmless. Never poll the DMA-busy flag from C. */
void hle_dma_begin(void);
void hle_dma_complete(void);
void hle_dma_end(void);
/* profiling support (port/src/ipprof.c): depth of nested 68k execution (0 = top level), the
 * innermost running C function (NULL at top level), and whether the CPU is about to hand pc
 * to an enabled C function instead of executing the instruction there */
int hle_nest_depth(void);
const hle_def *hle_current_def(void);
bool hle_pc_replaced(uint32_t pc);
/* forget layer state that is not in the savestate (pending cycle debt): emu reset/load */
void hle_state_reset(void);

/* ------------------------------------------------------------------ control */
void hle_install(void);                 /* hook the CPU (called by emu_init) */
/* spec: "all" | "none" | comma list of [+-]name-or-$hexaddr, applied left to right.
 * Returns false on an unknown name. */
bool hle_configure(const char *spec);
bool hle_set_enabled(const char *name_or_addr, bool on);
void hle_set_all(bool on);
/* activate / deactivate an opt-in group: its members follow the "all" state of the ordinary
 * replacements (on if any ordinary one is on), or go off. */
void hle_set_group(const char *group, bool on);
bool hle_group_active(const char *group);
const hle_def *hle_find(const char *name_or_addr);
int hle_count(void);
const hle_def *hle_get(int i);
bool hle_is_enabled(int i);
unsigned long hle_calls(int i);
unsigned long hle_calls_this_frame(void);
void hle_frame_begin(void);
void hle_list(void);                    /* print registry to stdout */

/* Run def->fn on the current CPU state exactly as the CPU hook would (load ctx, call,
 * rts, store ctx); def need not be registered. Used by difftest. Returns charged cycles. */
int hle_invoke(const hle_def *def);

/* ------------------------------------------------------------------ calling 68k code */
/* jsr to 68k code at addr with ctx's registers; returns when it rts's. Other enabled
 * HLE functions still apply inside. Use for callees that are not decompiled yet. */
void hle_call68k(hle_ctx *ctx, uint32_t addr);
/* same, but the HLE override at addr itself is bypassed (run the original). */
void hle_call68k_orig(hle_ctx *ctx, uint32_t addr);
/* Run the ORIGINAL 68k code from ctx->pc (the HLE override at ctx->pc itself is bypassed,
 * others apply) until it reaches one of the 0-terminated exits with SP == ctx->a[7]; load
 * the final registers into ctx and hle_jump to that exit. Used by difftest --passthrough
 * for HLE_RANGE functions. Interrupts don't fire while it runs. */
void hle_run68k_until(hle_ctx *ctx, const uint32_t *exits);
/* resume 68k execution at addr instead of returning (tail jump / bra to shared code) */
static inline void hle_jump(hle_ctx *ctx, uint32_t addr) { ctx->pc = addr; ctx->jumped = 1; }
/* move An,usp (the user stack pointer is not part of hle_ctx) */
void hle_set_usp(uint32_t usp);

/* ------------------------------------------------------------------ memory */
extern unsigned char work_ram[0x10000];   /* GPGX: 68k work RAM, 16-bit words host-endian */
extern uint8_t *hle_rom;                  /* GPGX cart.rom, same layout */

/* lvalue accessors for 68k work RAM ($FF0000-$FFFFFF, mirrored) */
#define RAM8(a)  (work_ram[((uint32_t)(a) & 0xFFFF) ^ 1])
#define RAM16(a) (*(uint16_t *)(work_ram + ((uint32_t)(a) & 0xFFFE)))
static inline uint32_t RAM32(uint32_t a) { return ((uint32_t)RAM16(a) << 16) | RAM16(a + 2); }
static inline void RAM32_W(uint32_t a, uint32_t v) { RAM16(a) = (uint16_t)(v >> 16); RAM16(a + 2) = (uint16_t)v; }

/* ROM ($000000-$3FFFFF), read-only */
static inline uint8_t ROM8(uint32_t a) { return hle_rom[(a & 0x3FFFFF) ^ 1]; }
static inline uint16_t ROM16(uint32_t a) { return *(const uint16_t *)(hle_rom + (a & 0x3FFFFE)); }
static inline uint32_t ROM32(uint32_t a) { return ((uint32_t)ROM16(a) << 16) | ROM16(a + 2); }

/* Generic 68k bus access (any address: I/O, VDP, Z80 area...) with the CPU's exact
 * semantics, including side effects of hardware registers. Word/long must be even. */
uint8_t bus_r8(uint32_t a);
uint16_t bus_r16(uint32_t a);
uint32_t bus_r32(uint32_t a);
void bus_w8(uint32_t a, uint8_t v);
void bus_w16(uint32_t a, uint16_t v);
void bus_w32(uint32_t a, uint32_t v);

/* VDP ports */
#define VDP_DATA 0xC00000
#define VDP_CTRL 0xC00004
static inline void vdp_ctrl_w(uint16_t v) { bus_w16(VDP_CTRL, v); }
static inline void vdp_ctrl_w32(uint32_t v) { bus_w32(VDP_CTRL, v); }
static inline void vdp_data_w(uint16_t v) { bus_w16(VDP_DATA, v); }
static inline uint16_t vdp_data_r(void) { return bus_r16(VDP_DATA); }
static inline uint16_t vdp_status_r(void) { return bus_r16(VDP_CTRL); }
static inline void vdp_reg_w(int r, uint8_t v) { vdp_ctrl_w((uint16_t)(0x8000 | (r << 8) | v)); }

/* stack helpers (operate on ctx->a[7] and RAM, like -(sp) / (sp)+) */
static inline void push16(hle_ctx *c, uint16_t v) { c->a[7] -= 2; bus_w16(c->a[7], v); }
static inline void push32(hle_ctx *c, uint32_t v) { c->a[7] -= 4; bus_w32(c->a[7], v); }
static inline uint16_t pop16(hle_ctx *c) { uint16_t v = bus_r16(c->a[7]); c->a[7] += 2; return v; }
static inline uint32_t pop32(hle_ctx *c) { uint32_t v = bus_r32(c->a[7]); c->a[7] += 4; return v; }

/* ------------------------------------------------------------------ registers */
/* partial register writes, like .b/.w operations on Dn */
static inline void set_b(uint32_t *r, uint32_t v) { *r = (*r & 0xFFFFFF00u) | (v & 0xFF); }
static inline void set_w(uint32_t *r, uint32_t v) { *r = (*r & 0xFFFF0000u) | (v & 0xFFFF); }

/* ------------------------------------------------------------------ condition codes */
enum { CC_C = 1, CC_V = 2, CC_Z = 4, CC_N = 8, CC_X = 16 };
static inline void cc_set(hle_ctx *c, unsigned mask, unsigned bits) { c->sr = (uint16_t)((c->sr & ~mask) | (bits & mask)); }
/* MOVE/AND/OR/EOR/NOT/TST/CLR/MOVEQ/EXT/SWAP: N,Z from result, V=C=0, X unchanged */
static inline void cc_logic8(hle_ctx *c, uint32_t r) { cc_set(c, CC_N | CC_Z | CC_V | CC_C, ((r & 0x80) ? CC_N : 0) | ((r & 0xFF) ? 0 : CC_Z)); }
static inline void cc_logic16(hle_ctx *c, uint32_t r) { cc_set(c, CC_N | CC_Z | CC_V | CC_C, ((r & 0x8000) ? CC_N : 0) | ((r & 0xFFFF) ? 0 : CC_Z)); }
static inline void cc_logic32(hle_ctx *c, uint32_t r) { cc_set(c, CC_N | CC_Z | CC_V | CC_C, ((r & 0x80000000u) ? CC_N : 0) | (r ? 0 : CC_Z)); }
/* LSR.W #n,Dx (1<=n<=8) on value v (already masked to 16 bits): sets X=C=last bit out */
static inline uint32_t lsr16(hle_ctx *c, uint32_t v, int n)
{
	uint32_t r = (v & 0xFFFF) >> n;
	unsigned cbit = ((v >> (n - 1)) & 1) ? (CC_C | CC_X) : 0;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, cbit | (r ? 0 : CC_Z));
	return r;
}
/* LSL.B #n,Dx (1<=n<=8) */
static inline uint32_t lsl8(hle_ctx *c, uint32_t v, int n)
{
	uint32_t w = (v & 0xFF) << n;
	uint32_t r = w & 0xFF;
	unsigned cbit = ((w >> 8) & 1) ? (CC_C | CC_X) : 0;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, cbit | ((r & 0x80) ? CC_N : 0) | (r ? 0 : CC_Z));
	return r;
}

/* ------------------------------------------------------------------ sandbox (difftest) */
/* Called after every bus write made through the accessors and after every stepped
 * instruction when non-NULL (difftest uses it to complete DMA / mask IRQs). */
extern void (*hle_post_access)(void);

#endif
