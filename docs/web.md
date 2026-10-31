# The browser build (GitLab #36)

The native program `scooby` (docs/native.md: the game's C, no 68k core) compiled to WebAssembly with
Emscripten and SDL2 (Emscripten's port). It is a static site: `index.html`, `app.js`, `style.css`
(`web/`) and `scooby.js` + `scooby.wasm` (`port/src/web/main_web.c` with the same game, HLE, renderer
and GPGX machine code as the desktop build). It holds no game data: the player provides the ROM.

## Building

    EMSDK=<emsdk dir> tools/web_build.sh        # -> build/web/
    cd build/web && python3 -m http.server 8000

`tools/web_build.sh` runs `emcmake cmake -S port -B build/web-cmake -DSCOOBY_DIST=ON -DGPGX_OPT=-O3`
and builds the `scooby_web` target only (the CMake project returns after it under Emscripten; the
desktop programs and tools are not configured). Emscripten 6.0.13 is the pinned version (CI installs
it; the script notes a different one). No ROM is needed to build. With `rom/baserom.md` present the
script runs `tools/py/romscan.py` over `build/web/`; without it, the secret-free check (below).

Link settings (`port/CMakeLists.txt`): `-O3`, `-sUSE_SDL=2`, `-sINVOKE_RUN=0` (the page calls
`main` once it has the ROM), `-sEXIT_RUNTIME=0`, memory growth from 64 MB, a 1 MB stack, IDBFS.
**No ASYNCIFY**: the desktop loop blocks (it sleeps for pacing and waits on the audio queue); the
browser build has no blocking loop at all, so nothing needs to be unwound. Asyncify would cost code
size and speed for nothing.

## The main loop

`emscripten_set_main_loop(web_frame, 0, 1)`: the browser calls `web_frame` on every
`requestAnimationFrame`, i.e. once per display refresh.

- **Logic** keeps its own clock at 59.92 Hz: an accumulator of real time (`emscripten_get_now`) runs
  as many logic frames as are due (usually 0 or 1, at most 4 to catch up; a gap over 250 ms, e.g. a
  hidden tab, restarts the clock instead of fast-forwarding). Tab (held) runs 4x.
- **Presentation** happens every rAF. Over 60 Hz (the rAF rate is measured over 120 frames and again
  every 120 frames) the high frame rate path (#27, docs/subsystems/hifps.md) is turned on with 16
  steps per logic frame, and each rAF draws the in-between picture nearest the accumulator's phase
  (`hifps_frame(j, 16)` with j = phase x 16 + 1; j = 16 is the logic frame itself). As on the
  desktop, the span R(k-1) -> R(k) is shown during the frame after k, and frames that the
  interpolator cannot model are held. At 60 Hz it is off and every rAF shows the latest logic frame.
  F4 turns it off / on (kept in the ini).
- **Audio**: SDL2's Web Audio device at the core's rate (SDL converts to the AudioContext's), queued
  per logic frame. The Web Audio clock is not the rAF clock: the queue is kept near 60 ms by moving
  the logic clock by 1/32 frame when it drifts (the same rule as the desktop hifps path). Browsers
  start an AudioContext only after a user gesture: the page resumes it on the first click, key or
  touch (and shows an "Enable sound" button while it is suspended); until then the queue is capped.

## Input and display

- Keyboard as on the desktop (the `key_*` options); the page stops the browser from acting on the
  game's keys (arrows, Tab, Enter, F1-F10). Esc does nothing (there is nothing to quit to); F11 and
  the Fullscreen button put the stage (canvas) in fullscreen.
- Mouse as on the desktop (docs/mouse.md). The canvas fills the stage in CSS; the SDL window
  follows the canvas's CSS size every frame and is created with `SDL_WINDOW_ALLOW_HIGHDPI`, so its
  drawing buffer is CSS size x devicePixelRatio. SDL reports pointer positions in CSS pixels;
  `window_to_game` scales them to the drawing buffer and through the filter's placement
  (`present_rect`), then the widescreen mapping (`ws_frame_to_game`) - the same path as the desktop.
- Gamepads: a pad SDL maps is a GameController (the desktop bindings); any other is read as the
  browser's "standard" Gamepad layout (buttons 0-3 face, 9 Start, 8 Back = pause, 12-15 D-pad,
  axes 0/1).
- Widescreen: default `auto` (the canvas's aspect, so it follows the window and fullscreen); F10
  toggles it (kept in the ini as `auto` / `4:3`). F3 filter (kept). SDL draws with WebGL; without it, with its software
  renderer.

## Persistence (IndexedDB)

`app.js` mounts IDBFS at `/scooby` before `main` and loads it (`FS.syncfs(true)`):

| path | what |
|---|---|
| `/scooby/rom/baserom.md` | the player's ROM, kept after its sha1 matched |
| `/scooby/scooby.ini` | the options (docs: `port/src/options.h`); F3/F4/F10 changes are saved |
| `/scooby/saves/` | game saves (F8, 8 slots, saveslot.c) and the quick state (F5 / F9) |

After every write (save, erase, quick state, ini) the C calls `Module.scoobySync`, which writes
IndexedDB 300 ms later (`FS.syncfs(false)`); the page also syncs when the tab is hidden.

## The ROM

On start the page tries, in order:
1. `fetch('rom/baserom.md')` beside the page: only a private deployment has one (it is never part of
   the site build). It is written to `/tmp` (memory) and not kept.
2. The copy in IndexedDB.
3. Asks: a file input and drag and drop (on the box or anywhere on the page), with the text that the
   player needs their own ROM and that it stays in the browser. The file is written to IDBFS, checked
   by `web_rom_check` (C: `rom_verify`, sha1 of the whole file) and kept on success.
"Forget ROM" (shown when the cached copy is in use) deletes it and reloads. The ROM is never sent
anywhere: the only network request the page makes besides its own files is the GET of step 1.

On a public site step 1 is a 404, which Chrome logs in the console as a failed resource load; it is
expected (the test tool knows it).

## Errors

The native runner stops with `abort()` when the game reaches an address that has no C block
(`native: no C block at $X`, docs/native.md). The page keeps the last 60 lines of stderr; `onAbort`
shows a panel instead of a frozen canvas: for "no C block" the 68k address and an explanation,
otherwise the message, with the log. A ROM refused by `emu_init` shows "Cannot start the game".

## GitHub Pages

`.github/workflows/pages.yml`: on a push to `main`, install emsdk 6.0.13, fetch Genesis Plus GX at the
pinned commit (`git fetch --depth 1` of that commit into `ext/gpgx`; `port/cmake/gpgx.cmake` then
exports and patches it as in every build), run `tools/web_build.sh`, check the site, upload it and
deploy with `actions/deploy-pages`.

**romscan in CI.** CI has no ROM, so the byte scan (every 64-byte window of the ROM against the
output) cannot run there. `romscan.py --if-rom` does what can be checked without it: no file is the
ROM (its sha1 is public) or a Genesis ROM image ("SEGA" at `$100`), so a ROM copied into the site by
mistake fails the job. That the build itself cannot carry ROM bytes follows from the build machine
not having any; the full byte scan runs wherever `tools/web_build.sh` runs with `rom/baserom.md`
(the same sources and compiler give the same output).

## Testing

`tools/web_test.js URL picker|server OUTDIR ROM` (puppeteer-core, `CHROME=` a Chrome or
chrome-headless-shell binary): picker mode starts from an empty profile, gives the ROM through the
file input, waits for the title, presses Start along the harness route to the Lobby, walks, measures
the logic frame rate, then reloads and expects the cached ROM; server mode expects the page to
start by itself from `rom/baserom.md`. Screenshots go to OUTDIR; console errors fail the run.
