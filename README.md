# Scooby-Doo Mystery (Genesis) decompilation

Goal: native Linux port with real mouse control, higher resolution/widescreen.
Tracking: https://github.com/awkto/scooby-decomp

## Layout
- `rom/` – copy your ROM to `rom/baserom.md` (sha1 `ccc9542d964de5bdc1e9101620cae40dd98e7127`, not committed)
- `src/scooby.s` – generated disassembly; `make` rebuilds a byte-identical ROM
- `tools/py/disasm.py` – recursive-descent disassembler (vectors + `data/exec_starts.txt` + gap fill + indirect-jump resolution in `tools/py/indirect.py`; extra seeds in `tools/py/seeds.txt`)
- `tools/harness/` – headless libretro harness (GPGX core built with `tools/patches/gpgx-cpuhook.patch`, `HOOK_CPU=1`)
- `data/` – runtime coverage (executed instruction starts, ROM bytes read as data); generated `indirect.txt` (indirect jmp/jsr sites → targets) and `code_gaps.txt` (classified non-code ranges in the code area, manual notes in `gap_notes.txt`); `assets.txt` (ROM graphics map)
- Symbols and functions: hand-edited `data/symbols.txt` (ROM labels) and `data/ram.txt` (RAM/I/O vars, emitted as `.equ`) feed `disasm.py`; it generates `data/functions.txt` (every traced insn in exactly one function), `data/callgraph.txt` and `data/xrefs.txt`. `make functions` regenerates all of it and checks coverage of `data/exec_starts.txt`. Query with `.venv/bin/python -I tools/py/xref.py ADDR|NAME` (callers of a function, readers/writers of a RAM var) or `--top`.
- `tools/py/gfx.py` – LZSS/RLE decompressors, tile/palette dumps, room + title screen reconstruction (see `docs/formats.md`)
- `tools/patches/gpgx-vdptrace.patch` – optional core patch for the harness `trace`/`vdp` commands
- `docs/sound.md`, `src/z80/driver.s` – sound driver (`make z80check`)

## Setup
    sudo apt install binutils-m68k-linux-gnu
    python3 -m venv .venv && .venv/bin/pip install capstone
    git clone https://github.com/libretro/Genesis-Plus-GX ext/gpgx
    (cd ext/gpgx && git apply ../../tools/patches/gpgx-cpuhook.patch && make -f Makefile.libretro HOOK_CPU=1)
    gcc -O2 -I ext/gpgx/libretro/libretro-common/include -o tools/harness/harness tools/harness/harness.c -ldl
    make disasm && make

## Play it (native Linux port)
You need your own ROM (`Scooby-Doo Mystery (USA)`, sha1 above; never included) and SDL2.

    # build (Linux; apt install build-essential cmake libsdl2-dev; GPGX checkout in ext/gpgx, see Setup)
    cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release && cmake --build build/port -j4
    build/port/scoobyport                    # finds the ROM, remembers it (see below)
    make dist                                # or tools/package.sh: release tarball with README + LICENSES (on a VM)

Or unpack a release tarball and run `./scooby`; its `README.txt` is the player's guide (same content as here).

**ROM**: `--rom FILE`, else the `rom` entry of the ini, else a ROM file next to the binary (found by sha1),
else `rom/baserom.md` in the current folder, else a zenity/kdialog file picker, else a window to drop the file
on. The ROM that worked is remembered in the ini. A file with the wrong sha1 is refused.

**Controls**: arrows = D-pad, Z/A = A, X/S = B, C/D = C, Enter = Start (rebindable); gamepad supported;
mouse: left click walks / uses the verb, right click looks, middle = Start, wheel = inventory (`docs/mouse.md`).
F8 game saves, F5/F9 quick states, F3 filter, F11 fullscreen, P pause, N step, Tab fast-forward, F12 screenshot, Esc quit.

**Game saves** (instead of typing 50-letter passwords): F8 opens 8 slots (mouse or keyboard); `--load-slot N`
and `--list-slots` on the command line. A slot is the game's own password state (start room + 29 story bytes,
`docs/passwords.md`) plus mystery, room name, date and play time, in `~/.local/share/scooby-decomp/slotN.sav`
(text; contains the password). Loading presses the game's own buttons: reset, title, mystery menu, CONTINUE,
password, END - so the result is exactly the password-entered game (`port/tools/slottest.sh` proves it by
comparing against a hand-typed password at 8 points of each full walkthrough). The mystery's intro cut scene
plays first, as after typing a password (Start skips it).

