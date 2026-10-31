#!/usr/bin/env python3
"""romscan.py - prove that no ROM bytes are in what we ship (GitLab #28).

    romscan.py [--rom rom/baserom.md] [--min 256] [--warn 64] TARGET...
    romscan.py --tracked                  every file the repository tracks (symlinks skipped)

TARGET is a file, a directory (recursive), or a .tar/.tar.gz/.tgz (every member is scanned).

How it works.  Every 64-byte window of the ROM at an offset that is a multiple of STRIDE (8) is put in a hash table
(a polynomial rolling hash).  Every 64-byte window at *every* offset of each scanned file is hashed the same way and looked
up; a hit is verified byte for byte and then extended in both directions to the longest run shared with the ROM.  Because
any shared run of >= 64 + STRIDE - 1 bytes contains an indexed window, no run of 256 bytes can slip through.  A shared run
of at least --min bytes (default 256) is a violation (exit status 1); runs of --warn (64) up to --min are listed as notes.
Runs made of at most 4 distinct byte values (the ROM's zero / 0xFF / space padding) are not content and are only counted.

Text-encoded data.  Source files can carry ROM bytes as numbers (`.byte 0x12, 0x34`, `defb $12`, `dc.w`, `.long` ...).  A raw
byte scan cannot see those, so for every file that looks like text the numbers of the data directives are also decoded into a
byte stream (consecutive directive lines form one stream; words and longs both as big- and little-endian) and that stream
is scanned the same way.  Such hits are reported as "text".  Decoded streams shorter than 64 bytes can never match.
What it cannot see: data re-encoded in some other way (compressed, XORed, drawn as an image, hand-converted); that has to be
excluded by policy (docs/porting.md: no ROM-derived binaries in git)."""
import argparse, io, os, re, subprocess, sys, tarfile

W = 64
STRIDE = 8
MASK = (1 << 64) - 1
B = 0x100000001B3
BW = pow(B, W - 1, 1 << 64)


def window_hashes(data, step=1, start=0):
    """yield (offset, hash) of every W-byte window at offsets start, start+step, ... (rolling over all offsets)"""
    n = len(data)
    if n < W:
        return
    h = 0
    for k in range(W):
        h = (h * B + data[k]) & MASK
    i = 0
    while True:
        if i >= start and (i - start) % step == 0:
            yield i, h
        if i + W >= n:
            break
        h = ((h - data[i] * BW) * B + data[i + W]) & MASK
        i += 1


def build_index(rom):
    idx = {}
    for off, h in window_hashes(rom, STRIDE):
        idx.setdefault(h, []).append(off)
    return idx


def extend(rom, data, ro, do):
    """longest common run containing the verified W-window rom[ro:ro+W] == data[do:do+W]: (length, start in data, start in rom)"""
    a = 0
    while ro - a - 1 >= 0 and do - a - 1 >= 0 and rom[ro - a - 1] == data[do - a - 1]:
        a += 1
    b = W
    while ro + b < len(rom) and do + b < len(data) and rom[ro + b] == data[do + b]:
        b += 1
    return a + b, do - a, ro - a


def scan_bytes(rom, idx, data):
    """list of (length, offset in data, offset in rom): maximal shared runs >= W, non-overlapping"""
    found = []
    covered = -1
    for off, h in window_hashes(data):
        if off < covered:
            continue
        for ro in idx.get(h, ()):
            if rom[ro:ro + W] == data[off:off + W]:
                ln, ds, rs = extend(rom, data, ro, off)
                found.append((ln, ds, rs))
                covered = ds + ln
                break
    return found


# ---- text-encoded data -------------------------------------------------------------------------------------
DIRECTIVE = re.compile(r"^\s*(?:[A-Za-z_.][\w.]*:)?\s*(\.byte|\.word|\.long|\.short|\.half|\.int|defb|defw|dc\.b|dc\.w|dc\.l|db|dw|dd)\b\s*(.*?)\s*(?:[;#@].*)?$",
                       re.I)
NUM = re.compile(r"(?<![\w$])(?:0x([0-9a-f]+)|\$([0-9a-f]+)|([0-9a-f]+)h|(-?\d+))(?![\w])", re.I)
SIZE = {"b": 1, "w": 2, "l": 4}


def decode_text(text):
    """yield byte streams (as bytes) from data directives; words/longs both endiannesses"""
    streams = []
    cur = {"be": bytearray(), "le": bytearray()}

    def flush():
        for k in ("be", "le"):
            if len(cur[k]) >= W and not (k == "le" and cur["le"] == cur["be"]):
                streams.append(bytes(cur[k]))
            cur[k] = bytearray()
    for line in text.splitlines():
        m = DIRECTIVE.match(line)
        if not m:
            if line.strip() and not line.strip().startswith((";", "#", "//", "@")):
                flush()
            continue
        d = m.group(1).lower().lstrip(".")
        size = 1 if d in ("byte", "defb", "dc.b", "db") else 4 if d in ("long", "int", "dc.l", "dd") else 2
        for n in NUM.finditer(m.group(2)):
            g = n.groups()
            v = int(g[0], 16) if g[0] else int(g[1], 16) if g[1] else int(g[2], 16) if g[2] else int(g[3])
            v &= (1 << (8 * size)) - 1
            cur["be"] += v.to_bytes(size, "big")
            cur["le"] += v.to_bytes(size, "little")
    flush()
    return streams


