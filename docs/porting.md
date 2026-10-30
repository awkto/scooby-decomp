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

difftest holds IRQs, so it can't see any of this. `port/tools/irqtest.sh` (frameprof
"lost VBlank" count over the pointer, inventory and mouse routes, `--hle all` and
`none`) is the regression check. Run it with hosttest after changing C code that calls
68k code.

### Tail jumps into 68k (HLE_FUNC + hle_jump)

A C function may do the deterministic part of a routine and `hle_jump` into the original
code for the rest, for example a rare path that can't be verified (`factory_test` jumps to
the test screen). difftest follows such a jump: after the C function it keeps running the
68k until the function returns (or the range exits), and then compares.

### Timing-dependent reads

- **HV counter / VDP status reads** (not waits): in the sandbox the V counter stays at
  the entry line and the H counter moves with the CPU clock. C code reads at the CPU time
  of the function's entry (C takes no time until it returns).
  - If only V matters (`sub_005C8E`: "is the raster still in lines `$E1`–`$F6`?"), C and 68k
    agree in difftest.
  - If the exact value flows into state, keep the function in 68k. `game_init` is the
    example: HV → `rng_next` → X flag. A tail jump to the read doesn't help either: the C
    prefix took no time, so the 68k still reads earlier than the original did. Tried and
    measured in #12.
  - In real play the earlier read just shifts a DMA by a frame.
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
