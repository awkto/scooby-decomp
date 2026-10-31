# The native build: no 68000 emulation (GitLab #24)

`build/port/scooby` runs the game with **no 68k instruction emulation and no 68k core linked**. The game
logic is the C of `port/src/game/` (the HLE functions and ranges of the earlier waves plus the last pieces
of #24), run by address by a small dispatcher, the *native runner*. What is still emulated is the rest of the
machine, as allowed for this step: the VDP state (GPGX's VDP code; the picture comes from the native
renderer, docs/subsystems/render.md), the Z80 with the YM2612/PSG and the original sound driver, the pads and
the interrupt lines. Work RAM is still GPGX's 64 KiB array (16-bit words in host order, read through
`RAM8/RAM16/RAM32`).

Measured definition of done (this document's "Results" section and `docs/remaining68k.md`):
1. **P1** every 68k instruction that any gate route executes is C: `--hle all` on the emulated build runs 0
   interpreted instructions on every route (`port/tools/ipprof.sh`, `tools/py/remaining68k.py`).
2. **P2** the native runner on the emulated build (`SCOOBY_NATIVE=1`, core linked, never stepped): the core's
   instruction counter is 0 on every route; all gates pass; `storytest long` none == native.
3. **P3** the program without the core (`scooby`): `port/tools/nativetest.sh` finds no core symbol in it, its
   machine state equals P2's at every frame of every open-loop route, all gates pass on it, and `make dist`
   ships it (romscan clean).

## How it runs

```
GPGX frame loop (system_frame_gen, per raster line)            port/hle/native.c
  m68k_run(line_end) ──────────────────────────────────────►   native_run(cycles):
                                                                 take a pending interrupt (vector -> C block)
                                                                 while the slice has time:
                                                                   C block at PC  (hle.c dispatch -> run_def)
                                                                   PC = its exit / the popped return address
                                                                   clock += the cycles the block charges
                                                                 no C block at PC -> fatal ("no C block")
```

- **C blocks by address.** Every `HLE_FUNC` / `HLE_RANGE` is registered under its 68k address
  (`port/hle/hle.h`). A block leaves through `hle_jump` (a range's declared exit, a computed target) or the
  implicit `rts` (pop the return address from the stack in work RAM). The runner looks up the block at the new
  PC (a hash table in `hle.c`) and runs it. A 68k address with no block is a fatal error with the address and
  the stack: nothing is interpreted and nothing falls back silently. Paths no route has reached (and that
  therefore have no C yet) end there; `hle_call68k_orig` and `hle_run68k_until` (difftest's way to run the
  original) are fatal too.
- **The 68k register file and stack are plain C state**: GPGX's `m68k` struct (D0-D7/A0-A7, PC, the SR
  flags), which the HLE layer loads into and stores from `hle_ctx` around each block as before. The stack
  stays in work RAM, so C code that pushes and pops (`HLE_EXIT_SP`, `movem` in `glue.h`) behaves as on the
  68000, and a savestate is a GPGX savestate.
- **Time.** Each block charges the original's cycle cost (docs/porting.md "Long-running functions"); the
  runner adds it to the CPU clock that GPGX schedules the VDP, the Z80 and the YM2612 against. Nested calls
  from C (`hle_call68k`) run C blocks too and keep the layer's time accounting. The machine is the same
  line-based GPGX machine as the emulated build, with the CPU replaced: DMA, the Z80 bus, the FIFO and the
  interrupt lines are GPGX's.
- **Waits** (`HLE_WAIT_FUNC` / `HLE_WAIT_RANGE`, `port/src/game/waits.c`). The game waits for VBlank in
  spin loops on flags and countdowns the VBlank handler changes (`wait_vblank`, the menu pacing countdown
  `$FF08AE`, the inventory and verb-bar spins, the pad-release spins, game_init's DMA-busy spin). A wait block
  tests the condition like the original; if it still holds it calls `hle_idle()`, which charges the rest of
  the CPU's current time slice, and jumps to itself. The frame loop moves on, delivers the interrupt at the
  raster line where the original's spin would have taken it, and the block runs again. These blocks carry
  `HLE_F_WAIT`: difftest skips them (its sandbox holds interrupts).
- **Interrupts.** VBlank (level 6) and HBlank (level 4) are taken between blocks: the runner pushes the
  exception frame and jumps to the vector. The vectors `$8BC` / `$8AA` are C blocks (`waits.c`): save
  d0-a6, call the RAM hook (`$FF0000` / `$FF0004`, C: `vblank_main`, `vblank_light`, `vblank_title_fade`,
  `hblank_verb_bar`, `hblank_nop`) at top level with the return address of the tail block, which restores the
  registers and does the `rte`. A new SR that unmasks a pending interrupt takes it at once, as the 68k core
  does; nested calls never take interrupts.
- **The verb-bar split** is the native renderer's band (docs/subsystems/render.md): `hblank_verb_bar` in C
  records the band instead of waiting for the beam, so the native runner always selects the native renderer
  (`--renderer gpgx` is refused).
- **Determinism / HV counter.** No C reads the HV counter. `hud_dma` / `actor_dma` use the frame-loop model
  (`frameloop_hv()`, #25). `game_init` read it once at boot into `d0` before stepping the RNG; the step
  does not depend on `d0` and `game_reset` overwrites `d0` and the flags, so the C uses the constant 0 and
  the beam never reaches game state.

### Glue (port/src/game/glue.c, glue.h)

The C ranges of the earlier waves stop in front of every `jsr`/`bsr` of a routine that waits, and resume
after it; the call itself, the `bra` back to a loop head, the `rts`, and the pushes and pops around such
calls were left to the 68k. Each is now a block, written as what it is:

```c
GLUE_CALL(0x000D54, 6, 0x00974C)   /* jsr wait_vblank */   push $D5A, continue at $974C
GLUE_GOTO(0x000D68, 0x000D38)                                bra $D38
GLUE_RTS(0x002404)                                           rts
HLE_FUNC(0x0023F2, script_run_call_op, 16) { glue_call(ctx, ctx->a[0], 0x0023F4); }   /* jsr (a0) */
```

They are `HLE_RANGE`s with the callee as an exit at `SP - 4` (`HLE_EXIT_SP`), so difftest can run the original
instruction against them. The list is the measured residue (`tools/py/interp_sites.py` over the ipprof
profiles): one block per instruction address that a route executed interpreted and that starts a straight
piece. Larger pieces that had no C at all are ported as functions:

| file | addresses | what |
|---|---|---|
| `intro.c` | `$13BA`-`$197E` | the mystery intro card (`mystery_intro`, the van sprites, the parallax scroll) |
| `roomflow.c` | `$2138`, `$2196`, `$219A`, `$1BDA`, `$22D6`, `$47AC`, `$A248` | room entry and fade-in, verb labels, `rng_next`, `bar_save`, `game_init` |
| `roomstream.c` | `$FC3A`, `$FD5E`, `$FE74`, `$FF68` | map column/row streaming into the planes |
| `waits.c` | 30 spins, both vectors | the wait primitives and interrupt entry |
| `glue.c` | ~250 blocks | calls, branches, returns, pushes/pops between C blocks |

## The two builds

| | `scoobyport` (`--hle all`) | `scoobyport` + `SCOOBY_NATIVE=1` (P2) | `scooby` (P3) |
|---|---|---|---|
| 68k core linked | yes (Musashi, GPGX) | yes | **no** (`gpgx_core_native`: no `m68kcpu.c`/`s68kcpu.c`) |
| who calls the C blocks | the core's HLE hook, per instruction fetch | the native runner | the native runner |
| interpreted instructions | 0 on the gate routes (P1) | 0, counted by `m68k_insn_count` | none possible |
| difftest, capture, tools | yes | yes | (not built) |

- `tools/patches/gpgx-native.patch` routes the core's `m68k_run`, `m68k_step_one`, `m68k_set_irq_delay` and
  the HLE SR accessors to the runner while `native_active` is set, and counts the instructions the core
  executes (`native: N C blocks run, M instructions executed by a 68k core` at exit).
- `port/hle/native_core.c` (only in `scooby`) defines the `m68k` state and the CPU API the machine and the HLE
  layer call (`m68k_run`, `m68k_pulse_reset`, `m68k_set_irq`, `m68k_get_reg`, the bus accessors...) on top
  of the runner. The Sega CD sub-CPU entry points are fatal stubs (cartridge games only).
- The runner gives the execution event (`cpu_hook`, `EMU_HOOK_EXEC`) for every block, so frameprof, state
  capture and ipprof work on it unchanged.
- Savestates are GPGX's in both builds; a state saved by the native runner (PC at a block start) also loads in
  the emulated build. A state saved by `--hle none` can have the PC in the middle of a 68k function and does not
  load into the native runner: gates that need start states make them on the side that uses them
  (`MOUSETEST_STATE_HLE=all`).

## Running the gates on the native runner

```
port/tools/nativetest.sh                                       # scan + P2 == P3 every frame, 0 core instructions
STORYTEST_NATIVE=1 port/tools/storytest.sh [long]              # none vs the native runner (P2: scoobyport)
STORYTEST_NATIVE=1 SCOOBY_NATIVE_BIN=build/port/scooby port/tools/storytest.sh [long]     # P3
SCOOBY_PORT="port/tools/nativewrap.sh scoobyport" SCOOBY_FRAMEPROF="port/tools/nativewrap.sh frameprof" \
  MOUSETEST_STATE_HLE=all port/tools/{hosttest,irqtest,mousetest,wstest,hifpstest}.sh       # P3 (NATIVEWRAP=p2: P2)
SCOOBY_BIN=build/port/scooby port/tools/slottest.sh
```

`nativewrap.sh` runs a gate's `--hle none` invocations (its reference side) on the emulated build and every
other one on the native build. `rendertest` compares the native renderer with GPGX's own picture of the
68k's raster split; there is no raster split on the native runner, so it stays an emulated-build gate.

## Results

(filled in at the end of #24; see the commit log)

## What is left

- Code no route reaches has no C and stops the native build ("no C block"): the coverage gaps of
  docs/remaining68k.md section 4a, the `hle_call68k_orig` fallbacks of rare script modes, and parts of
  functions whose other paths are C. Each is a plain port once a route (or a poked state) reaches it.
- The machine around the runner is still GPGX's line-based scheduler (the VDP's DMA timing, the Z80 slices,
  the FIFO). A frame-synchronous machine (DMA at once, VBlank as the sync point, as crabsmack/xmen2-pc did)
  would remove the cycle charges' remaining influence on timing; it is not needed for "no 68k emulation".
- Work RAM keeps GPGX's layout (16-bit words in host order); a host-order arena with typed access is a
  separate refactor.
