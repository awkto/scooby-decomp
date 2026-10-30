# Native renderer and frame pacing (GitLab #25, groundwork for #24 / #26 / #27)

Code: `port/src/render/` (renderer, emulator bridge, output filters), `port/src/game/frameloop.c`
(the beam-free replacements of the HBlank split and the VBlank HV readers), `actor_dma` in
`port/src/game/sprite.c`. Patch: `tools/patches/gpgx-render.patch` (three timing hooks in GPGX's
line renderer). Test: `port/tools/rendertest.sh`.

## Functions

| Address | Name | What | Status |
|---|---|---|---|
| `$01006C` | `hblank_verb_bar` | HBlank hook: raster wait to line 167, then planes A/B to `$A000`, v-scroll 0, h-scroll = `ram_hscroll_verb`, window reg = `ram_verb_bar_window` | C in the `native` group: records the band, no VDP access, no busy-wait. Not difftestable (the original waits on the beam) |
| `$005C8E` | `hud_dma` | VBlank: HUD/inventory upload on request (`VBF_HUD_DMA`) if the raster is in lines `$E1`-`$F6`; falls into `inventory_upload` | C in the `native` group, HV from the frame-loop model |
| `$00AF24` | `actor_dma` | VBlank: one actor tile DMA if the raster is in `$E000`-`$F000`, sprite table, SAT DMA, actor timers | C (sprite.c, was `#ifdef SPRITE_ACTOR_DMA_C`) in the `native` group, HV from the model |
| `$01006A` | `hblank_nop` | `rts` | C (#12) |
| `$00974C` / `$009742` | `wait_vblank` / `wait_vblanks` | VBlank wait primitives | 68k (section "What is left for #24") |

## Selecting the renderer

```
build/port/scoobyport --renderer native      # or SCOOBY_RENDERER=native for every program
build/port/scoobyport --renderer gpgx        # default: GPGX's picture
build/port/scoobyport --filter smooth|crt    # output filter (F3 cycles), see "Output"
```

`--renderer native` does two things:
1. the picture on screen (and `--screenshot`, script `shot`, F12) is the native renderer's frame;
2. it activates the opt-in HLE group **`native`** (`HLE_FUNC_GROUP`, `hle_set_group` in hle.c): with
   `--hle all` the three functions above are C. `--hle all` without the native renderer leaves them
   68k, because GPGX's picture needs the real mid-frame register writes. `+name` / `-name` work as
   usual, `--list-hle` shows `group=native`.

## Architecture

```
            emulated build (now)                               CPU-less build (#24)
  68k + C game code --writes--> GPGX VDP state              C game code --vdp_op--> native VDP state
            |                      |                                   |                  |
            |        render_line(l) hook (patch)                 frame loop: VBlank hook, |
            |                      v                             game slice, then:        v
            |       render_line_pixels(live view, band)      render_frame(scene) -> RGBA frame
            |       render_parse_sprites / _colors
            v                      v
   hblank_verb_bar (C) --band--> render_emu.c --RGBA--> present.c (filter) --> SDL
```

- `render.c` knows nothing about the emulator. It reads a `render_vdp` view: VRAM (host-endian
  words), CRAM (the VDP's 9-bit colours), VSRAM, the 24 registers, the VDP's internal **sprite
  cache** (y / size / link of every sprite: the hardware keeps them on-chip and updates them on VRAM
  writes into the sprite table; x and the attribute word are read from VRAM) and the active width
  latched for the frame.
- It is driven per line in the order the VDP works: draw line `l` (planes, window, sprites, merge)
  -> parse the sprite table for line `l+1` -> convert the line to colours. The persistent state
  (`render_ctx`: the two per-line sprite lists, the sprite-masking flag) is the VDP's own.
- **Raster mode** (`render_emu.c`, what runs now): GPGX calls the three steps at the exact moments it
  renders, parses and remaps (`gpgx-render.patch` adds `scooby_render_line_cb`,
  `scooby_parse_satb_cb`, `scooby_remap_line_cb`; they change no emulator state). The view points at
  GPGX's live memories, so every mid-frame change (DMA during the display, a CRAM write in HBlank, a
  register write in the middle of the frame) shows exactly where GPGX shows it. This is why the
  output can be, and is, pixel-identical to GPGX on every frame.
- **Frame mode** (`render_frame`): one call per frame from a `render_scene` = a VDP snapshot plus the
  band. This is the contract for the CPU-less build, where nothing writes the VDP during the display:
  the frame loop runs the game slice and the VBlank hook, then draws. `SCOOBY_RENDERCMP_FRAME=1`
  measures how often the emulated game's mid-frame writes make a difference (see "Measurements").

GPGX still renders too (its status bits - sprite overflow / collision - and its frame are part of the
emulated machine). Comparing both costs ~0.1 ms per frame.

## Pixel pipeline (render.c)

Per line, as the VDP does it (and GPGX bit for bit):
- planes B and A in 2-cell columns from the h-scroll table entry of the line (full / cell / line
  modes), v-scroll full-screen or per 2-cell column (the partly shown left column uses 0 in H32, the
  AND of the last two VSRAM words in H40), name table sizes from register 16;
- the window: whole line when the line is on the window's side of register 18, otherwise the
  horizontal split of register 17 (left or right of a column), including the VDP's "window bug" (the
  scrolled-in column of plane A next to a left window is fetched one column further right);
