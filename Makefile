AS      := m68k-linux-gnu-as
LD      := m68k-linux-gnu-ld
ASFLAGS := -m68000 --register-prefix-optional
PY      := .venv/bin/python -I
BASEROM := rom/baserom.md
Z80ASM  := z80asm
Z80_OFF := 0x1BB94A
Z80_LEN := 0x1886
SHA1    := ccc9542d964de5bdc1e9101620cae40dd98e7127

all: build/scooby.md
	@echo "$(SHA1)  build/scooby.md" | sha1sum -c -

build/baserom.md: $(BASEROM)
	@mkdir -p build
	cp $(BASEROM) $@

build/scooby.o: src/scooby.s build/baserom.md
	$(AS) $(ASFLAGS) -I build -o $@ $<

build/scooby.md: build/scooby.o
	$(LD) -e 0 -Ttext=0 --oformat=binary -o $@ $<

# Z80 sound driver (blob at ROM $$1BB94A, $$1886 bytes): assemble src/z80/driver.s
# with z80asm (apt install z80asm) and compare byte-for-byte with the ROM.
Z80_TABS := $(addprefix build/z80/,$(shell grep -o 'tab_[0-9A-F]*_[0-9A-F]*\.bin' src/z80/driver.s))

# data tables inside the driver blob: sliced from the base ROM (name = ROM offset hex _ length hex)
build/z80/tab_%.bin: $(BASEROM)
	@mkdir -p build/z80
	@n=$*; dd if=$(BASEROM) bs=1 skip=$$((0x$${n%_*})) count=$$((0x$${n#*_})) of=$@ 2>/dev/null

build/z80/driver.bin: src/z80/driver.s $(Z80_TABS)
	@mkdir -p build/z80
	$(Z80ASM) -I build/z80 -o $@ $<

z80check: build/z80/driver.bin
	@dd if=$(BASEROM) bs=1 skip=$$(($(Z80_OFF))) count=$$(($(Z80_LEN))) 2>/dev/null | cmp - build/z80/driver.bin \
	  && echo "build/z80/driver.bin: OK ($$(($(Z80_LEN))) bytes match ROM)"

z80disasm:
	$(PY) tools/py/z80dis.py $(BASEROM) src/z80/driver.s

disasm:
	$(PY) tools/py/disasm.py

# Regenerate src/scooby.s + data/{functions,callgraph,xrefs}.txt (run after data/exec_starts.txt,
# data/symbols.txt or data/ram.txt change), then check every executed insn is in exactly one function.
functions: disasm
	$(PY) tools/py/symtab.py

# Linux release tarball (binary, README, LICENSES, source) + romscan certificate; run on a VM: see tools/package.sh
dist:
	tools/package.sh

clean:
	rm -rf build

.PHONY: all dist disasm functions clean z80check z80disasm
