#!/usr/bin/env python3
"""Coverage campaign driver (run on the VM, from the worktree root, with system python3).

    campaign.py boot                       -> runs/states/titlemenu.state
    campaign.py warp [--variants init,ones,rand1,rand2]   password-warp into every room (51 x variants)
    campaign.py fuzz [--frames N] [--seeds K] [--from GLOB]   random no-pause play from warp states
    campaign.py stat                       -> list states in the index

All emulator runs use build/harness; each job's cov.bin is OR-ed into runs/cov_acc.bin and deleted.
Savestates (gitignored, regenerated): runs/states/*.state; index runs/states/index.json
"""
import concurrent.futures as cf
import fnmatch
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzzscript  # noqa: E402
import password  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
os.chdir(ROOT)
STATES = "runs/states"
ACC = "runs/cov_acc.bin"
INDEX = os.path.join(STATES, "index.json")
NROOMS = {0: 21, 1: 30}
lock = threading.Lock()


def acc_merge(path):
    d = open(path, "rb").read()
    n = len(d)
    with lock:
        a = open(ACC, "rb").read() if os.path.exists(ACC) else bytes(n)
        v = int.from_bytes(a, "big") | (int.from_bytes(d, "big") & int.from_bytes(b"\x03" * n, "big"))
        open(ACC, "wb").write(v.to_bytes(n, "big"))


def run_job(out, lines, states=None):
    """Run the harness on `lines` in a fresh dir `out`; states = {name: path} are symlinked in.
    Returns the lines of peek.log."""
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    for n, p in (states or {}).items():
        os.symlink(os.path.abspath(p), os.path.join(out, n + ".state"))
    sp = os.path.join(out, "script.txt")
    open(sp, "w").write("\n".join(lines) + "\n")
    subprocess.run(["nice", "build/harness", "ext/gpgx/genesis_plus_gx_libretro.so", "rom/baserom.md", sp, out],
                   capture_output=True, text=True, timeout=1800)
    cov = os.path.join(out, "cov.bin")
    if os.path.exists(cov):
        acc_merge(cov)
        os.remove(cov)
    pk = os.path.join(out, "peek.log")
    return open(pk).read().split("\n") if os.path.exists(pk) else []


def load_index():
    return json.load(open(INDEX)) if os.path.exists(INDEX) else {}


def save_index(ix):
    with lock:
        json.dump(ix, open(INDEX, "w"), indent=1, sort_keys=True)


def init_state(world):
    rom = open("rom/baserom.md", "rb").read()
    hdr = struct.unpack(">I", rom[0x31AF2 + 4 * world:0x31AF2 + 4 * world + 4])[0]
    s0 = struct.unpack(">I", rom[hdr:hdr + 4])[0]
    return bytes(rom[s0:s0 + 29])


def variants_for(world, name):
    if name == "init":
        return init_state(world)
    if name == "ones":
        return bytes([0xFF] * 29)
    if name.startswith("rand"):
        r = random.Random(int(name[4:]) * 1000 + world)
        return bytes(r.randrange(256) for _ in range(29))
    raise ValueError(name)


def cmd_boot():
    lines = [l.rstrip("\n") for l in open("tools/scripts/boot_title.txt") if l.strip() and not l.startswith("#")]
    os.makedirs(STATES, exist_ok=True)
    run_job("runs/boot", lines)
    shutil.move("runs/boot/titlemenu.state", f"{STATES}/titlemenu.state")


def warp_job(world, room, var):
    st = variants_for(world, var)
    lines = ["load titlemenu"] + password.full_script(world, room, st) + ["save warp"]
    out = f"runs/warp/w{world}_r{room}_{var}"
    pk = run_job(out, lines, {"titlemenu": f"{STATES}/titlemenu.state"})
    got = {}
    for l in pk:
        f = l.split()
        if len(f) == 3 and f[1] in ("room", "myst"):
            got[f[1]] = int(f[2], 16)
    ok = got.get("room") == room and got.get("myst") == world
    name = f"pw_w{world}_r{room}_{var}"
    if os.path.exists(f"{out}/warp.state"):
        shutil.move(f"{out}/warp.state", f"{STATES}/{name}.state")
    return name, dict(world=world, room=room, var=var, ok=ok, pw=password.encode(room, st), via="password")


def cmd_warp(variants):
    jobs = [(w, r, v) for v in variants for w in (0, 1) for r in range(2, 2 + NROOMS[w])]
    ix = load_index()
    with cf.ThreadPoolExecutor(4) as ex:
        for name, meta in ex.map(lambda j: warp_job(*j), jobs):
            ix[name] = meta
            if not meta["ok"]:
                print("WARP FAILED", name)
    save_index(ix)
    print(sum(1 for m in ix.values() if m.get("ok")), "/", len(ix), "warp states ok")


def fuzz_job(name, seed, frames, bias, pause=False, pointer=False):
    out = f"runs/fuzz/{name}_s{seed}{'p' if pause else ''}{'x' if pointer else ''}"
    lines = fuzzscript.gen("start", frames, seed, no_pause=not pause, peek=300, bias=bias, roomsave=True, pointer=pointer)
    pk = run_job(out, lines, {"start": f"{STATES}/{name}.state"})
    rooms = {l.split()[2] + l.split()[3] for l in pk if "roomchange" in l}
    return name, seed, rooms


def cmd_fuzz(frames, seeds, pattern, all_states=False, pause=False, pointer=False):
    ix = load_index()
    names = sorted(n for n in ix if (all_states or ix[n].get("ok")) and fnmatch.fnmatch(n, pattern)
                   and os.path.exists(f"{STATES}/{n}.state"))
    jobs = [(n, s, frames, s % 2 == 1, pause, pointer) for n in names for s in range(1, seeds + 1)]
    seen = {}
    with cf.ThreadPoolExecutor(4) as ex:
        for name, seed, rooms in ex.map(lambda j: fuzz_job(*j), jobs):
            seen.setdefault(name, set()).update(rooms)
    allrooms = set().union(*seen.values()) if seen else set()
    print("room changes seen by fuzzing:", len(allrooms), sorted(allrooms))


def cmd_debug():
    """Enable the hidden debug pause menu in both worlds (states dbgmenu_w0/1) via the cheat code."""
    for w in (0, 1):
        lines = [l.rstrip("\n") for l in open(f"tools/scripts/debug_menu_w{w}.txt") if l.strip() and not l.startswith("#")]
        r = 16 if w == 0 else 3
        out = f"runs/dbg_w{w}"
        run_job(out, lines, {f"pw_w{w}_r{r}_init": f"{STATES}/pw_w{w}_r{r}_init.state"})
        shutil.move(f"{out}/dbgmenu_w{w}.state", f"{STATES}/dbgmenu_w{w}.state")
        ix = load_index()
        ix[f"dbgmenu_w{w}"] = dict(world=w, room=r, ok=True, via="cheat-code pause menu")
        save_index(ix)


def arg(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


if __name__ == "__main__":
    c = sys.argv[1]
    if c == "boot":
        cmd_boot()
    elif c == "warp":
        cmd_warp(arg("--variants", "init").split(","))
    elif c == "fuzz":
        cmd_fuzz(int(arg("--frames", 10000)), int(arg("--seeds", 2)), arg("--from", "pw_*_init"), "--all" in sys.argv, "--pause" in sys.argv, "--pointer" in sys.argv)
    elif c == "debug":
        cmd_debug()
    elif c == "stat":
        for n, m in sorted(load_index().items()):
            print(n, m)
