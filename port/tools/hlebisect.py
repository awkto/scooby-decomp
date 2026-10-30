#!/usr/bin/env python3
"""Find which C replacement(s) make a closed-loop story step differ from --hle none.

    port/tools/hlebisect.py ROUTE [--steps N]       (run on a VM from the worktree root, after building build/port)

Runs the route with --hle none (reference checkpoints), then bisects the replacement list: disabling a half of the
replacements ("all,-a,-b,...") and checking whether the checkpoints match the reference.  Prints the minimal set of
replacements whose removal restores agreement (delta-debugging style, one culprit at a time) and the first differing
checkpoint of the unmodified --hle all run.
"""
import json, os, subprocess, sys, time

route = sys.argv[1]
root = os.getcwd()
env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", STORY_OUT="runs/hlebisect/gen")
n = [0]


def run(spec):
    n[0] += 1
    d = f"runs/hlebisect/r{n[0]}"
    os.makedirs(d, exist_ok=True)
    e = dict(env, SCOOBY_BACKEND="port:" + spec)
    srv = subprocess.Popen([sys.executable, "tools/py/session.py", "serve", d], env=e, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if os.path.exists(d + "/sock"):
            break
        time.sleep(0.2)
    cp = d + ".cp.json"
    subprocess.run([sys.executable, "tools/py/story_routes.py", d, route, "--cp", cp], env=e, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=1200)
    subprocess.run([sys.executable, "tools/py/session.py", "stop", d], env=e, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    srv.wait(timeout=30)
    return json.load(open(cp)) if os.path.exists(cp) else None


key = lambda c: (c["room"], c["inv"], c["story"])
ref = run("none")


def same(cp):
    return cp is not None and len(cp) == len(ref) and all(key(a) == key(b) for a, b in zip(cp, ref))


names = [l.split()[1] for l in subprocess.run(["build/port/scoobyport", "--list-hle"], capture_output=True, text=True, env=env).stdout.splitlines()
         if len(l.split()) > 1]
full = run("all")
if same(full):
    print("no divergence")
    sys.exit(0)
for i, (a, b) in enumerate(zip(full or [], ref)):
    if key(a) != key(b):
        print("first differing checkpoint", i, a["label"], "\n  all :", key(a), "\n  none:", key(b))
        break
culprits = []
cands = list(names)
off = lambda s: "all," + ",".join("-" + x for x in s) if s else "all"
while not same(run(off(culprits))):
    lo = list(cands)
    # predicate: disabling culprits + S restores agreement.  With one culprit left among `lo`, bisect on S = a half.
    if not same(run(off(culprits + lo))):
        print("disabling every candidate does not restore agreement (timing or a non-HLE cause)")
        break
    while len(lo) > 1:
        half = lo[: len(lo) // 2]
        lo = half if same(run(off(culprits + half))) else lo[len(lo) // 2:]
    culprits.append(lo[0])
    cands.remove(lo[0])
    print("culprit:", lo[0], flush=True)
print("replacements responsible:", culprits)
