#!/bin/bash
# vmjob.sh ARGS...  (on the VM) build the harness if needed, then run tools/py/campaign.py ARGS
set -e
mkdir -p build
if [ ! -x build/harness ] || [ tools/harness/harness.c -nt build/harness ]; then
  gcc -O2 -I ext/gpgx/libretro/libretro-common/include -o build/harness tools/harness/harness.c -ldl
fi
exec python3 tools/py/campaign.py "$@"
