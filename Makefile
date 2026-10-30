AS      := m68k-linux-gnu-as
LD      := m68k-linux-gnu-ld
ASFLAGS := -m68000 --register-prefix-optional
PY      := .venv/bin/python -I
BASEROM := rom/baserom.md
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

disasm:
	$(PY) tools/py/disasm.py

clean:
	rm -rf build

.PHONY: all disasm clean
