# Native port: host, HLE layer and difftest

The port runs the original ROM on a statically linked Genesis Plus GX core and replaces
68k functions with C one at a time. Each replacement is proven equivalent to the original
by `difftest` before it is enabled. When every function has been replaced, the CPU core
is no longer needed.

```
port/
  CMakeLists.txt        targets: scoobyport (SDL2 host), difftest (headless)
  cmake/gpgx.cmake      exports GPGX 60bee4c from ext/gpgx + applies tools/patches/*.patch
  third_party/gpgx/     GPGX license (sources are not copied into this repo)
  hle/hle.h             THE API for decompiled code: HLE_FUNC, ctx, RAM/ROM/bus/VDP accessors, flags
  hle/hle.c             registry, dispatch from the CPU, nested 68k calls, bus accessors
  hle/sandbox.[ch]      run one function in isolation, snapshot and compare machine state
  hle/gx.[ch]           the only code that touches GPGX internals
  src/emu.[ch]          core wrapper (libretro entry points called directly), ROM sha1 check
  src/script.[ch]       harness-compatible scripts (run/press/shot/ram/vdp/save/load/hle/capture/regs/peek, mouse/click/wheel)
  src/capture.[ch]      entry-state capture (savestate + pad state when PC hits ADDR)
  src/main.c            SDL2 host
  src/game/*.c          decompiled game code, grouped by subsystem (io.c, vdp.c, util.c,
                        mainloop.c + reset.c (#12), ...); cc68k.h = exact-flag helpers
  src/game/game.h       prototypes of decompiled functions + named RAM variables
  src/game/walkpath.*   click-to-walk: collision queries, A* pathfinder, auto-walk driver (docs/mouse.md)
  src/game/mousehook.*  game-side state for the mouse (pointer mode, hit test, verb bar geometry)
  src/mouse/            host-side mouse control (SDL-free; main.c feeds it events, scripts drive it)
  tools/difftest.c      differential tester
  tools/dt              one-command build + capture + difftest
  tools/frameprof.c     per-frame profile: hooks, VBlank/HBlank/main-loop counts, hot spots
  scripts/              lobby.txt (route to the Lobby), capture.txt (route used for captures)
```

## Building

All building and testing happens on a leased VM (see README), not on your workstation:

    vm=$(tools/vm/lease my-task)
    tools/vm/vmrun $vm 'cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release && cmake --build build/port -j4'
    tools/vm/release $vm

At configure time CMake runs `git archive 60bee4c` on the Genesis-Plus-GX checkout in
`ext/gpgx` (override it with `-DGPGX_DIR=...`). `-DGPGX_CLONE=ON` clones it once if it is
missing. CMake then applies `gpgx-cpuhook`, `gpgx-vdptrace` and `gpgx-hle.patch` into
`build/port/gpgx-src` and compiles that copy. ext/gpgx is never modified. The export is
redone whenever a patch file changes.

`tools/patches/gpgx-hle.patch` adds the following to the core:
- `m68k_hle_bitmap` and `m68k_hle_handler`: before fetching an instruction whose address
  bit is set, the handler runs and may replace the instruction.
- `m68k_step_one()`: executes exactly one instruction without polling interrupts. Nested
  calls are safe.
- `m68k_bus_read*/write*`: bus accesses with the CPU's exact semantics.
- `m68k_hle_get_sr/set_ccr/set_sr`.
- `vdp_hle_get_internal()`: VDP port latches, for comparing state.

## Running the host

    build/port/scoobyport                       # rom/baserom.md, sha1 checked (wrong ROM -> refused)
    build/port/scoobyport --hle none            # pure emulation, no C replacements
    build/port/scoobyport --hle all,-pad_read   # everything except one function
    build/port/scoobyport --list-hle

| Key | Action |
| --- | --- |
| arrows | D-pad |
| Z or A | Genesis A |
| X or S | Genesis B |
| C or D | Genesis C |
| Enter | Start |
| F1 | debug overlay (frame, fps, HLE calls/frame, Shaggy X/Y, pad byte, audio queue) |
| F2 | toggle all C replacements (A/B comparison) |
| F5 / F9 | save / load state (slot files in `~/.local/share/scoobyport`) |
| F6 / F7 | previous / next slot |
| P | pause |
| N or . | advance one frame |
| Tab (held) | fast forward |
| F11 or Alt+Enter | fullscreen |
| F12 | screenshot to `runs/port/` |
| Esc | quit |

Gamepads use the SDL GameController mapping:

| Pad button | Genesis button |
| --- | --- |
| D-pad, left stick | D-pad |
| X, Y | A |
| A | B |
| B, RB | C |
| Start | Start |
| Back | pause |

