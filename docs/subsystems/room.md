# Subsystem: room / scene loader and room data (GitLab #18)

Code: `port/src/game/room.c`. Data formats: `docs/formats.md` ("Rooms"). Dump: `tools/py/gfx.py roomdump`.
Test driver: `port/tools/dt-room` (captured states plus synthetic room variants, see "Testing").

The LZSS / RLE decompressors are C too (`decomp.c`, `docs/subsystems/decomp.md`); `room.c` calls
`lzss_decompress` directly. The VDP write helpers come from `vdp.c`.

## Functions

| Address | C name | 68k label (data/symbols.txt) | What it does | Status |
|---|---|---|---|---|
| `$00758A` | `room_load` | `room_load` | Loads the current room: clears VRAM, uploads tiles, fonts and UI tiles, builds the plane/zone RAM copies, finds the post-zone tables, places Shaggy, camera, first draw | C, difftest |
| `$007950` | `room_draw_view` | `room_draw_view` | Writes the 32 x 19 cell camera window to plane B / A, runs the scroll helpers | C, difftest |
| `$007A2A` | `font_tiles_load` | `font_tiles_load` | Recolours the 94-tile text font and writes it to VRAM (11 callers) | C, exact registers and flags |
| `$007A8C` | `room_setup_actors` | `room_setup_actors` | Assigns actor slots 2..5 from the objects of the room | C, difftest |
| `$007F22` | `game_state_init` | `game_state_init` | Copies the mystery's initial game state, reads the start room | C |
| `$007F66` | `object_table_init` | `object_table_init` | Expands the header's object init records into `$FF1200` | C |
| `$00750A` | `pad_direction` | `pad_direction` | D-pad to direction index 0..7 | C, exact registers and flags |
| `$0073D6`, `$007476` | `zone_probe_cursor`, `zone_probe_point` | same | Pixel-exact collision probe through the zone map | C, exact registers and flags |
| `$007D54` | `overlay_apply` | `overlay_apply` | Draws / restores a tile overlay (plane copy + VRAM) | C |
| `$007C04` | `objects_update` | `objects_update` | Per-frame object timers and overlay refresh | C |

Callers of the loader (not ported, other subsystems): `$002138` `room_enter`, `$002196` `room_change`,
`$00219A` `room_fade_in`, `$00221A` / `$002278` (run the load / arrival script), script opcodes
`$11`/`$18`. Callees that stay in 68k: `$005D1C` / `$005CB6` (inventory icon and verb bar VRAM
areas), `$0047AC` (verb bar text), `$00FC3A`/`$00FD5E`/`$00FE74`/`$00FF68` (scroll column helpers), `$0099F6`
(VRAM DMA), `$0013BA` (mystery intro), `$009776` (sound stop).

Where these are not "exact" the C header comment states the proof that the registers / flags are dead for the
caller, and the `dt-room` script passes `--ignore-regs` / `--ignore-flags` for them. This applies to
`room_load` (only caller `$002138` saves and restores all registers and loads SR itself), `room_draw_view`
(callers: `room_load` and the `movem`-protected restore at `$0046xx`), `room_setup_actors` (only caller `room_load`),
`objects_update` (called from the VBlank handler, which restores everything), `overlay_apply` (flags only).

## Load sequencing in one picture

```
script_op_11/18 -> room_change $002196
  room_enter $002138
    SR=$2700, pad=$FF
    room_load $00758A
       clear VRAM -> stage palettes (FF0778 = shared 0-1 + room 2-3) -> flag objects of the room
       tiles (LZSS) -> VRAM 0 ; text font -> after the tiles ; UI font ; UI tile blocks ; blank/solid tile
       window plane $D000 ; verb bar names $AA80
       $005D1C $005CB6 [$0047AC]            68k, DMA of inventory/verb-bar areas
       map pack: plane A, plane B (+pristine copies), zone map ; post-zone tables
       Shaggy to entry point, camera, room_draw_view (CPU writes to $E000/$C000), room_setup_actors
    SR=$2000 ; $005B92 ; $00B0B8 sprite tile uploads until $FF0AB6 clears (one burst per frame)
    load script  $00221A  (room entry +$0C)         screen still dark
  room_fade_in $00219A
    palette_fade $009E7E : CRAM -> FF0778 in 8 steps (VBlank handler DMAs FF06F8 to CRAM each frame)
    arrival script $002278 (room entry +$10)
```

Notes:

- `room_load` runs with interrupts masked and uses only CPU writes through the data port (`$C00004`/`$C00000`).
  The only DMA in the whole sequence is from `$005CB6`, `$005D1C` and the sprite uploads.
- The camera is always a multiple of 8 pixels after a load. Scrolling later is done by the main loop
  with `$00FC3A`... (not part of this subsystem).
- Oddities kept bit for bit: the plane A pristine copy copies a constant 0x1800 words, UI tile block B (0xC0
  words) advances the VRAM pointer by only 0xC0 bytes, and a dead store `move.w d0,($FFFFFE).l` after the font load.

## Testing

Only 6 room loads occur on the standard capture route (`port/scripts/capture.txt`), so `difftest` got a
`--poke` option: every captured entry state is run once per variant after patching work RAM
(`--poke W:FF06AC=5,L:FF068E=0x0FCA0C,...`). `port/tools/dt-room` loads each of the 51 rooms of both
mysteries from each captured state, with different entry points and mode bits (306 comparisons, all equal).
`object_table_init` cannot be run to its `rts` in the sandbox (it ends in the VBlank-waiting mystery intro), so it
`hle_jump`s into the 68k code at the `bsr $0013BA` and `dt-room` compares the object table built at boot between a pure-emulation run and a
run with the C function (identical, 1116 non-zero bytes). Results per function: see the table in `docs/porting.md`.
`hosttest.sh`: Lobby with HLE off is 100 % identical to the harness; with HLE on 98.99 % (differences are
Shaggy/Scooby positions only).

Known gaps: the per-row table, the spawn point kind/facing bits, object fields `+4`/`+8`/`+0x16`, the `+0x28` header table
beyond script/name offsets and the contents of the `hdr+0x18` icon records are described but not fully decoded;
`$005D1C`, `$005CB6`, `$0047AC` and the scroll helpers `$00FC3A`... are still 68k.
