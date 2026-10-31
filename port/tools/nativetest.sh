#!/bin/sh
# The native build gate (GitLab #24, docs/native.md):   port/tools/nativetest.sh     (on a VM, after building build/port)
# 1. scan: build/port/scooby and frameprof_native link no 68k core: no symbol of the Musashi interpreter (opcode
#    handlers m68k_op_*, its internals m68ki_*, the opcode jump table, the cycle tables, the instruction counter
#    of gpgx-native.patch), and libgpgx_core_native.a has no m68kcpu/s68kcpu object. The same scan must find the
#    core in scoobyport (so the scan itself works).
# 2. routes: every open-loop route (boot to the Lobby, both mystery intros, the extra routes) on
#    scoobyport with the core linked but never stepped (SCOOBY_NATIVE=1) and on scooby: no "no C block" stop,
#    0 instructions executed by the core, and the same machine state (RAM, VRAM, CRAM, VSRAM, VDP registers,
#    Z80 RAM: SCOOBY_STATE_LOG) at every frame in both.
# The gameplay gates on the native runner: STORYTEST_NATIVE=1 SCOOBY_NATIVE_BIN=build/port/scooby
# port/tools/storytest.sh [long], and the other gates with SCOOBY_PORT / SCOOBY_FRAMEPROF = port/tools/nativewrap.sh.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
out=runs/nativetest
rm -rf "$out"; mkdir -p "$out"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
fail=0
CORE_SYMS='^(m68k_op_|m68ki_|s68k_op_|m68k_insn_count$)|instruction_jump_table|m68ki_cycles|m68ki_exception_cycle_table'

# 1. the scan
for b in scooby frameprof_native; do
	n=$(nm "build/port/$b" | awk '{print $NF}' | grep -Ec "$CORE_SYMS")
	if [ "$n" = 0 ]; then echo "PASS scan $b: no 68k core symbol"; else echo "FAIL scan $b: $n 68k core symbols"; fail=1; fi
done
objs=$(ar t build/port/libgpgx_core_native.a | grep -Ec '^[ms]68kcpu\.c\.o$')
if [ "$objs" = 0 ]; then echo "PASS scan libgpgx_core_native.a: no m68kcpu/s68kcpu object"; else echo "FAIL libgpgx_core_native.a has the core"; fail=1; fi
n=$(nm build/port/scoobyport | awk '{print $NF}' | grep -Ec "$CORE_SYMS")
if [ "$n" -gt 1000 ]; then echo "PASS scan control: scoobyport has the core ($n symbols)"; else echo "FAIL scan control: scoobyport $n"; fail=1; fi

# 2. the routes
run() { # NAME SCRIPT
	SCOOBY_NATIVE=1 SCOOBY_STATE_LOG="$out/$1.p2.state" timeout 1800 build/port/scoobyport --hle all --script "$2" \
		--outdir "$out/work" --frames 0 >"$out/$1.p2.log" 2>&1
	e2=$?
	SCOOBY_STATE_LOG="$out/$1.p3.state" timeout 1800 build/port/scooby --hle all --script "$2" \
		--outdir "$out/work" --frames 0 >"$out/$1.p3.log" 2>&1
	e3=$?
	c2=$(sed -n 's/.*native: \([0-9]*\) C blocks run, \([0-9]*\) instructions.*/\1 \2/p' "$out/$1.p2.log")
	c3=$(sed -n 's/.*native: \([0-9]*\) C blocks run, \([0-9]*\) instructions.*/\1 \2/p' "$out/$1.p3.log")
	f=$(wc -l < "$out/$1.p3.state" 2>/dev/null || echo 0)
	if [ $e2 != 0 ] || [ $e3 != 0 ]; then
		echo "FAIL $1: exit $e2 / $e3: $(grep -h 'native:' "$out/$1.p2.log" "$out/$1.p3.log" | head -2)"; fail=1
	elif [ "${c2#* }" != 0 ] || [ -z "$c2" ]; then
		echo "FAIL $1: the core executed instructions ($c2)"; fail=1
	elif ! cmp -s "$out/$1.p2.state" "$out/$1.p3.state" || [ "$f" -lt 100 ]; then
		echo "FAIL $1: machine state differs between core-linked and core-less ($(cmp "$out/$1.p2.state" "$out/$1.p3.state" | head -1))"; fail=1
	else
		echo "PASS $1: $f frames, ${c3% *} C blocks, 0 68k instructions, same state every frame with and without the core"
	fi
}
mkdir -p "$out/work"
{ cat port/scripts/lobby.txt; } > "$out/lobby.txt"
run lobby "$out/lobby.txt"
run intro_w0 port/scripts/script/intro_w0.txt
run intro_w1 port/scripts/script/intro_w1.txt
for r in boot_screens verbs inventory_play pointer_play script_talk password_screen sound_test capture; do
	run "extra_$r" "port/scripts/$r.txt"
done
exit $fail
