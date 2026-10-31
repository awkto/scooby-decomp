#!/usr/bin/env python3
"""Interactive harness session (run on the VM, from the worktree root).

    session.py serve DIR     start build/harness reading commands from a FIFO; control socket DIR/sock
    session.py stop  DIR     close the harness (writes DIR/cov.bin)
    session.py DIR CMD...    send harness script lines, print the peek/echo log lines they produce

The harness is a batch tool (script file in, peek.log out); here the "script file" is a FIFO that a
daemon keeps open, so a driver (tools/py/story.py) can step the emulator, look at RAM, decide, and
continue -- while every state-changing command is recorded as a flat route that replays from boot.
"""
import json, os, socket, subprocess, sys, time


def serve(d):
    os.makedirs(d, exist_ok=True)
    ctl = os.path.join(d, "ctl")
    sock = os.path.join(d, "sock")
    log = os.path.join(d, "peek.log")
    for p in (ctl, sock, log):
        if os.path.exists(p):
            os.remove(p)
    os.mkfifo(ctl)
    # backend: default the GPGX harness; SCOOBY_BACKEND=port:HLESPEC drives build/port/scoobyport instead
    # SCOOBY_BACKEND=native:HLESPEC: the native runner (#24, no 68k instruction executed): SCOOBY_NATIVE_BIN
    # (the scooby build) or build/port/scoobyport with SCOOBY_NATIVE=1
    be = os.environ.get("SCOOBY_BACKEND", "harness")
    if be.startswith("port") or be.startswith("native"):
        hle = be.split(":", 1)[1] if ":" in be else "all"
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        binary = "build/port/scoobyport"
        if be.startswith("native"):
            env["SCOOBY_NATIVE"] = "1"
            binary = os.environ.get("SCOOBY_NATIVE_BIN") or binary
        cmd = [binary, "--hle", hle, "--script", ctl, "--outdir", d, "--frames", "0"]
        if os.environ.get("SCOOBY_PORT_OPTS"):     # e.g. widescreen=16:9 (port/tools/storytest.sh, STORYTEST_WS)
            cmd += ["--options", os.environ["SCOOBY_PORT_OPTS"]]
        h = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=open(os.path.join(d, "harness.err"), "w"))
    else:
        h = subprocess.Popen(["build/harness", "ext/gpgx/genesis_plus_gx_libretro.so", "rom/baserom.md", ctl, d],
                             stderr=open(os.path.join(d, "harness.err"), "w"))
    fifo = open(ctl, "w")           # opens once the harness has opened it for reading
    s = socket.socket(socket.AF_UNIX)
    s.bind(sock)
    s.listen(4)
    n = 0
    pos = 0
    while True:
        c, _ = s.accept()
        try:
            req = json.loads(c.makefile().readline())
        except ValueError:
            c.close()
            continue
        if req.get("quit"):
            fifo.close()
            h.wait()
            c.sendall(b'{"log":[]}\n')
            break
        for ln in req["cmds"]:
            fifo.write(ln + "\n")
        n += 1
        tag = f"SYNC{n}"
        fifo.write(f"echo {tag}\n")
        fifo.flush()
        lines = []
        t0 = time.time()
        while time.time() - t0 < 900:
            if os.path.exists(log):
                with open(log) as f:
                    f.seek(pos)
                    data = f.read()
                i = data.find(tag + "\n")
                if i >= 0:
                    lines = [l for l in data[:i].split("\n")[:-1] if l]
                    pos += i + len(tag) + 1
                    break
            time.sleep(0.002)
        try:
            c.sendall((json.dumps({"log": lines}) + "\n").encode())
            c.close()
        except OSError:
            pass            # client went away; the commands were still executed
    s.close()


def request(d, cmds=None, quit=False):
    s = socket.socket(socket.AF_UNIX)
    s.connect(os.path.join(d, "sock"))
    s.sendall((json.dumps({"cmds": cmds or [], "quit": quit}) + "\n").encode())
    r = json.loads(s.makefile().readline())
    s.close()
    return r["log"]


if __name__ == "__main__":
    if sys.argv[1] == "serve":
        serve(sys.argv[2])
    elif sys.argv[1] == "stop":
        request(sys.argv[2], quit=True)
    else:
        print("\n".join(request(sys.argv[1], sys.argv[2:])))
