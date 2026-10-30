import time, traceback
from story_full import *
t0 = time.time()
for f in CARNIVAL:
    try:
        f(g)
    except Exception:
        traceback.print_exc()
        print("FAILED in", f.__name__, g.state(), flush=True)
        break
    g.mark(f.__name__)
    print("== done", f.__name__, "room", g.w(0xFF06AC), "t=%.0f" % (time.time() - t0), flush=True)
print("END", g.state())