def looks_text(data):
    return b"\0" not in data[:4096] and len(data) > 0


# ---- targets -----------------------------------------------------------------------------------------------
def iter_files(target):
    """yield (display name, bytes)"""
    if os.path.isdir(target):
        for root, dirs, files in os.walk(target):
            dirs.sort()
            for f in sorted(files):
                p = os.path.join(root, f)
                if os.path.islink(p) or not os.path.isfile(p):
                    continue
                yield p, open(p, "rb").read()
    elif target.endswith((".tar", ".tar.gz", ".tgz")):
        with tarfile.open(target) as t:
            for m in t.getmembers():
                if m.isfile():
                    yield f"{target}:{m.name}", t.extractfile(m).read()
    else:
        yield target, open(target, "rb").read()


def git_files():
    out = subprocess.run(["git", "ls-files", "-z"], stdout=subprocess.PIPE, check=True).stdout.split(b"\0")
    for f in out:
        if not f:
            continue
        p = f.decode()
        if os.path.islink(p) or not os.path.isfile(p):
            continue
        yield p, open(p, "rb").read()


ROM_SHA1 = "ccc9542d964de5bdc1e9101620cae40dd98e7127"


def romless_check(a):
    """Without the ROM the byte comparison is impossible.  What can still be checked without it: no file (or archive
    member) is the ROM (its sha1 is public) or any Genesis ROM image (the "SEGA" system name at $100 of a cartridge
    header).  A build machine without the ROM cannot put ROM bytes into its output in the first place; this catches a
    ROM copied in by mistake (e.g. a deployment's rom/ directory)."""
    import hashlib
    bad = nfiles = 0
    for t in a.targets:
        for name, data in iter_files(t):
            nfiles += 1
            if hashlib.sha1(data).hexdigest() == ROM_SHA1:
                bad += 1
                print(f"VIOLATION {name}: is the ROM (sha1 {ROM_SHA1})")
            elif len(data) >= 0x200 and data[0x100:0x104] == b"SEGA":
                bad += 1
                print(f"VIOLATION {name}: looks like a Genesis ROM image (\"SEGA\" at $100)")
    print(f"romscan: no ROM at {a.rom}: byte scan skipped; secret-free check of {nfiles} files -> {'FAIL' if bad else 'CLEAN'}")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("targets", nargs="*")
    ap.add_argument("--rom", default="rom/baserom.md")
    ap.add_argument("--min", type=int, default=256, help="shared run length that fails the scan")
    ap.add_argument("--warn", type=int, default=64, help="shared run length that is reported as a note")
    ap.add_argument("--tracked", action="store_true", help="scan every file the repository tracks (symlinks skipped)")
    ap.add_argument("--if-rom", action="store_true",
                    help="no ROM at --rom: do only the secret-free check (no file is a Genesis ROM image or has the ROM's sha1) "
                         "and exit 0 if it passes (CI, which has no ROM)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    if a.if_rom and not os.path.exists(a.rom):
        return romless_check(a)
    rom = open(a.rom, "rb").read()
    idx = build_index(rom)
    srcs = []
    if a.tracked:
        srcs.append(("git ls-files", git_files()))
    for t in a.targets:
        srcs.append((t, iter_files(t)))
    if not srcs:
        ap.error("nothing to scan")
    bad = notes = nfiles = nbytes = ntext = padding = 0
    for label, it in srcs:
        for name, data in it:
            nfiles += 1
            nbytes += len(data)
            hits = [("binary", h) for h in scan_bytes(rom, idx, data)]
            if looks_text(data):
                for s in decode_text(data.decode("latin-1")):
                    ntext += 1
                    hits += [("text", h) for h in scan_bytes(rom, idx, s)]
            for kind, (ln, ds, rs) in hits:
                if len(set(rom[rs:rs + ln])) <= 4:      # padding: zeros, 0xFF, spaces ... is not content
                    padding += 1
                elif ln >= a.min:
                    bad += 1
                    print(f"VIOLATION {name}: {kind} run of {ln} bytes shared with the ROM (file offset {ds}, ROM ${rs:06X})")
                elif ln >= a.warn:
                    notes += 1
                    print(f"note      {name}: {kind} run of {ln} bytes shared with the ROM (file offset {ds}, ROM ${rs:06X})")
            if a.verbose and not hits:
                print(f"clean     {name}")
    print(f"romscan: {nfiles} files, {nbytes} bytes, {ntext} decoded data streams; "
          f"{bad} violation(s) (run >= {a.min}), {notes} note(s) (run >= {a.warn}), {padding} padding run(s) ignored "
          f"(<= 4 distinct byte values) -> {'FAIL' if bad else 'CLEAN'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
