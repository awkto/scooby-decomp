#!/usr/bin/env python3
"""storybytes.py MYST -- table of the 29 story bytes ($FF2A00..$FF2A1C): for every flag bit what the password-restore script
(start script 2) does when it is set, and which object script sets/clears it (and under which verb).

Input: build/scripts/w{M}_start.txt and w{M}_objects.txt (run tools/py/scriptdis.py first).  Output: Markdown rows."""
import os, re, sys

m = sys.argv[1] if len(sys.argv) > 1 else "0"
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "scripts")
start = open(os.path.join(root, f"w{m}_start.txt")).read()
objs = open(os.path.join(root, f"w{m}_objects.txt")).read()
rooms = open(os.path.join(root, f"w{m}_rooms.txt")).read()

# restore effects: inside "if Fb.n == 1:" ... "endif" (flat; nested ifs are rare)
s2 = start[start.index("; ---- start2"):]
effects = {}
cur = None
depth = 0
for ln in s2.split("\n"):
    t = ln[7:].strip() if len(ln) > 7 else ""
    mm = re.match(r"if F([0-9A-F]+)\.(\d) == 1:", t)
    if mm and cur is None:
        cur = (int(mm.group(1), 16), int(mm.group(2)))
        depth = 1
        effects.setdefault(cur, [])
        continue
    if cur is None:
        continue
    if t.startswith("if "):
        depth += 1
    elif t == "endif":
        depth -= 1
        if depth == 0:
            cur = None
        continue
    mv = re.match(r'move_obj #(\d+)"([^"]*)" -> (INVENTORY|nowhere|room\d+"[^"]*")', t)
    if mv:
        effects[cur].append(f'{mv.group(2) or "#" + mv.group(1)} -> ' + re.sub(r'room(\d+)"([^"]*)"', r'room \1 (\2)', mv.group(3)))
        continue
    mv = re.match(r'set_state #(\d+)"([^"]*)" = (\$[0-9A-F]+)', t)
    if mv:
        effects[cur].append(f'{mv.group(2) or "#" + mv.group(1)} state {mv.group(3)}')

# setters: object script lines "set Fb.n = v" with the enclosing "case VERB"
setters = {}
for blk in re.split(r"(?m)^(?=; ---- )", objs):
    h = re.match(r'; ---- object #(\d+) "([^"]*)"', blk)
    if not h:
        continue
    verb = None
    for ln in blk.split("\n"):
        t = ln[7:].strip() if len(ln) > 7 else ""
        mc = re.match(r"case (\w+)", t)
        if mc:
            verb = mc.group(1)
        if t.startswith("on_event"):
            verb = "event"
        ms = re.match(r"set F([0-9A-F]+)\.(\d) = (\d)", t)
        if ms:
            k = (int(ms.group(1), 16), int(ms.group(2)))
            setters.setdefault(k, []).append(f'{h.group(2) or "#" + h.group(1)} {verb} ={ms.group(3)}')

print("| byte | bit | restore effect (password load) | set by (object, verb) |")
print("|---|---|---|---|")
keys = sorted(set(effects) | {k for k in setters if k[0] < 29})
for k in keys:
    if k[0] >= 29:
        continue
    eff = "; ".join(dict.fromkeys(effects.get(k, []))) or "(none: engine/scratch flag)"
    st = "; ".join(dict.fromkeys(setters.get(k, [])[:4]))
    print(f"| `{k[0]:02X}` | {k[1]} | {eff} | {st} |")
