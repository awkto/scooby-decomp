#!/bin/bash
# mk.sh : build rom + harness (+ port) on the VM
cd "$(dirname "$0")/../.."
nice make -j4 2>&1 | tail -1
[ -x build/harness ] || gcc -O2 -I ext/gpgx/libretro/libretro-common/include -o build/harness tools/harness/harness.c -ldl
ls build/harness
