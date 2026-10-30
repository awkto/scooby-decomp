# Subsystem: sprites and actor animation (GitLab #15)

Code: `port/src/game/sprite.c`, `sprite.h`. Data formats (animation sets, scripts, frame headers,
chunky/tile frames, depth scaling, piece layouts, SAT order): `docs/formats.md` "Actor sprites".
Decoder/renderer: `tools/py/gfx.py sprite` and `gfx.py spritecheck`.
RAM names: `data/ram.txt` block "#15"; ROM names: `data/symbols.txt` block "#15".

## Functions

| Address | Name | What it does | Status |
|---|---|---|---|
| `$00801A` | `actors_init` | game start: slots 0/1 = Shaggy/Scooby sets, active + depth-scaled, animation 4 requested, timers −1, no frames shown; tail `bsr actor_animate` | C (tail-jumps to the `bsr` at `$8190`, which may block), 2/2 (all calls on the route) |
| `$00A984` | `sprite_build` | the SAT in `$FF0008`: pointer, effect sprite, inventory highlight, fixed sprites, verb-bar masking sprites, then the actors nearest first | C, exact registers and flags, 200/200 |
| `$00AD16` | `actor_draw` | helper of sprite_build (only caller): latch the DMA'd frame, emit its pieces | C (static `actor_draw`, covered by sprite_build) |
| `$00AD14` | `sprite_build_nop` | a lone `rts`, never called | – |
| `$00AF24` | `actor_dma` | VBlank: tile DMA of one slot (raster window `$E000`–`$F000`), sprite_build, SAT DMA, script timers | 68k with the GPGX renderer (HV counter read, docs/porting.md "HV counter reads", #33); C in the opt-in group `native` (`--renderer native`, #25) with the frame-loop HV model (docs/subsystems/render.md), D6 = HV value ignored in difftest |
| `$00B0B8` | `actor_animate` | step every slot's animation script, decode frames, depth-scale chunky frames, follow-mode sync | C, 200/200 (flags ignored: every caller tests only flags it sets itself); blocking follow-mode path: rebuilds the `movem` frame and `hle_jump`s to the 68k spin at `$00B5A4` |
| `$00D3F0` / `$00D42C` | `frame_pack_48x80` / `_80x48` | chunky → tiles without scaling (`$FF09E9` bit 3 and scaled hot spot y < 2) | C, 11/11 each (all calls of `port/scripts/sprite_pack.txt`) |
| `$00D468` / `$00DA6A` | `frame_scale_48x80` / `_80x48` | depth scaler | C, 200/200 and 115/115 (all calls on the route) |

The scalers' final D0–D7/A1–A5 and flags are dead (only caller `actor_animate`, which reloads them and is
itself wrapped in `movem`); difftest ignores them (`dt_ignores`). `actor_animate` calls them as C directly and
adds their measured 68k cost to its charge (143,852 / 122,489 / 94,000 cycles). RLE decoding (`$009C64`, #14)
and `sound_play`/`sound_stop` are called through `hle_call68k` (the decompressor's C version from #14 applies
there when it is enabled).

Charges (difftest averages of the originals): sprite_build 5,279; actor_animate 1,700 + scalers (+ nested
RLE); actors_init 500 + the 68k tail.

## Frame pipeline

See the diagram in docs/formats.md ("From script to screen"). In short: `actor_animate` (main loop) produces
tiles into the slot's RAM buffer and sets `TILES_READY` (`$FF0AB6`); `actor_dma` (VBlank) uploads one slot per
frame and clears `DMA_DONE` (`$FF0AC1`); `sprite_build` (same VBlank) latches the frame header, hot spot and
depth for display once nothing newer is waiting, and draws it.

Two quirks, kept bit for bit:
- The depth latched for display (`$FF05C4/$FF05D0`) is the depth at latch time, not the one the scaler used.
  The hot spot is the scaler-time one, so the picture is right; only the latched depth can be stale
  (seen in `runs/sprite_ref/c`: tiles scaled at `$77`, latched `$75`).
- If a newer frame is decoded before the DMA'd one is latched, the old header is shown with the new tiles
  for a frame.

The animation of an actor whose hot spot is more than 80 px off screen pauses (Scooby's never does).

## Follow mode (`$FF09E8` bit 3, script sub-op `$15`)

Script sub-op `$15` waits for Shaggy's animation to end, puts the first free slot ≥ 2 in `$FF08B0` and sets
`$FF09E8` bit 3. From then on Shaggy's and the partner's frames are released together: each marks
`$FF0AC6` when it starts a frame, the second loop of `actor_animate` scales both only when both are marked,
then the tail places the partner at Shaggy's position minus the depth-scaled offset at Shaggy's frame header
`+$0E/+$10`, releases both frames and spins (interrupts on) until VBlank has uploaded both. `actor_dma`
skips the raster window test for the partner. With `$FF09E9` bit 3 the partner also copies Shaggy's
animations through `$0FC760`. No route reaches follow mode yet, so this path is translated but untested.

## Verification

- `port/tools/dt sprite_build actor_animate frame_scale_48x80 frame_scale_80x48 actors_init` on the capture
  route; `frame_pack_*`: `build/port/difftest capture 00D3F0 200 --script port/scripts/sprite_pack.txt`
  (shrinks Shaggy/Scooby to depth `$FC` with `$FF09E9` = `$0A`), then `port/tools/dt frame_pack_48x80`.
- Format: `port/scripts/sprite_ref.txt` (harness: Lobby, then walk) dumps RAM/VRAM/screenshot at 3 points;
  `gfx.py spritecheck runs/sprite_ref/X.ram runs/sprite_ref/X.vram --shot X.ppm` rebuilds Shaggy's and
  Scooby's tiles from ROM: byte-identical to VRAM `$F000`/`$F780` at all 3 points, and Shaggy's rebuilt picture
  matches the screenshot pixel for pixel (206/206, 257/257; Scooby is partly behind the foreground pillar).

## Open

- Header word `+6` of a set, frame header `+$0C`, and the set's frame table (no reader found).
- The 32-wide, non-80-high piece table looks broken (all pieces reuse tile 0) and no frame uses it.
- Follow mode and the `$FF09E9` bit 3 partner animation have no test states.
- `$FF0AC8` (cleared by actors_init) and the `+$0A`.. effect sprite tables are not decoded.