The mouse plays the whole game too (left click walks / uses, right click looks, middle
click is Start, wheel pages the inventory); `--no-mouse` turns it off. See
[docs/mouse.md](mouse.md); tests: `port/tools/mousetest.sh`.

The picture is always scaled by a whole number and letterboxed.

Audio paces the emulation: one frame is emulated each time the SDL audio queue drops
below about 50 ms. The frame rate is GPGX's NTSC rate (59.92 Hz). Without an audio device,
a timer paces it instead.

Headless (VM, CI):

    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy build/port/scoobyport \
        --script port/scripts/lobby.txt --outdir runs/port --screenshot runs/port/end.ppm
    # --window-shot f.ppm saves the scaled window (with --overlay: the overlay too)
    # --frames N exits after N frames (after the script)
    # --capture ADDR N saves entry states while you play (interactive capture)
    port/tools/hosttest.sh   # wrong-ROM refusal, Lobby == harness screenshot, savestate
                             # round trip, scaled window + overlay, real-time pacing + audio

## The C API (port/hle/hle.h)

```c
#include "game.h"            /* includes hle.h */

/* $00992C: copy D1.w words from (A0)+ to VRAM address D0.w ... */
HLE_FUNC(0x00992C, vram_write_words, 0)
{
	uint16_t addr = (uint16_t)ctx->d[0];
	...
	vdp_ctrl_w(...);  vdp_data_w(bus_r16(ctx->a[0]));  ctx->a[0] += 2;
}
```

- `HLE_FUNC(addr, name, cycles)` defines `void name(hle_ctx *ctx)` and registers it with
  a constructor. Because `scooby_common` is a CMake OBJECT library, a new `.c` file under
  `src/game/` is picked up with no other changes (re-run cmake so the glob sees it).
- The host calls the function when the CPU reaches `addr`. Afterwards the layer pops the
  return address, which is the original `rts`. A function that ends in a jump to other code
  calls `hle_jump(ctx, addr)` instead.
- `ctx->d[8]`, `ctx->a[8]` (a[7] = SP), `ctx->sr`. Partial writes: `set_b(&ctx->d[0], v)`,
  `set_w(...)`.
- Flags: `cc_logic8/16/32` (MOVE/AND/OR/TST: N Z, V=C=0, X kept), `lsr16`, `lsl8`,
  `cc_set(ctx, mask, bits)`. Add more helpers to hle.h as you need them.
- Memory:
  - `RAM8(a)` / `RAM16(a)` are lvalues; `RAM32(a)` / `RAM32_W(a, v)` for longs.
  - `ROM8/16/32(a)`.
  - `bus_r*/bus_w*`: any address, with the hardware side effects of I/O, VDP and the Z80
    area.
  - VDP: `vdp_ctrl_w`, `vdp_ctrl_w32`, `vdp_data_w`, `vdp_data_r`, `vdp_status_r`,
    `vdp_reg_w`.
  - All port I/O must go through `bus_*` or the `vdp_*` helpers so that difftest sees it.
- Cycles: the third `HLE_FUNC` argument is the cost charged to the CPU. A function can set
  `ctx->cycles` per call, for example proportional to a loop count. difftest prints the
  original's min/avg/max cycles: use the average.
- Callees that are not decompiled yet: `hle_call68k(ctx, addr)` runs the 68k code as a
  `jsr` and returns when it `rts`es. Inside it, other enabled C replacements still apply.
  `hle_call68k_orig` bypasses an override at `addr` itself.
- C-to-C calls: decompiled functions take `hle_ctx *` and keep the original register
  contract. Call them directly (`z80_bus_request(ctx);`) and declare them in `game.h`.
- Interrupts never fire inside a C function. Code that waits for VBlank must not run
  inside C, not even through `hle_call68k`. Port the code around the wait as
  `HLE_RANGE`s that stop in front of the blocking call: see "Waiting for VBlank" below.
- `HLE_RANGE(addr, name, cycles, exit, ...)` replaces a code range that is not a
  subroutine (Reset pieces, one main-loop pass). It must end with `hle_jump` to a
  declared exit.
- `hle_set_usp(v)` stands for `move An,usp`.

### Conventions

- One file per subsystem: `src/game/<subsystem>.c` (io, vdp, sound, room, script, actor,
  ...).
- Name functions descriptively, in snake_case. If the purpose is unknown, use
  `sub_XXXXXX`.
- Put the address and the original asm idiom in the comment above each function, and
  document every non-obvious flag or register side effect.
- Give RAM variables `#define`s in `game.h` (for example `PAD1_CUR 0xFF09E0`). Move them to
  structs when the layout is understood.