- sprites from the per-line list (16 / 20 per line, 256 / 320 pixels per line including off-screen
  ones, the link list, at most 64 / 80 entries walked), front to back; sprite masking by x = 0 after
  a sprite with x > 0 (or after a full previous line);
- merge: pixels are `IPpp cccc` bytes (render.c header); lookup tables built from the priority rules,
  including shadow/highlight (register 12 bit 3: the plane priority decides normal/shadow, palette 3
  colours 14/15 of sprites are the highlight/shadow operators, colour 14 is never shadowed);
- left column blanking (register 0 bit 5), display off (register 1 bit 6: backdrop);
- colours: 9-bit CRAM -> 4-bit channels (shadow c, normal 2c, highlight c+7; register 0 bit 2 clear
  keeps one bit per channel) -> 8 bits as c4 * 17. Comparisons convert to GPGX's RGB565 exactly.

Not implemented because the game never uses them: interlace (`im2`), modes 4 and below, 128 KiB VRAM,
the "enhanced vscroll" GPGX option.

## Bands: the verb-bar split without a raster effect

In gameplay the HBlank interrupt fires at line 163 (register 10 = `$A3`); `hblank_verb_bar` spins
until line 167, H `$50`, and then writes:

| write | meaning | band field |
|---|---|---|
| `$921C` | window register = `$1C` (whole screen is window while the rest is written) | (transient) |
| VSRAM 0, 1 = 0 | v-scroll of planes A and B | `vscroll_a/b = 0` |
| `$8228`, `$8405` | planes A and B both at `$A000` (the verb/inventory bar) | `reg_plane_a = $28`, `reg_plane_b = $05` |
| VRAM `$DC00`, `$DC02` = `ram_hscroll_verb` | full-screen h-scroll of both planes | `hscroll_a/b` |
| `$9200 \| ram_verb_bar_window` | window register | `reg_window_v` |

GPGX draws line 168 with the new values (the writes land in line 167 after its render). `vblank_main`
undoes the split at the next VBlank (register 2/4/5 writes, then `camera_update` rewrites `$DC00` and
VSRAM).

