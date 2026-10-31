#!/bin/sh
# nativewrap.sh PROG ARGS... - run a gate's program on the native runner (GitLab #24, docs/native.md).
# The gates call build/port/scoobyport (SCOOBY_PORT) and build/port/frameprof (SCOOBY_FRAMEPROF); pointing
# those at this wrapper runs every invocation with C replacements on the native runner, and the --hle none
# invocations (the reference side of a gate) on the emulated 68k as before:
#   SCOOBY_PORT="port/tools/nativewrap.sh scoobyport" SCOOBY_FRAMEPROF="port/tools/nativewrap.sh frameprof" port/tools/hosttest.sh
# NATIVEWRAP=p3 (default): the builds without a 68k core (build/port/scooby, build/port/frameprof_native);
# NATIVEWRAP=p2: the same programs with the core linked but never stepped (SCOOBY_NATIVE=1).
prog=$1
shift
root=$(cd "$(dirname "$0")/../.." && pwd)
for a in "$@"; do
	case "$prev" in --hle) case "$a" in none|off) exec "$root/build/port/$prog" "$@" ;; esac ;; esac
	prev=$a
done
if [ "${NATIVEWRAP:-p3}" = p2 ]; then
	SCOOBY_NATIVE=1 exec "$root/build/port/$prog" "$@"
fi
case $prog in
scoobyport) exec "$root/build/port/scooby" "$@" ;;
*) exec "$root/build/port/${prog}_native" "$@" ;;
esac
