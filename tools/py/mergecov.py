#!/usr/bin/env python3
"""Merge harness coverage (cov.bin) into data/exec_starts.txt and data/rom_data_reads.txt.

    mergecov.py [--dry] [--acc OUT.bin] COV_OR_DIR...

Each argument is a cov.bin file or a directory searched recursively for cov.bin.
cov.bin: one byte per ROM byte; bit0 = instruction start executed, bit1 = read as data by the 68k.
The existing text files are unioned with the new coverage and rewritten (sorted; data reads as
merged [start,end) ranges).  --acc also writes the OR of the given cov.bin files (+ existing text
data) to OUT.bin so partial unions can be accumulated on the VM and merged once.
"""
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXEC = os.path.join(ROOT, "data", "exec_starts.txt")
READS = os.path.join(ROOT, "data", "rom_data_reads.txt")
ROMSIZE = 0x400000


def load_text(size):
    cov = bytearray(size)
    ex_hdr, rd_hdr = [], []
    if os.path.exists(EXEC):
        for line in open(EXEC):
            if line.startswith("#"):
                ex_hdr.append(line.rstrip("\n"))
            elif line.strip():
                cov[int(line, 16)] |= 1
    if os.path.exists(READS):
        for line in open(READS):
            if line.startswith("#"):
                rd_hdr.append(line.rstrip("\n"))
            elif line.strip():
                a, b = (int(x, 16) for x in line.split())
                for i in range(a, b):
                    cov[i] |= 2
    return cov, ex_hdr, rd_hdr


def find_covs(args):
    out = []
    for a in args:
        if os.path.isdir(a):
            for dp, _, fs in os.walk(a):
                if "cov.bin" in fs:
                    out.append(os.path.join(dp, "cov.bin"))
        else:
            out.append(a)
    return sorted(out)


def write_text(cov, ex_hdr, rd_hdr):
    n = len(cov)
    with open(EXEC, "w") as f:
        for h in ex_hdr:
            f.write(h + "\n")
        for i in range(n):
            if cov[i] & 1 and i >= 0x200:   # below $200 is the vector table; reset fetch artefacts
                f.write(f"{i:06x}\n")
    with open(READS, "w") as f:
        for h in rd_hdr:
            f.write(h + "\n")
        i = 0
        while i < n:
            if cov[i] & 2:
                j = i
                while j < n and cov[j] & 2:
                    j += 1
                f.write(f"{i:06x} {j:06x}\n")
                i = j
            else:
                i += 1


def main():
    args = sys.argv[1:]
    dry = "--dry" in args
    acc = None
    if "--acc" in args:
        k = args.index("--acc")
        acc = args[k + 1]
        del args[k:k + 2]
    args = [a for a in args if a != "--dry"]
    files = find_covs(args)
    if not files:
        sys.exit("no cov.bin found")
    first = open(files[0], "rb").read()
    size = len(first)
    cov, ex_hdr, rd_hdr = load_text(size)
    e0 = sum(1 for b in cov if b & 1)
    r0 = sum(1 for b in cov if b & 2)
    mask = int.from_bytes(b"\x03" * size, "big")
    new = int.from_bytes(bytes(cov), "big")
    for p in files:
        d = open(p, "rb").read()
        assert len(d) == size, p
        new |= int.from_bytes(d, "big") & mask
    cov = bytearray(new.to_bytes(size, "big"))
    e1 = sum(1 for b in cov if b & 1)
    r1 = sum(1 for b in cov if b & 2)
    print(f"{len(files)} cov files: exec starts {e0} -> {e1} (+{e1 - e0}); data-read bytes {r0} -> {r1} (+{r1 - r0})")
    if acc:
        open(acc, "wb").write(bytes(cov))
    if not dry:
        write_text(cov, ex_hdr, rd_hdr)


if __name__ == "__main__":
    main()
