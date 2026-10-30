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

## Findings so far
- Developer: Illusions Gaming Company (Sunsoft/Acclaim, 1995). Hand-written 68k assembly.
- All 68k code lives in $000200–$011000 plus a small stub at $1F6D7A–$1F6F66; the rest is data.
- Pad read routine $9D3A → $FF09E0 (pad1), $FF09E1 (pad2), edge bits $FF09E6/7.
- D-pad walks Shaggy directly (no cursor): X $FF04DC, Y $FF04F4 (16.16 fixed; note: harness `.ram` dumps before 2026-10-30 were byte-swapped within words). Verb bar:
  TAKE LOOK OPEN PUSH EAT / GIVE TALK SHUT PULL USE.
- Graphics: rooms are LZSS-packed tiles + 3 LZSS name/zone maps per room, indexed from two
  mystery headers at `$031AF2`; title uses an RLE (PackBits-like) format. Details in `docs/formats.md`.

## Heavy work runs on remote VMs
Remote test machines each have the toolchain, the ROM in `/opt/scooby/rom`, and GPGX built with both patches in `/opt/scooby/ext/gpgx`.

    vm=$(tools/vm/lease my-task)          # take a free VM (waits if all busy)
    tools/vm/vmrun $vm 'make && ...'      # rsync this worktree to VM:/work/<worktree> and run there
    tools/vm/vmpull $vm runs/x/ runs/x/   # fetch results back
    tools/vm/release $vm                  # ALWAYS release when done
    tools/vm/status                       # who holds what
