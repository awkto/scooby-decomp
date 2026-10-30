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
import glob
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


def route_lines(path):
    """The input lines (run/press) of a route script, without peeks/shots/saves/comments."""
    return [l.rstrip("\n") for l in open(path) if l.split()[:1] in (["run"], ["press"])]


def cut(lines, total):
    """First `total` frames of a list of run/press lines."""
    out, t = [], 0
    for l in lines:
        n = int(l.split()[-1])
        if t + n <= total:
            out.append(l)
            t += n
        else:
            out.append(" ".join(l.split()[:-1] + [str(total - t)]))
            t = total
        if t >= total:
            break
    return out


def nat_job(key, route, seed, frames):
    """Replay the flat route to `key` from boot, then random play; every first-seen room gets a flat route.
    (Loading savestates is NOT used here: a loaded state does not replay bit-exactly against a continuous run
    over long distances, so routes are always continuous scripts from boot.)"""
    w, r = key
    pointer, bias = seed % 3 == 0, seed % 3 == 1
    out = f"runs/nat/nat_w{w}_r{r}_s{seed}"
    body = [l for l in fuzzscript.gen("start", frames, seed, no_pause=True, peek=0, bias=bias, roomsave=False,
                                      pointer=pointer) if l.split()[0] in ("run", "press")]
    lines = list(route) + ["roomsave rs"] + body
    pk = run_job(out, lines)
    found = {}
    for l in pk:
        f = l.split()
        if len(f) == 4 and f[1] == "roomchange":
            k = (int(f[2][1:]), int(f[3][1:]))
            found.setdefault(k, int(f[0]))
    allin = list(route) + body
    res = {}
    for k, fr in found.items():
        res[k] = cut(allin, fr + 90)
    for f in glob.glob(f"{out}/*.state"):
        os.remove(f)
    return key, seed, res


def cmd_natural(rounds, frames, seeds):
    """Go-explore from the natural starts: random play, remember the first route into each new room, recurse."""
    boot = {w: route_lines(f"tools/scripts/natural_w{w}.txt") for w in (0, 1)}
    routes = {(0, 16): boot[0], (1, 12): boot[1]}
    todo = list(routes)

    def length(ls):
        return sum(int(l.split()[-1]) for l in ls)
    for rd in range(rounds):
        jobs = [(k, routes[k], sd + 1000 * rd + 17 * k[1], frames) for k in todo for sd in range(1, seeds + 1)]
        todo = []
        with cf.ThreadPoolExecutor(4) as ex:
            for key, seed, res in ex.map(lambda j: nat_job(*j), jobs):
                for k, full in res.items():
                    if k not in routes:
                        todo.append(k)
                        routes[k] = full
                    elif length(full) < length(routes[k]):
                        routes[k] = full
        print("round", rd, "rooms with natural routes:", len(routes), sorted(routes), flush=True)
    os.makedirs("tools/scripts/natural", exist_ok=True)
    for f in glob.glob("tools/scripts/natural/*.txt"):
        os.remove(f)
    for (w, r), lines in sorted(routes.items()):
        name = f"natural_w{w}_r{r:02d}"
        txt = [f"# Natural route (random-play discovered, no password/cheat) to mystery {w} room {r}; ends with save {name}"] + lines
        txt += ["peek myst FF06AA 2", "peek room FF06AC 2", f"save {name}"]
        open(f"tools/scripts/natural/{name}.txt", "w").write("\n".join(txt) + "\n")
    print(len(routes), "route scripts written")


def cmd_verify_natural():
    ok = {}
    names = sorted(glob.glob("tools/scripts/natural/natural_w*_r*.txt"))

    def one(path):
        base = os.path.basename(path)[:-4]
        w, r = int(base.split("_")[1][1:]), int(base.split("_")[2][1:])
        pk = run_job(f"runs/natv/{base}", [l.rstrip("\n") for l in open(path) if not l.startswith("#")])
        got = {l.split()[1]: int(l.split()[2], 16) for l in pk if len(l.split()) == 3 and l.split()[1] in ("room", "myst")}
        return base, got.get("myst") == w and got.get("room") == r
    with cf.ThreadPoolExecutor(4) as ex:
        for b, good in ex.map(one, names):
            ok[b] = good
    json.dump(ok, open("runs/natural_verified.json", "w"), indent=1, sort_keys=True)
    print(sum(ok.values()), "/", len(ok), "natural routes verified")
    for b, g in sorted(ok.items()):
        if not g:
            print("MISMATCH", b)


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
    elif c == "natural":
        cmd_natural(int(arg("--rounds", 3)), int(arg("--frames", 8000)), int(arg("--seeds", 3)))
    elif c == "verify-natural":
        cmd_verify_natural()
    elif c == "debug":
        cmd_debug()
    elif c == "stat":
        for n, m in sorted(load_index().items()):
            print(n, m)