- Plain C behaviour first. Equivalence is about state, not timing.

## Decompiling one function, end to end

1. **Pick and read it.** Find the label in `src/scooby.s`. Note its callers (`grep
   'loc_XXXXXX' src/scooby.s`), its exits (all `rts`, any `jmp`/`bra` tails), the registers
   it saves and clobbers, and its final flags. The final flags come from the last
   flag-setting instruction before `rts`. Watch out for `move.w sr,-(sp)` …
   `move.w (sp)+,sr` pairs: they restore the flags as they were at the push.
2. **Write it** in `port/src/game/<subsystem>.c` with `HLE_FUNC(0xADDR, name, cycles)`.
   Mirror every register and flag effect the original leaves behind, even ones that look
   unused: difftest compares all of them.
3. **Test it** on a VM:

       tools/vm/vmrun $vm 'port/tools/dt name'

   `dt` builds difftest, then captures 200 entry states into `runs/states/<ADDR>/` if
   there are fewer. Capture plays `port/scripts/capture.txt`: boot, the Lobby route, then
   about 20,000 frames of seeded random play. It first counts all calls on that route and
   keeps every k-th call, so the states are spread out and are not 200 identical boot-time
   calls. dt then runs every state twice:
   - the original 68k code until it returns, with all other C replacements off;
   - your C function.

   It compares:
   - D0–D7/A0–A7, SR, the inactive SP and the return PC;
   - 64 KiB work RAM;
   - VRAM, CRAM, VSRAM, VDP registers, VDP address/code/pending latches, DMA length;
   - Z80 RAM, Z80 bus and bank state, I/O registers;
   - the ordered list of 68k writes to $A00000–$A1FFFF (Z80 area and I/O) and
     $C00000–$DFFFFF (VDP ports).

   RAM below the entry SP (scratch stack, 256 bytes or as deep as the original went) is
   ignored. On failure it prints the first failing state, its entry registers, and the
   first mismatching item of each category. For example:

       FAIL runs/states/009BE4/00000.cap (frame 36713, call #0): 1 category differ
         entry: D0=00000000 D1=74788000 ...
         SR: 68k=2700 C=2710 (XNZVC 68k=00000 C=10000)

   Iterate until you get `name $ADDR: 200/200 states equal`.
4. **If the route never calls the function**, or calls it fewer than 200 times, get more
   states in one of these ways:
   - write a script that reaches the code (harness syntax) and run
     `difftest capture ADDR 200 --script my.txt`;
   - start from a savestate: `load path/x.state` inside the script;
   - capture while playing: `scoobyport --hle none --capture ADDR 200`.

   Keep good scripts under `port/scripts/`. Never commit the states: they are ROM-derived
   and live in runs/.
5. **Play-test it.** Run `scoobyport --script port/scripts/lobby.txt --screenshot x.ppm`
   with `--hle all` and with `--hle none`, and check the game still reaches the same
   places. Small sprite or animation differences are expected: C functions don't take the
   same number of cycles. Check that `SCOOBY_HLE_TRACE=1` shows the function being called
   with sensible charged cycles.
6. **Commit** the C file, any new scripts, and new `hle.h` helpers.

difftest options:
- `--ignore-regs d0,a1` / `--ignore-flags`: only with a comment in the C source proving no
  caller reads them.
- `--strict-stack`: also compare the scratch stack.
- `--with-hle`: keep other C replacements active on both sides.
- `--poke "W:FF06AC=5,L:FF068E=0xFCA0C,D0=7"` (repeatable): run every state once per variant after patching
  work RAM (`B/W/L:HEXADDR=VALUE`) and registers (`D0..D7/A0..A7=VALUE`). Use it to reach inputs the capture
  route never produces (other rooms, ids, flag bits); see `port/tools/dt-room`.
- `--passthrough`: the C side calls the original through `hle_call68k_orig`. This
  self-tests the harness and must always pass.
- `--keep-going`, `-v`, `--max N`, `--states DIR`.

`difftest check all` tests every registered function.

### Sandbox semantics (what "equivalent" means)

These rules make the comparison independent of timing:
- Interrupts are held off.
- The Z80 is frozen, so bus requests don't run it.
- A VDP DMA completes at once after each write or instruction.
- The VDP's line base follows the CPU clock.

VDP data-port writes made from C are untimed: GPGX's FIFO wait model breaks when hundreds
of writes happen inside one CPU time slice. The function's cost is charged through its
cycle count instead (see `gx_vdp_untimed_begin` in hle/gx.c). Code that busy-waits on the
VDP status or the HV counter cannot be compared exactly. Keep those waits in 68k, or
restructure them.

## Waiting for VBlank: blocking code, HLE_RANGE and tail jumps

Read this before porting anything that waits. The main loop and frame structure are
mapped in `docs/subsystems/mainloop.md` (#12).

### The problem

The game has no frame-locked main loop. It waits for the VBlank interrupt in many places:
- `wait_vblank` `$974C`: set bit 0 of `ram_vblank_flags`, spin until VBlank clears it.
- Counters that VBlank decrements: `ram_frame_countdown` `$FF08AE`, `$FF0AB1`/`$FF0AB2`,
  the actor timers.
- Flags that VBlank-side code clears: bit 5 of `ram_vblank_flags`, `$FF0AB6`, `$FF0ABC`/`$FF0ABD`
  animation states, `$FF0ACB` (verb-bar slide).
- Raster waits on the VDP status / HV counter: `hblank_verb_bar`.

A C function never sees an interrupt. While it runs, the emulator's frame loop is
suspended, the VDP line counter does not advance, and `hle_call68k` steps the CPU without
polling interrupts. So:
- a C function that reaches a waiting function through `hle_call68k` **hangs the game**
  (`hle_call68k` aborts after 200 M instructions);
- difftest of a function that waits never finishes on the original side ("original did
  not return"), because the sandbox holds interrupts.

`tools/py/blocking.py` finds the spin loops. It also finds every function that reaches one
through the call graph, including script opcodes called through the opcode tables:

    tools/py/blocking.py sub_00B0B8 palette_fade vdp_dma_vram
    sub_00B0B8               BLOCKS: spins at loc_00B5A4
    palette_fade             BLOCKS: calls wait_vblank
    vdp_dma_vram             ok
    tools/py/blocking.py --all            # all 91, with the reason

DMA-busy polls (VDP status bit 1) don't count, except after a VRAM fill/copy (see #30
below): the sandbox finishes DMA at once and GPGX stalls the CPU during a 68k→VDP DMA. It's a heuristic. If a difftest original "did not return",
look for a wait it missed.

### The rule: stop in front of the blocking call

1. Keep the wait primitives in 68k: `wait_vblank`, `wait_vblanks`, the spin loops,
   `hblank_verb_bar`. In the native port each becomes "yield until the host has run the
   next VBlank". The host loop then is: run VBlank hook, run game code until it yields.
2. Never call a blocking function with `hle_call68k`. Split the caller at the call
   instead:
   - Port the code **between** blocking calls as `HLE_RANGE(start, name, cycles, exits...)`.
   - A range ends with `hle_jump(ctx, exit)`, where the exit is the address of the
     `jsr`/`bsr` instruction of the blocking call. The 68k executes that call and returns
     to the next instruction.
   - Register that next instruction as the start of the next range. The main loop
     (`mainloop.c`) and Reset (`reset.c`) are built like this. Loops whose body contains
     a blocking call are a range that ends at the call, plus a range after the call that
     either jumps back into the loop body (in C) or leaves the loop.
3. A range may have several exits (one per path) and may exit to its own start (a loop
   head). The stack pointer at the exit must equal the one at the entry. The layer aborts
   when a range leaves through an address it did not declare.
4. `difftest`/`dt` treat ranges automatically. The original runs from the start until it
   first reaches one of the declared exits at the entry SP, after at least one
   instruction. `difftest capture ADDR` works for any instruction address, so captures
   need nothing special. `-v` prints the exit each state took. Check that the interesting
   exits occur, and write a route script if they don't (`port/scripts/pointer_play.txt`
   and `inventory_play.txt` exist for this reason).
5. Registers and flags at an exit are compared like a function's return values. Code in
   the middle of a function leaves working values in registers, so reproduce them all.
   `port/src/game/cc68k.h` has helpers for the exact condition codes: `cc_sub`,
   `cc_add`, `cmpw`, `cc_btst`, `lslw`, `lsrw`.

### What nested 68k calls hide from the rest of the machine (#30)

68k code that runs nested inside C (`hle_call68k`, `hle_run68k_until`) executes within
one time slice of the emulator's frame loop. The raster can't advance until the C
function returns. The layer makes that safe (`nest_begin()` in hle.c):
- Interrupts are held off during nested execution. An SR-writing instruction would
  otherwise take a pending HBlank in the middle. They are re-raised from the VDP's
  pending flags afterwards, and the CPU takes them right after the C function.
- The VDP line base follows the CPU during nested execution, and is restored
  afterwards. Without this, a nested 68k VDP data-port write that ran far ahead of the
  raster pushed GPGX's FIFO model past `fifo_timing[]`. That stalled the CPU by ~15
  frames, and those VBlanks (`vblank_main`, i.e. pad reading) were lost. That was #30:
  only 323 of 390 frames ran vblank_main in `mouse/look.txt` with `--hle all`.
- A VRAM **fill/copy** DMA started before a C function doesn't progress during it, so
  a nested poll of the DMA-busy bit hangs. `game_init` after `boot_hw_init`'s fill does
  this, which is why `boot_init` stops in front of it.

- **DMA** started by nested code completes at once (#31). The frame loop advances a VDP DMA
  line by line, and it can't run until the C function returns. Before this, a nested
  `vdp_dma_vram` either stalled the CPU clock for seconds (#19) or never reached VRAM.
  `room_load`'s plane uploads went missing that way, and the Lobby was drawn without its
  background (hosttest "Lobby HLE-on" fell to 60%). Calling DMA routines from C through
  `hle_call68k` is fine now. #19's script.c keeps some DMA calls in 68k from before this
  fix; that is harmless. **C code that starts a DMA itself** (the VDP helpers, #13) brackets the
  control-port writes with `hle_dma_begin()` / `hle_dma_complete()` / `hle_dma_end()` (hle.h): the
  transfer completes at once and its stall time becomes nested time, charged by the layer. C never polls
  the DMA-busy bit.
- **The CPU clock** runs on across the nested calls of one C function, together with the
  raster line base and the V counter (#33, see "HV counter reads"). All three are rewound
  when the C function returns, and the nested time is added to its charge (see the next
  section). Before this, a nested LZSS run
  of millions of cycles followed by a Z80 bus request made the Z80/YM2612 catch up by
  frames at once (YM buffer overflow). room.c used to rewind the clock itself
  (`gx_m68k_set_cycles`); the layer does it now.

difftest holds IRQs, so it can't see any of this. `port/tools/irqtest.sh` (frameprof
"lost VBlank" count over the pointer, inventory and mouse routes, `--hle all` and
`none`) is the regression check. Run it with hosttest after changing C code that calls
68k code.

### Long-running functions: what to charge (#31), the policy

A C function takes no emulated time. What it costs is what the layer adds to the CPU
clock when it returns. **Every agent follows these rules:**

1. **Charge the original's cost.** Set `ctx->cycles` (or the `HLE_FUNC` cycles argument)
   to the average 68k cycles that difftest prints for the original. Large values are
   fine. Nested 68k calls are counted automatically: the layer rewinds the clock after
   them and adds their time to the charge. If your number is a measured *total* that
   already includes the nested callees, charge `total - hle_nested_cycles()` (room.c:
   `charge_total()`).
2. **Don't spread or cap the cost yourself.** The layer looks at the interrupt mask at
   entry:
   - **VBlank unmasked** (SR mask < 6): the cost above 1000 cycles is spent at a trap
     (`STALL_PC` = $0000F4) in 1000-cycle slices. Interrupts are taken between slices,
     as the original would have been interrupted along the way. The real continuation
     is pushed like a `jsr`, so a savestate taken meanwhile simply resumes there (the
     rest of the cost is dropped).
     - Example: `font_tiles_load` (1.02 M cycles) is called by scripts with interrupts
       on. Charged in one go, it skipped ~8 VBlanks per call. That was the irqtest
       failure on `mouse/walk.txt` in #31.
   - **Masked** (`room_load`, called at SR = $2700; Reset pieces before `move #$2000,sr`):
     charged at once. The original loses those VBlanks too.
3. **Don't make a long function an `HLE_RANGE` chain just for its cycle cost.** Split
   only around real **waits** (blocking calls, see "Waiting for VBlank"). A loader that
   never waits is one `HLE_FUNC` with a big charge.
4. **Never move the CPU clock yourself** (`gx_m68k_set_cycles` is for the layer), and
   don't busy-wait on emulated time in C.
5. **Exact frame timing is not a goal.** Recorded frame-exact routes (`lobby.txt` and
   friends) can desync with C enabled, because the charges are averages and C work
   happens at the call instead of spread over the original's instructions. difftest
   checks state, and `irqtest.sh` checks interrupt delivery. Closed-loop route testing
   is a separate task.

`port/tools/irqtest.sh` (no lost VBlanks), `hosttest.sh` and `mousetest.sh` must pass with
`--hle all` after any change to charges or nested calls.

### Tail jumps into 68k (HLE_FUNC + hle_jump)

A C function may do the deterministic part of a routine and `hle_jump` into the original
code for the rest, for example a rare path that can't be verified (`factory_test` jumps to
the test screen). difftest follows such a jump: after the C function it keeps running the
68k until the function returns (or the range exits), and then compares.

### Timing-dependent reads

- **HV counter reads: the strategy (#33).** The 68k code reads the HV counter at
  `$C00008` in four places:
  - `hud_dma` `$005C8E`: upload the HUD only while the beam is in lines `$E1`–`$F6`.
  - `actor_dma` `$00AF24`: actor tile DMA only in the HV window `$E000`–`$F000`.
  - `game_init` `$00A248`: RNG step.
  - `hblank_verb_bar` `$01006C`: raster wait.

  None of them is in C. `hud_dma` and `actor_dma` are called from C `vblank_main`
  through `hle_call68k`; `game_init` and `hblank_verb_bar` stay 68k (see below). The rules:
  1. **Keep HV reads in 68k code, and call it nested.** Inside a top-level C function, time
     runs on across its nested 68k calls. The CPU clock, the raster line base **and the V
     counter** advance with the nested instructions, so an HV read after `camera_update`
     and `palette_cycle` sees the line the original would see (roughly: C's own work
     counts as zero). When the C function returns, the layer rewinds all three and charges
     the time.
  2. **Don't read `$C00008` from C code.** C runs at the time of its entry plus the nested
     time so far, and has no notion of its own cost. If C must decide on the beam
     position, call the 68k routine that does the read.
  3. **If the HV value flows into game state** (`game_init`: HV → `rng_next` → X flag),
     keep the routine in 68k: no C ordering reproduces the exact value.
  4. In difftest the V counter is frozen at the entry line on both sides, so these
     functions compare equal there. The live behaviour is checked by
     `port/tools/storytest.sh` (closed-loop gameplay) and `irqtest.sh`.

  **#33**: before #31, nested calls advanced the clock but not the V counter. After #31
  each nested call was rewound on its own, so in C `vblank_main` the beam was always at
  line `$E0` when `hud_dma` checked it. The HUD upload never happened, `VBF_HUD_DMA` (bit 5
  of `$FF09DC`) was never cleared, and `inventory_redraw` (`$5D0E`) hung after every TAKE.
- **Interrupt mask**: never lower the mask in `ctx->sr` before a nested `hle_call68k`. A
  pending HBlank would run `hblank_verb_bar` inside the nested call, and its raster wait
  never ends. Reproduce only the final SR the original leaves behind (see `vblank_main`).
  Lowering it at the exit of a range or function is fine, and needed (Reset's
  `move.w #$2000,sr` before `palette_fade`). The interrupt is then taken right after the C
  code, at the same instruction boundary as in the original.

### Interrupt handlers

The VBlank and HBlank vectors are 5-instruction trampolines (`movem`, `jsr (hook)`,
`movem`, `rte`) through the RAM hooks `$FF0000`/`$FF0004`. They stay in 68k. The hook
targets (`vblank_main`, `vblank_light`, `vblank_title_fade`, `hblank_nop`) are ordinary
functions and are ported as `HLE_FUNC`. Code called from VBlank must not wait for VBlank.
It may call anything non-blocking through `hle_call68k`.

### Tools

- `build/port/frameprof [--script F]... [--watch ADDR=name]... [--frames] [--hist A B]`
  prints, per frame or as segments, which hooks are active, the VBlank/HBlank/main-loop
  counts, first/last hits of watched addresses, and the hottest instructions. Use it to
  see where code runs and how often.
- `tools/py/blocking.py`: see above.
- `port/tools/irqtest.sh`: no gameplay frame may miss vblank_main with IRQs unmasked
  (uses `frameprof --hle SPEC`, which also counts this).

## Functions replaced so far

| Address | Name | File | difftest result |
| --- | --- | --- | --- |
| $1F6D7A | z80_bus_request | io.c | 200/200 |
| $1F6D8E | z80_bus_release | io.c | 200/200 |
| $009D34 | pad_read (reads pads 1 and 2; PADn_CUR/PREV/EDGE at $FF09E0–$FF09E7) | io.c | 200/200 |
| $00992C | vram_write_words (VRAM memcpy from (A0)+) | vdp.c | 200/200 |
| $009964 | vram_write_word | vdp.c | 200/200 |
| $009BE4 | vram_fill_words (VRAM memset; also reached via `bra` from $9C58) | vdp.c | 96/96 (all calls on the route) |
| $005B56 | str_copy (strcpy A0 to A1) | util.c | 54/54 (all calls on the route) |
| $005E4A | player_update (Shaggy per-frame update; see docs/subsystems/walking.md) | walk.c | 200/200 + walk_dt.sh |
| $006AE8 / $006B28 / $006C28 / $006D24 | player_zone_flags / camera_follow / verb_menu_open_check / actors_update (tails of player_update) | walk.c | 200/200 each |
| $0073D6 | walk_blocked_probe (per-pixel collision test) | walk.c | 200/200 |
| $007476 | walk_blocked_line_probe | walk.c | 200/200 |
| $00750A | pad_dir8 | walk.c | 7/7 (all calls on the route) + walk_dt.sh |
| $00758A | room_load (room/scene loader) | room.c | 306/306 (6 captured states x 51 rooms of both mysteries, via `--poke`) |
| $007950 | room_draw_view | room.c | 120/120 (6 states x 20 camera positions) |
| $007A2A | font_tiles_load (exact regs+flags) | room.c | 42/42 (all calls on the route) |
| $007A8C | room_setup_actors | room.c | 180/180 (6 states x 30 rooms) |
| $007F22 | game_state_init | room.c | 2/2 (all calls on the route) |
| $007F66 | object_table_init (ends by `hle_jump` into the 68k mystery intro) | room.c | not difftestable; object table identical after boot, C vs 68k (`dt-room`) |
| $00750A | pad_direction | room.c | 7/7 (all calls on the route) |
| $0073D6 / $007476 | zone_probe_cursor / zone_probe_point (exact regs+flags) | room.c | 12/12 each |
| $007D54 | overlay_apply | room.c | 1008/1008 (12 states x 84 ids/camera/DMA-mode variants, flags ignored) |
| $007C04 | objects_update | room.c | 12/12 |

`port/tools/dt-room` runs the room.c functions with the extra `--poke` variants.

The "pad read $9D3A" in older notes is the body of $9D34. The function is entered at
$9D34 with `jsr $1F6D7A` (Z80 bus request) and is called from $A4A4 and $A6C0.

The five walking entries ignore final D0-D7/A0-A6/CCR (`dt_ignores` in difftest.c): they only
return into vblank_main, whose VBlank wrapper restores every register.

#12 (main loop, interrupts, Reset), files `mainloop.c` and `reset.c`. Details and
per-function results are in `docs/subsystems/mainloop.md`:

| Address | Name | File | difftest result |
| --- | --- | --- | --- |
| $00A65C | vblank_main | mainloop.c | 200/200 |
| $00A4A4 | vblank_light | mainloop.c | 200/200 |
| $00A38C | vblank_title_fade | mainloop.c | 200/200 |
| $00879E | title_anim | mainloop.c | 200/200 |
| $01006A | hblank_nop | mainloop.c | 200/200 |
| $000D3E (range) | main_loop_pass + 9 small ranges after blocking calls | mainloop.c | 200/200 (+ pointer and inventory routes); the inventory-arrow ranges have no states |
| $000200… (ranges) | boot_hw_init, region_check, boot_init, logo1–5 pieces, logos_done, game_start… (21 ranges) | reset.c | all calls on the route (2–200 each) |
| $00069A | factory_test | reset.c | 3/3 |
| $00A27A | game_reset | reset.c | 5/5 |
| $000BC8 | logo5_frame | reset.c | 183/183 |

#19 (script interpreter), file `script.c`. Byte code: `docs/script.md`; per-function list,
split into ranges and difftest results: `docs/subsystems/script.md`. Summary: the two dispatch
loops (4 ranges), the non-blocking handlers (case_verb, talk_choice, if, set, set_verb,
sound, set_hotspot, on_event, freeze, pal_cycle, 9 sub-ops) as functions, the blocking ones
(if_else, move_obj, anim_wait, set_state, sys, wait, goto_room(_cut), wait_anim,
unfreeze_wait, bar_hide/bar_show) as ranges around their waits, and the drivers
action_execute, run_object_events, the room scripts and the TALK menu loop as ranges.
`tools/py/script_dt_all.sh` / `script_dt_pokes.sh` run them all.

#14 (decompressors), file `decomp.c`, details in `docs/subsystems/decomp.md`; test driver
`port/tools/dt-decomp` (captured states plus one variant per LZSS/RLE stream in the ROM):

| Address | Name | File | difftest result |
| --- | --- | --- | --- |
| $0097AA | lzss_decompress (per-token cycle model, called directly by room.c / reset.c) | decomp.c | 484/484 (60 route states + 212 stream variants x 2 states) |
| $009C64 | rle_decompress | decomp.c | 248/248 (8 route states + 31 title/logo streams, with and without the $FF09ED bit 3 side effect) |

#13 (VDP helpers), files `vdp.c` and `palette.c`; design, the native-renderer hook (`vdp_set_hook`) and the
list of remaining raw `$C00004` users are in `docs/subsystems/vdp.md`; test driver `port/tools/dt-vdp`:

| Address | Name | File | difftest result |
| --- | --- | --- | --- |
| $0099F6 / $009A3C / $009B10 | vdp_dma_vram (64 KiB split) / vdp_dma_setup / vdp_dma_cram | vdp.c | 200/200 each + 600/600 poked variants for the split and the $8000-word overflow |
| $009996 / $0099C4 | vram_read_word / cram_write_word | vdp.c | 200/200 / 60/60 (VBlank-hook probe + pokes) |
| $009C1E / $009C58 / $009C5E | vdp_fill_cram / vdp_clear_vram / vdp_clear_cram | vdp.c | 3/3, 36/36, 3/3 (all calls on the route) |
| $00972E / $009716 / $009722 | vdp_reg_set / plane_a_clear / plane_b_clear | vdp.c | 60/60 (all calls) / 20/20 / 20/20 (probe) |
| $009F72 / $009F58 / $009F46 / $009F30 | draw_text_plane_a / _hi / draw_text_labelled / draw_text_centered | vdp.c | 170/170, 170/170, 60/60, 17/17 (password and sound-test screens, probe) |
| $009FA2 / $00A054 | draw_text_wrapped / draw_status_line | vdp.c | 140/140 (20 probe states x 7 poked strings, 0-90 chars) / 42/42 (6 states x 7 strings) |
| $009DE0, $009DF2, $009E60 (ranges) | palette_read_cram (fade out) | palette.c | 34/34, 34/34, 200/200 |
| $009E82, $009F0A (ranges) | palette_fade (fade in; the movem push/pop stay 68k) | palette.c | 116/116, 800/800 (route has one fade-in; pokes vary target and step) |

#15 (sprites and actor animation), files `sprite.c`/`sprite.h`. Details: `docs/subsystems/sprite.md`,
formats: `docs/formats.md` "Actor sprites":

| Address | Name | File | difftest result |
| --- | --- | --- | --- |
| $00A984 | sprite_build (SAT; incl. helper $00AD16 actor_draw) | sprite.c | 200/200 |
| $00B0B8 | actor_animate (scripts, frame decode, depth scaling; follow-mode spin stays 68k via hle_jump) | sprite.c | 200/200 (flags ignored) |
| $00D468 / $00DA6A | frame_scale_48x80 / _80x48 (depth scalers) | sprite.c | 200/200, 115/115 (all calls on the route; regs/flags ignored) |
| $00D3F0 / $00D42C | frame_pack_48x80 / _80x48 | sprite.c | 11/11 each (`scripts/sprite_pack.txt`, all calls) |
| $00801A | actors_init (tail-jumps to its `bsr actor_animate`) | sprite.c | 2/2 (all calls on the route) |
| $00AF24 | actor_dma | stays 68k (HV read, #33); C reference under `#ifdef SPRITE_ACTOR_DMA_C` | |

#21 (title screen, menus, password screens, sound test, DEBUG editors), files `menus.c`, `menu_password.c`,
`menu_debug.c`, `soundtest.c`, `password.c`. Details: `docs/subsystems/menus.md`. About 70 functions/ranges (the
whole menu system waits for VBlank, so it is mostly HLE_RANGEs); test driver `port/tools/dt-menus` plays
`port/scripts/menus/tour1..3.txt` (one session, generated by `gen.py`) and difftests every range on the states it
captures there, plus 150 poked variants each for the password routines (`tools/py/menus_pokes.py`); the codec test
is `port/tools/password_test.sh`. Register exemptions are in `difftest.c` ("#21 menus"). `difftest` now also prints
"C charged avg" (the cycles the C side charges without its nested 68k calls) next to the original's cost.

### Sound glue (#22, `sound.c`, docs/subsystems/sound.md)

| Address | C | Result |
|---|---|---|
| $1F6EF8 / $1F6F16 | sound_cmd_play / sound_cmd_stop | 109/109 and 7/7 (all calls on the capture + `scripts/sound_spam.txt` routes) |
| $1F6F52 | sound_cmd_stop_all | 123/123 |
| $1F6F2A / $1F6F3E | sound_cmd_pause / sound_cmd_unpause | **no states**: no scriptable route reaches the in-game pause (cutscene loop at $003372); same code shape as stop_all |
| $009762 / $009776 | sound_play_wrapper / sound_stop_wrapper | 436/436 and 28/28 (109 / 7 route states x 4 `--poke D0=` ids) |
| $1F6D98 / $1F6DDE / $1F6EA8 | z80_load_driver / z80_reset_driver / sound_init_wrapper | 3/3 each (boot is the only caller; all calls on the route) |

`port/tools/soundtest.sh` compares the Z80 mailbox byte stream (`SCOOBY_SNDLOG`) of `--hle all`, `--hle none` and
`--hle all` minus the sound functions on the Sound Test route, an in-game route and a rapid-press route.
