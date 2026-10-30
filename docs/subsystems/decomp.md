# Subsystem: decompressors and asset loading (GitLab #14)

Code: `port/src/game/decomp.c`. Formats: `docs/formats.md` ("Compression format 1/2"). Reference
implementations in Python: `tools/py/gfx.py`. Test driver: `port/tools/dt-decomp`.

| Address | C name | What it does | Status |
|---|---|---|---|
| `$0097AA` | `lzss_decompress` | LZSS (4 KB ring, flag bit + 8-bit literal / 12-bit position + 4-bit length, position 0 ends). a0 = stream, a1 = ring ($FF3000), a2 = output; leaves the size at (a1).l. All registers preserved | C, difftest 484/484 |
| `$009C64` | `rle_decompress` | PackBits-like RLE. a0 = stream, a2 = output. Preserves d0-d2/a0-a2; toggles VDP reg 12 (shadow/highlight) if `$FF09ED` bit 3 | C, difftest 248/248 |
| `$0098F8` | (init helper) | `moveq/moveq/move.l (a0)+,d0/movea.l/lea` inside lzss; has no other caller | folded into `lzss_decompress` |

The 68k routines are leaf code (no VBlank waits), so they are plain `HLE_FUNC`s. Every caller that
is not ported yet (title screen, sprite tile uploads `$00B358`/`$00B404`, menus `$008D36`,
`$0096F4`, logos, mystery intro, ending credits) reaches them through the 68k `jsr` and gets C
automatically. Callers that are C (`room_load` in room.c, the logo ranges in reset.c) call
`lzss_decompress` / `lzss_call` directly. The callers listed above contain waits for VBlank
(`sub_0096F4`, `sub_008D36`) or are part of bigger unported actor code (`$00B11A` sprite
loader), so they belong to other subsystems; the VRAM/DMA copies after the decompression
(`$00992C`, `$0099F6`, `$009B10`) are VDP helpers (#13/#12).

## Exact behaviour worth knowing

* RLE runs are written as whole longs (after aligning a2) and a2 is rewound: up to 3 bytes past
  the end of the last run are overwritten with the run value. The C does the same because difftest
  compares RAM.
* RLE's end test is `a2 == a1` with a1 = start + the LOW WORD of the size long (`lea (0,a2,d0.w)`).
* LZSS: flags at exit = `move.l a2,(a1)`: N/Z from the size, V=C=0, X=0.
* RLE: flags from the final `move.w #$8c00,(vdp_ctrl)`: N=1, X = the X left by the last run token
  (`lsr.w #2,d0`: bit 1 of the remaining count).
* The ring buffer is never cleared; it persists between calls (so a stream may in principle read
  bytes the previous call wrote). C works on the real RAM ring, so this is preserved.

## Cycle charges (policy "Long-running functions")

The cost is data dependent (LZSS from 7 K to 6.7 M cycles per call, avg ~0.9 M over the route
and the stream variants), so a flat average would be wrong for both the small sprite frames
and the 25 KB room tiles. Instead the C code counts instructions of the original path by path
(literal, match, per-byte copy, refill, ...) and charges that: its instruction count matches the
original exactly (60/60 captured states), and the cycle total is within ~3 % (the emulated
original runs a steady ~2.7 % longer than the 68000 timing tables; the factor is applied;
the residual per state is 2-3 %). RLE: model is exact to 4 cycles. The layer spreads big
charges as policy says (slices at the stall trap when VBlank is unmasked; room_load runs masked
and is charged whole). `room_load`'s measured totals already contain the LZSS time and nothing
is nested any more, so they are unchanged.