The native model is a `render_band`: from `line` = 168 to the end of the frame the renderer uses those
register / scroll values instead of the VDP's. `verb_bar_band()` (frameloop.c) builds it from RAM;
the C `hblank_verb_bar` records it with `render_emu_set_band()` and does nothing else (it stores the
constant `$A750` in `ram_hblank_hv`, which nothing reads, and charges 2740 cycles, the measured
average of the original's spin from the interrupt to its `rts`). The band is cleared after every
frame, so a frame whose HBlank hook is not `hblank_verb_bar` has none.

**Verification of the model**: `SCOOBY_RENDER_SPLIT=model` (the default of `rendertest.sh`) keeps the
68k `hblank_verb_bar` (so GPGX shows the real split) and records the band from the model when the 68k
routine starts (an exec hook at `$01006C`). The native renderer then draws the bottom band from the
model values, not from the registers the 68k wrote, and the frame must still be identical to GPGX's.
`RENDERTEST_SPLIT=raster` draws from the live registers instead.

Differences when the 68k split is gone (C `hblank_verb_bar`): GPGX's own picture shows the world
planes under the verb bar (it is no longer shown); the VDP keeps the world registers across the whole
frame, so a VBlank handler that does not undo the split (`vblank_light`, the mystery intro card) no
longer starts the next frame with the verb-bar planes (in the original that is a one-frame artefact
of the hook order).

## Frame pacing: the HV counter without a beam

| reader | test | frame-loop model |
|---|---|---|
| `hud_dma` `$5C8E` | V in `$E1`..`$F6`, else no upload this frame | `frameloop_hv()` = `$E100`: passes |
| `actor_dma` `$AF24` | HV in `$E000`..`$F000` (signed compare), else no tile DMA this frame | passes |
| `game_init` `$A248` | HV seeds the RNG step | unchanged, 68k (decision for #24: seed policy) |
| `hblank_verb_bar` | raster wait | gone (band) |

The model: the VBlank hook runs at the start of the vertical blank and the whole blank is available
(`FRAMELOOP_VBLANK_HV` = line `$E1`, H 0). In the original the tests fail only when the hook is
entered late (interrupts masked during a room load, a long DMA): then the upload slips by a frame,
which is a timing artefact, not game state. In the CPU-less build there is no DMA budget at all (an
upload is a copy), so "always in time" is the natural rule.

difftest: `frameloop_set_hv_live(true)` (difftest.c) makes `frameloop_hv()` read the real counter,
which the sandbox freezes on both sides, so `dt hud_dma` / `dt actor_dma` still compare with the
original. `hud_dma` ignores X (bit 7 of the HV word = the H counter, which the sandbox does not
freeze), `actor_dma` ignores D6 (the HV value); both are dead in their only caller `vblank_main`.

## Output (present.c, main.c)

The host presents the RGBA frame (the native one, or GPGX's RGB565 converted) through an output
filter; the game never sees it:
- `nearest` (default): whole-number scale, letterboxed;
- `smooth`: "sharp bilinear": a whole-number prescale on the CPU, then SDL's linear filter for the
  fractional rest, so the picture fills the window without shimmering;
- `crt`: whole-number scale with a scanline beam profile, a horizontal blend between neighbours and
  an RGB aperture mask (software; needs a scale of 2 or more).
The mouse maps through the same placement (`present_rect`), and the overlay and pointer are drawn
into the game frame before the filter.

## For #26 (widescreen) and #27 (high frame rate)

The renderer is written against `render_scene`, not against GPGX, and the per-line work takes its
inputs from a few explicit values. The planned extensions:
- **wider viewport**: `render_vdp.width` is already a parameter of every stage (line buffers are
  `RENDER_MAX_W` + margins). A widescreen scene adds a viewport rectangle in plane coordinates; planes
  wrap naturally (the name tables are 64 cells wide), sprites need their x range opened (the 0x80
  offset and the 256/320-pixel limits become scene parameters), and the room streamer has to keep
  more columns in VRAM (game side, #26).
- **interpolation**: give the scene fractional scroll values (`hscroll_a/b`, `vscroll_a/b` as 16.16
  per band) and a sprite list built by the game layer with sub-pixel positions instead of the parsed
  SAT (the actor structs have 16.16 positions: `ACTOR_X` / `ACTOR_Y`). `render_parse_sprites` is the
  only place that reads the SAT, so a scene can carry its own list. Rendering at N times the game rate
  then means rendering N scenes interpolated between two game frames.
- **bands** are the general mechanism for any mid-frame change the game makes on purpose (only the
  verb bar here); a scene can carry several.

## Measurements

`port/tools/rendertest.sh` (raster mode, every frame compared, `--hle all` and `none`):

| groups | programs | frames compared | differing |
|---|---:|---:|---:|
| mouse, menus (tours 1-3), intros (w0, w1), extra (boot screens, Lobby, verbs, inventory, pointer, TALK, password, sound test, 20k-frame random capture route) | 42 | 338,542 | 0 |
| story (6 closed-loop routes, none + all) and mouse | 28 | 141,556 | 0 |

This includes the H40 legal screen, the shadow/highlight title effects, the window-split dialogue rows,
palette fades with CRAM writes in HBlank, and every scene change of the routes.

`RENDERTEST_SPLIT=model` (bottom band from the model) on menus, intros and extra: 297,254 frames, 1,988
differ, **all of them only in line 168**: in those frames the 68k's final register `$12` write landed
after the VDP had drawn line 168, which GPGX then shows with the transient `$921C` value (the whole
line is window, i.e. black) - a beam race of the original that the band model does not reproduce on
purpose (one frame in intro_w0 differs in line 168 the other way: `ram_verb_bar_window` changed between
the interrupt at line 163, when the model reads it, and the 68k's late write). rendertest allows
exactly these line-168-only differences in model mode and nothing else.

Cost of the original split: 2743 68k cycles per frame on average (Lobby, `SCOOBY_RENDER_SPLIT=model`
prints it), now the charge of the C `hblank_verb_bar`.

Interpreted 68k after step 2 (`SCOOBY_RENDERER=native port/tools/ipprof.sh`, docs/remaining68k.md):
**1.19 %** of the original's instructions on the #24 gate routes, down from 2.85 % with the GPGX
renderer. `hblank_verb_bar` (38.7 % of the remaining work before), `actor_dma` (20.3 %) and `hud_dma`
(0.6 %) no longer execute; the nested share fell from 8.6 M to 74 K instructions.

`SCOOBY_RENDERCMP_FRAME=1` additionally renders every frame in frame mode from a snapshot taken
before line 0 and counts frames that differ from GPGX's (mid-frame VDP writes of the emulated game).
Use it with `SCOOBY_RENDER_SPLIT=model` (or the native group), otherwise every gameplay frame differs
in the verb-bar band, which the snapshot does not have.

## Verification

```
port/tools/rendertest.sh                 # story mouse menus intros extra; PASS = 0 differing frames
RENDERTEST_SPLIT=raster port/tools/rendertest.sh
SCOOBY_RENDERER=native STORYTEST_HLE=all port/tools/storytest.sh   # and hosttest, irqtest, mousetest
```
