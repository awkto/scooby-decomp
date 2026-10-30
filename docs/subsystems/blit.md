# Scaled blits and small VBlank pieces (GitLab #24 follow-up)

Found by the interpreted-instruction profile (`docs/remaining68k.md`): two functions were 88 % of all
68k instructions still interpreted with `--hle all`. Format: `docs/formats.md`, "Scaled bitmap blits".

| addr | name | what | status |
|---|---|---|---|
| `$00E40C` | `scaled_blit_120x40` (`sub_00E40C`) | shrink the 120x40 chunky picture at `$FF3800` to 600 tile longs at `ZONE_MAP` | C, blit.c, dt-menus 200/200 (+200 poked variants) |
| `$00F272` | `scaled_blit_64x32` (`sub_00F272`) | same, 64x32 at `$FF5000` -> 256 longs at `PLANEA_NAMES` | C, blit.c, 200/200 (+200 variants) |
| `$00A704` | `inventory_arrows_draw` (label `meters_draw`) | the inventory up/down arrows (2x3 name words each, frames `$FF0804/6`, timers `$FF0AB1/2`) | C, hud.c, 200/200 |
| `$00FA78` | `camera_update` | camera -> h/v-scroll words, pan, screen shake, column/row streaming requests, VDP scroll writes | C, camera.c, 200/200 (+200 variants) |
| `$00A7CE` | `palette_cycle` | rotate the colours of due cycling slots, DMA to CRAM | C, palcycle.c, 200/200 (+80 variants) |
| `$00AFDA`, `$00B0A6` | `run_object_events_setup`, `_done` | the register/frame save and restore around the object-event walk (`HLE_EXIT_SP` +-66) | C, script.c, 200/200 each |

The blits and the camera/palette/arrows routines are called nested from C (`hle_call68k`), so the 68k
never has to run them; the exemptions in `difftest.c` ("scaled blits", `camera_update`,
`palette_cycle`) only cover scratch registers that no caller reads (proofs in the C sources).
Cycle charges: the blit has a per-row/per-pixel model of the original's 68000 timing (+1 % measured)
so a deep or shallow zoom costs what the original did (37 k to 331 k cycles).
