# Subsystem: VDP helpers (GitLab #13)

Code: `port/src/game/vdp.c` (uploads, fills, DMA, register writes, plane-A text), `port/src/game/palette.c`
(fades). Names: `data/symbols.txt` block "#13". Cycle charges are the difftest averages of the originals.

Everything the game does to VRAM/CRAM goes through a handful of small routines at `$00972E`-`$009F72`.
The rest of the `$C00004` writers are inline in other subsystems (list at the end).

## Functions

| Address | C name (68k symbol) | What it does | Status |
|---|---|---|---|
| `$00992C` | `vram_write_words` (`vdp_copy_to_vram`) | CPU copy of D1 words from (A0)+ to VRAM D0 | C (#11) |
| `$009964` | `vram_write_word` (`vdp_write_word`) | one word to VRAM D0 | C (#11) |
| `$009996` | `vram_read_word` | D1.w = VRAM word at D0 | C |
| `$0099C4` | `cram_write_word` | one word to CRAM D0 (no static caller) | C |
| `$0099F6` | `vdp_dma_vram` | DMA D1 words from A0 to VRAM D0; cut at 64 KiB source boundaries | C |
| `$009A3C` | `vdp_dma_setup` | one VRAM DMA transfer (regs, bus request, start, wait) | C |
| `$009B10` | `vdp_dma_cram` | the same for CRAM (palette: D0=0, D1=$40, A0=`ram_pal_cur`) | C |
| `$009BE4` | `vram_fill_words` (`vdp_fill_vram`) | fill D1 words of VRAM D0 with D2 | C (#11) |
| `$009C1E` | `vdp_fill_cram` | same for CRAM (only reached by `$009C5E`) | C |
| `$009C58` / `$009C5E` | `vdp_clear_vram` / `vdp_clear_cram` | `clr.w d2` then fill | C |
| `$00972E` | `vdp_reg_set` | VDP register D0 := D1 | C |
| `$009716` / `$009722` | `plane_a_clear` / `plane_b_clear` | zero the 64x32 nametables at `$C000` / `$E000` | C |
| `$009DE0` | `palette_read_cram` | fade OUT: read CRAM, 8 steps down, wait + DMA each | C, 4 ranges |
| `$009E7E` | `palette_fade` | fade IN to the 64-word image at A0 | C, 2 ranges (+ the movem in 68k) |
| `$009F72` | `draw_text_plane_a` | string at A0 -> plane A tiles (char + `ram_font_base`) at column D1, row D2 | C |
| `$009F58` | `draw_text_plane_a_hi` | same with tile priority | C |
| `$009F46` | `draw_text_labelled` | draws the string at `$02FFF4` at column 0, then the caller's | C |
| `$009F30` | `draw_text_centered` | column = (32 - strlen) / 2 | C |
| `$009CDE` | `sub_009CDE` | bchg of a vblank flag, then a double-buffered word write to `$FF0808`; **no caller anywhere** (dead code) | not ported |
| `$009FA2` | `draw_text_wrapped` | word-wrapped text (30 columns) into the dialogue-choice rows of the verb-bar plane, `$FF080A` = rows * 300 | C |
| `$00A054` | `draw_status_line` | clear plane `$D000` (1024 single-word writes), draw the status/command line wrapped and right-aligned, set VDP reg `$12` and `$FF080A` | C |
| `$009C64` | `rle_decompress` | writes `$C00004` only to set/clear the H40-interlace register `$8C08/$8C00`; ported in #14 | C (#14) |
| `$005C8E`, `$00AF24` | `hud_dma`, `actor_dma` | read the HV counter | 68k by rule (docs/porting.md "HV counter reads"); C with the frame-loop HV model in the `native` group (#25, render.md) |

## Design

The 68k routines write the control port directly. The C versions repeat those writes in the same
order (difftest compares the ordered list of VDP-port writes), but each routine first expresses
what it is about to do as a *semantic operation* and offers it to a hook.

### The hook point for a native renderer (#25)

```c
typedef struct vdp_op {
    vdp_op_kind kind;   /* VDP_OP_COPY: words from a 68k address;  VDP_OP_FILL: words of a value */
    vdp_mem     mem;    /* VDP_VRAM or VDP_CRAM */
    bool        dma;    /* COPY: DMA (68k stalls) or CPU data-port writes */
    uint32_t    src;    /* COPY: 68k address of the first word (ROM or work RAM $FF0000..) */
    uint16_t    value;  /* FILL */
    uint16_t    dst;    /* byte address in VRAM/CRAM; auto-increment is 2 */
    uint32_t    words;
} vdp_op;
typedef bool (*vdp_hook_fn)(const vdp_op *);
void vdp_set_hook(vdp_hook_fn);          /* in game.h */
```

"Upload N words from RAM/ROM address X to VRAM Y" is a `VDP_OP_COPY` with `mem = VDP_VRAM`,
`src = X`, `dst = Y`, `words = N`. A renderer installs a hook, applies the operation to its own
tile/plane/palette memory (reading the words with `RAM16(src)` / `ROM16(src)` / `bus_r16`) and
returns `true`; the helper then touches no VDP port, only advances A0 and charges the cycles.
Returning `false` (or having no hook, the default) performs the operation on the emulated VDP.

Where the hook is consulted:

| Caller | Op |
|---|---|
| `vram_write_words` | COPY, `dma = false` |
| `vdp_dma_vram` | COPY, `dma = true`, the **whole** logical upload (before the 64 KiB cut) |
| `vdp_dma_setup`, `vdp_dma_cram` | COPY, `dma = true` (one transfer; CRAM from `ram_pal_cur`) |
| `vram_write_word`, `cram_write_word` | FILL of 1 word |
| `vram_fill_words`, `vdp_fill_cram` (and the clear entries) | FILL |

Not covered, because the game does not go through these routines: VSRAM writes (scroll values are
written inline), the sprite attribute table DMA (it is `vdp_dma_vram` from `$FF0008`, so it *is* covered),
VDP register writes (`vdp_reg_set` and inline `$8Fxx`/`$9xxx` words), and VRAM *reads* (`vram_read_word`
reads through the port; a renderer that owns VRAM must serve it). Plane text (`draw_text_*`) is built
from `vram_write_word`, so it arrives as FILLs of 1 word.

Timing when a hook handles an op: the layer is charged `DMA_SETUP_CYCLES` + 5 cycles per word
(the blanking DMA rate) instead of the real stall, so the game's pacing barely changes.

### DMA from C and the clock

A 68k-to-VDP DMA halts the CPU for its whole length. In the emulator the frame loop advances a
DMA line by line, which cannot happen while a C function runs, so C never polls the DMA-busy bit
(the original does: `btst #1,status`). `dma_chunk()` brackets the sequence with
`hle_dma_begin()` / `hle_dma_complete()` / `hle_dma_end()` (hle.c): the same "nested time" machinery
that runs 68k code inside C (docs/porting.md #31/#33). The transfer completes at once; the stall
(real DMA time, ~5 cycles per word in blanking, more in the active display) is part of the nested
time that the layer rewinds and then adds to the charge; and spreads in slices when interrupts are on.
C code does not move the clock itself. A C function that calls `vdp_dma_*` directly adds
`VDP_DMA_CYCLES` to its own `ctx->cycles` (the HLE_FUNC charge only applies to calls from 68k).

DMA register programming, for the record (`dma_chunk`): reg 1 = shadow (`ROM[$010124]`) | `$10`;
`$93/$94` length in words; `$95-$97` source >> 1 (the top byte's bits 6/7 are the DMA type, 0 = 68k bus);
control words `(dst & $3FFF) | $4000` (VRAM) or `$C000` (CRAM), then `$80 | dst >> 14`, which starts
the DMA. Afterwards reg 1 goes back to the shadow value. The Z80 bus is requested around it
(`z80_bus_request/release`).

`vdp_dma_vram` splits a transfer at the next 64 KiB boundary of the *source* address (the DMA source
counter is 17 bits wide in words, i.e. wraps at 128 KiB; the game cuts at 64 KiB to be safe). The
original has a 16-bit overflow when the first piece is exactly `$8000` words (D1.w << 1 = 0): the
destination of the second piece is then wrong. C keeps that behaviour.

### Flags

Every set-address routine saves SR right after `lsr.w #6` on the address and restores it at the end, so the
caller sees the flags of that shift (`addr_flags`). `vdp_dma_*` preserve the SR entirely (the Z80 helpers'
flag changes are undone), except that `vdp_dma_vram` leaves the flags of its last `cmp.l` / `add.w`.
`draw_text_plane_a` returns D0-D2 *sign-extended* from their saved words (`movem.w (sp)+`).

### Fades and blocking

Both fades wait one VBlank per step (`wait_vblanks`) and are chains of `HLE_RANGE`s that stop in front of
the wait (docs/porting.md "Waiting for VBlank"): see the comment at the top of `palette.c` for the exits.
A range must leave with the stack pointer it entered with, so the `movem` push of `palette_fade` stays 68k
(the first range starts at `$009E82`), and so does the pop + `rts` at `$009F2A`.

## Verification

`port/tools/dt-vdp` (on a VM) runs everything; results are in the table in docs/porting.md. Notes:
- Functions the capture route never reaches were tested from route scripts (`port/scripts/password_screen.txt`,
  `sound_test.txt`) or by pointing the VBlank hook at them (`port/tools/vblank_probe.sh`, then `difftest --poke`
  for inputs): `cram_write_word` and `draw_text_labelled` have no reachable caller in normal play, the plane
  clears only run in the fullscreen-picture script op.
- The 64 KiB split of `vdp_dma_vram` and its 16-bit overflow are covered by `--poke` variants, not by the route.
- `SCOOBY_VDP_HOOKTEST=1` installs a test "renderer" (vdp.c `selftest_hook`) that redoes every hooked op from its
  `vdp_op` description alone, through data-port writes. The game must draw and play the same with and without it:
  with it on, irqtest and storytest (`STORYTEST_HLE=all`) pass, and Lobby / password / sound-test screenshots differ
  from the normal run by only 170-260 pixels (sprite animation phase: the hooked path is charged a different cost).

## Remaining raw `$C00004` users

These write VDP registers / scroll values / the control port inline, not through the helpers, and belong to other
subsystems. Found with `grep -n 'vdp_ctrl\|vdp_data' src/scooby.s`:

| Where | What | Owner |
|---|---|---|
| Reset, `boot_vdp_init` `$00091C`, `$00093C`, `$0009CE`, `$000A38`, BusError | register init, CRAM `$C0000000` clears, blank-screen toggles | #12 reset.c (ranges) |
| `mystery_intro` `$0014C8`-`$0017FA` | scroll/VSRAM writes, display on/off | #19 / intro |
| script ops `minigame_tug` `$003183`, `closeup_picture` `$004378`, `fullscreen_picture` `$0045C8`-`$004716`, `$0038DC`, `$00421C` | plane/scroll/register writes for picture scenes | #19 script.c |
| `title_screen` `$008525`-, `$008876`, `$008972`-`$008F6C`, `menu_start_game`, `$009008`-`$00920A`, `sound_test` | title/menu/password: register writes, scroll | title (#12) |
| `vblank_main` `$00A65C` and `sub_00A130` | scroll registers, window register `$9202` | #12 mainloop.c / verbs |
| `game_init` `$00A248` | reads the HV counter (RNG) | stays 68k (HV rule) |
| `hud_dma` `$005C8E`, `actor_dma` `$00AF24` | read the HV counter | stay 68k (HV rule) |
| `$00FC0A`-`$010104` incl. `hblank_verb_bar` `$01006C` | HBlank raster code | stays 68k (raster/HV) |

A native renderer therefore still has to emulate the VDP registers and the scroll/VSRAM state; what this subsystem
gives it is the bulk data path (tiles, nametables, SAT, palette) as semantic ops.
