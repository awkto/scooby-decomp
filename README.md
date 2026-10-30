# Scooby-Doo Mystery (Genesis) decompilation

Goal: native Linux port with real mouse control, higher resolution/widescreen.
Tracking: https://github.com/awkto/scooby-decomp

## Layout
- `rom/` – copy your ROM to `rom/baserom.md` (sha1 `ccc9542d964de5bdc1e9101620cae40dd98e7127`, not committed)
- `src/scooby.s` – generated disassembly; `make` rebuilds a byte-identical ROM
- `tools/py/disasm.py` – recursive-descent disassembler (vectors + `data/exec_starts.txt` + gap fill)
- `tools/harness/` – headless libretro harness (GPGX core built with `tools/patches/gpgx-cpuhook.patch`, `HOOK_CPU=1`)
- `data/` – runtime coverage (executed instruction starts, ROM bytes read as data), `assets.txt` (ROM graphics map)
- `tools/py/gfx.py` – LZSS/RLE decompressors, tile/palette dumps, room + title screen reconstruction (see `docs/formats.md`)
- `tools/patches/gpgx-vdptrace.patch` – optional core patch for the harness `trace`/`vdp` commands

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
- D-pad walks Shaggy directly (no cursor): X $FF04DC, Y $FF04F4 (16.16 fixed). Verb bar:
  TAKE LOOK OPEN PUSH EAT / GIVE TALK SHUT PULL USE.
- Graphics: rooms are LZSS-packed tiles + 3 LZSS name/zone maps per room, indexed from two
  mystery headers at `$031AF2`; title uses an RLE (PackBits-like) format. Details in `docs/formats.md`.
