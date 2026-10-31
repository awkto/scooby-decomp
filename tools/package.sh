#!/bin/sh
# Release tarball for Linux (GitLab #28):  tools/package.sh [OUTDIR]     (or: make dist)
#   builds the SDL2 host (the native build `scooby`, no 68k core) in a clean Release tree (-DSCOOBY_DIST=ON), lays out scooby-VERSION-linux-x86_64/ with
#   the binary (renamed `scooby`), README.txt, LICENSES/ and source/, tars it, and runs tools/py/romscan.py over
#   the result.  The build needs cmake, a C compiler, libsdl2-dev and the ext/gpgx checkout (README Setup);
#   the scan needs the ROM (rom/baserom.md) and refuses to pass without it.  Run it on a build machine.
# Output: OUTDIR/scooby-VERSION-linux-x86_64.tar.gz  (default build/dist)
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
out=${1:-build/dist}
ver=${SCOOBY_VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}
name=scooby-$ver-linux-x86_64
stage=$out/$name
rom=${ROM:-rom/baserom.md}
[ -f "$rom" ] || { echo "package.sh: $rom missing - needed by romscan to certify the package" >&2; exit 1; }
[ -f ext/gpgx/LICENSE.txt ] || { echo "package.sh: ext/gpgx checkout missing (README Setup)" >&2; exit 1; }

rm -rf "$stage" "$out/$name.tar.gz"
mkdir -p "$stage/LICENSES" "$stage/source" "$out"
cmake -S port -B "$out/cmake" -DCMAKE_BUILD_TYPE=Release -DSCOOBY_DIST=ON >/dev/null
# the native build (#24, docs/native.md): no 68k core is linked; refuse to ship one that has it
cmake --build "$out/cmake" -j"$(nproc)" --target scooby
if nm "$out/cmake/scooby" | awk '{print $NF}' | grep -Eq '^(m68k_op_|m68ki_)|instruction_jump_table'; then
	echo "package.sh: $out/cmake/scooby links a 68k core" >&2; exit 1; fi
install -m755 "$out/cmake/scooby" "$stage/scooby"
strip "$stage/scooby"
cp dist/README.txt "$stage/README.txt"
cp ext/gpgx/LICENSE.txt "$stage/LICENSES/GPGX-LICENSE.txt"
cp dist/SDL2-LICENSE.txt "$stage/LICENSES/SDL2-LICENSE.txt"
cp dist/LICENSES-README.txt "$stage/LICENSES/README.txt"
# the source of the port and the patches to the core (GPGX's licence asks for it with a modified binary)
tar -c port tools/patches | tar -x -C "$stage/source"
tar -C "$out" -czf "$out/$name.tar.gz" "$name"
echo "package.sh: $out/$name.tar.gz ($(du -h "$out/$name.tar.gz" | cut -f1))"
if [ -x .venv/bin/python ]; then py=".venv/bin/python -I"; else py="python3 -I"; fi
$py tools/py/romscan.py --rom "$rom" "$out/$name.tar.gz"
