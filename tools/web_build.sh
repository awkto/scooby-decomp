#!/bin/bash
# Build the browser version (GitLab #36, docs/web.md) into build/web/:
#   index.html, app.js, style.css (web/) + scooby.js, scooby.wasm (port/src/web/main_web.c, Emscripten).
# Needs the Emscripten SDK (emcc on PATH, or EMSDK=<emsdk dir> to source its environment) and a
# Genesis-Plus-GX checkout at ext/gpgx (or GPGX_DIR=...; see port/cmake/gpgx.cmake).
# No ROM is needed to build. When rom/baserom.md exists, the output is scanned for ROM bytes
# (tools/py/romscan.py); without it the scan is skipped (CI has no ROM, so it cannot ship one).
set -euo pipefail
cd "$(dirname "$0")/.."
EMSDK_VERSION=6.0.13            # the version CI installs (.github/workflows/pages.yml)

if ! command -v emcc >/dev/null; then
  if [ -n "${EMSDK:-}" ] && [ -f "$EMSDK/emsdk_env.sh" ]; then
    # shellcheck disable=SC1091
    . "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
  fi
fi
command -v emcc >/dev/null || { echo "web_build.sh: emcc not found (install emsdk $EMSDK_VERSION, set EMSDK)" >&2; exit 1; }
v=$(emcc -dumpversion 2>/dev/null || true)
[ "$v" = "$EMSDK_VERSION" ] || echo "web_build.sh: note: emcc $v, CI pins $EMSDK_VERSION" >&2

bdir=build/web-cmake
out=build/web
gpgx=()
[ -n "${GPGX_DIR:-}" ] && gpgx=(-DGPGX_DIR="$GPGX_DIR")
emcmake cmake -S port -B "$bdir" -DCMAKE_BUILD_TYPE=Release -DSCOOBY_DIST=ON -DGPGX_OPT=-O3 "${gpgx[@]}" >/dev/null
cmake --build "$bdir" --target scooby_web -j"${JOBS:-4}"

rm -rf "$out"
mkdir -p "$out"
cp web/index.html web/app.js web/style.css "$out/"
cp "$bdir/scooby.js" "$bdir/scooby.wasm" "$out/"
ls -l "$out"

py=.venv/bin/python
[ -x "$py" ] || py=python3
if [ -f rom/baserom.md ]; then
  $py -I tools/py/romscan.py --rom rom/baserom.md "$out"
else
  $py -I tools/py/romscan.py --if-rom --rom rom/baserom.md "$out"
fi
