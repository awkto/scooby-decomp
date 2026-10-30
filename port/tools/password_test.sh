#!/bin/sh
# Password codec test (GitLab #21): C (port/src/game/password.c) vs tools/py/password.py on
# many random states.  Run on a VM (or anywhere with cmake + a C compiler):
#   port/tools/password_test.sh [COUNT]
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
[ -f build/port/CMakeCache.txt ] || cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/port -j"$(nproc)" --target password_test >/dev/null || exit 1
PY=.venv/bin/python; [ -x $PY ] || PY=/opt/venv/bin/python
mkdir -p runs/password
$PY -I - "${1:-5000}" > runs/password/vectors.txt <<'P'
import random, sys
sys.path.insert(0, "tools/py")
import password as pw
r = random.Random(21)
n = int(sys.argv[1])
for i in range(n):
    room = r.randrange(32)
    kind = i % 4
    if kind == 0: s = [r.randrange(256) for _ in range(29)]
    elif kind == 1: s = [0] * 29
    elif kind == 2: s = [255] * 29
    else: s = [r.choice((0, 255, r.randrange(256))) for _ in range(29)]
    p = pw.encode(room, s)
    print(room, bytes(s).hex(), p)
    if i % 5 == 0:
        # alter one symbol: usually a wrong checksum (skipped when it still decodes)
        k = r.randrange(50); c = r.choice([c for c in pw.ALPHA if c != p[k]])
        q = p[:k] + c + p[k + 1:]
        if not pw.decode(q)[2]:
            print(room, bytes(s).hex(), q, "bad")
P
build/port/password_test runs/password/vectors.txt
