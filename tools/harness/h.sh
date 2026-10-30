#!/bin/bash
# h.sh SCRIPT OUTDIR   — run the harness on a script (run on the VM, from the worktree root)
exec build/harness ext/gpgx/genesis_plus_gx_libretro.so rom/baserom.md "$1" "$2"