**Options**: `~/.config/scooby-decomp/scooby.ini` (written when the ROM is first remembered), overridden per
run with `--options scale=4,filter=crt,volume=60,key_a=Z|Space`; `--options --json` lists them all with
defaults. Picture/sound/controls: filter, scale, fullscreen, vsync, renderer, mouse, mute, volume, key_*.
Options that change the game itself would default to off; there are none yet.

**Release hygiene**: `tools/py/romscan.py` proves no ROM bytes are shipped (rolling hash of every 64-byte ROM
window vs every byte of every file; a shared run of 256+ bytes fails): `romscan.py TARBALL` and
`romscan.py --tracked` (the whole repository). `make dist` runs it on the tarball; `port/tools/disttest.sh`
unpacks the tarball and checks it runs, refuses a modified ROM, remembers the ROM, and is clean.
Sign-off of what a human actually played: `docs/USER_VERIFIED.md`.

## Native port internals (`port/`)
`scoobyport` is an SDL2 host with the GPGX core compiled in statically. It refuses a ROM
with the wrong sha1. It supports keyboard and gamepad input, integer scaling, F5/F9
savestates, frame advance and a debug overlay. C replacements of 68k functions plug in
through the HLE layer (`port/hle/hle.h`), and `difftest` proves each one equivalent to the
original on 200 captured entry states.

    cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release && cmake --build build/port -j4   # on a VM
    build/port/scoobyport                      # play (rom/baserom.md)
    port/tools/dt pad_read                     # capture states if needed + difftest one function

See `docs/porting.md` for the API, the controls, and the step-by-step workflow for
decompiling a function.

## Findings so far
- Developer: Illusions Gaming Company (Sunsoft/Acclaim, 1995). Hand-written 68k assembly.
- All 68k code lives in $000200–$011000 plus a small stub at $1F6D7A–$1F6F66; the rest is data.
- Pad read routine $9D3A → $FF09E0 (pad1), $FF09E1 (pad2), edge bits $FF09E6/7.
- D-pad walks Shaggy directly (no cursor): X $FF04DC, Y $FF04F4 (16.16 fixed; note: harness `.ram` dumps before 2026-10-30 were byte-swapped within words). Verb bar:
  TAKE LOOK OPEN PUSH EAT / GIVE TALK SHUT PULL USE.
- Graphics: rooms are LZSS-packed tiles + 3 LZSS name/zone maps per room, indexed from two
  mystery headers at `$031AF2`; title uses an RLE (PackBits-like) format. Details in `docs/formats.md`.

## Coverage / playthrough tooling (issue #7)
- `tools/harness/harness.c` also has `record`/`replay` (per-frame input log), `peek`/`echo` (RAM words to `peek.log`),
  `roomsave` (auto-savestate on first entry to each room); see the header comment.
- `tools/py/password.py`: codec + input generator for the CONTINUE password screen (`docs/passwords.md`) - warps to any room.
- `tools/py/campaign.py` (via `tools/py/vmjob.sh`, on a VM): `boot`, `warp` (all 51 rooms), `fuzz` (random no-pause play),
  accumulating coverage into `runs/cov_acc.bin`; `tools/py/mergecov.py` merges cov.bin files into `data/exec_starts.txt` and
  `data/rom_data_reads.txt`; `tools/py/covreport.py --md docs/coverage.md` writes the coverage report.
- `states/INDEX.md` + `tools/scripts/rooms/*.txt`: room name -> script that regenerates the savestate (states are gitignored).

## Heavy work runs on remote VMs
Remote test machines each have the toolchain, the ROM in `/opt/scooby/rom`, and GPGX built with both patches in `/opt/scooby/ext/gpgx`.

    vm=$(tools/vm/lease my-task)          # take a free VM (waits if all busy)
    tools/vm/vmrun $vm 'make && ...'      # rsync this worktree to VM:/work/<worktree> and run there
    tools/vm/vmpull $vm runs/x/ runs/x/   # fetch results back
    tools/vm/release $vm                  # ALWAYS release when done
    tools/vm/status                       # who holds what
