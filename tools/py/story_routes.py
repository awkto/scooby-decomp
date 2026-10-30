#!/usr/bin/env python3
"""Generator for tools/scripts/story/*.txt (flat harness scripts from power-on).

    story_routes.py SESSION_DIR NAME...     (run on the VM; needs `session.py serve SESSION_DIR` on a FRESH session)

Each route is a function below.  They run in strict mode (no savestates, no scans: object hit points come from
scripts/story/points_w*.json, produced by collect_points.py), so every emulator-visible input is recorded in the
route and `tools/py/verify.py` (batch harness replay) reproduces the same end state.  The expected end state of
every route is asserted here and written into the route header.
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from story import *
import story_full

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts", "story")


def finish(g, name, title, expect):
    st = g.state()
    ok = all(st[k] == v for k, v in expect.items())
    hdr = f"{title}\nend state: myst={st['myst']} room={st['room']} inv={st['inv']} story={st['story']}\nexpected {expect}: {'OK' if ok else 'MISMATCH'}"
    g.write_route(os.path.join(OUT, name + ".txt"), hdr)
    print(name, "OK" if ok else "MISMATCH", st["room"], st["inv"], st["story"])
    return ok


def w0_cafe_items(g):
    """Blake's Hotel, natural start (no password): Lobby -> Cafe; take the Bottle of Oil, open the Cabinet and take the
    Can Opener, push the Radio round, open its battery compartment and take the Battery."""
    g.boot(0)
    assert g.exit_via(45)              # Lobby Archway -> The Cafe (room 13)
    g.run(60)
    g.press("D", 30, 30)
    g.press("L", 20, 30)
    g.act("Take", 137)                 # Bottle of Oil          story $04 bit1
    g.act("Open", 126)                 # Cabinet                story $0E bit7 cleared; Can Opener appears
    g.act("Take", 127)                 # Can Opener             story $03 bit6
    g.act("Push", 133)                 # Radio turned           story $10 bit0
    g.act("Open", 133)                 # Battery Compartment    story $10 bit1
    g.act("Take", 135)                 # Battery                story $04 bit2
    return finish(g, "w0_cafe_items", "Blake's Hotel: Lobby -> Cafe, collect Oil, Can Opener, Battery (natural start)",
                  dict(room=13, inv=[134, 124, 132]))


def warp(g, myst, room):
    """password-warp into a room with the initial story state (CONTINUE screen), recorded in the route"""
    g.run_script(f"tools/scripts/rooms/w{myst}_r{room:02d}.txt", record=True)
    g.run(90)
    assert g.state()["room"] == room


def w0_mine_wheels(g):
    """Blake's Hotel, password warp to The Mine (room 6): collect the three mine-car wheels, fit them to the Mine Car."""
    warp(g, 0, 6)
    for t in (30, 31, 32):
        g.act("Take", t)
    print(g.inventory(), [(t, n) for t, n, r in g.room_objects()])
    g.act("Look", 29)
    g.use(28, 29)                      # Wheels on the Mine Car
    g.act("Look", 29)
    print(g.inventory(), g.state())
    return finish(g, "w0_mine_wheels", "Blake's Hotel (warp to The Mine): take the three wheels",
                  dict(room=6))


def w0_office(g):
    """Blake's Hotel, warp to The Office (room 17): heater, drawer, missing book, chalkboard."""
    warp(g, 0, 17)
    g.act("Look", 98)
    g.act("Take", 99)
    g.act("Open", 94)
    g.act("Take", 95)                  # Scissors (inside the drawer): used on the rope across the river
    g.act("Look", 100)
    print(g.inventory(), [(t, n) for t, n, r in g.room_objects()])
    return finish(g, "w0_office", "Blake's Hotel (warp to The Office): heater, drawer", dict(room=17))


def w0_kitchen(g):
    """Blake's Hotel, warp to The Kitchen (room 15): beads, chili, pot of water, flour."""
    warp(g, 0, 15)
    g.act("Take", 103)                 # Chili
    g.act("Take", 102)                 # Beads
    g.act("Push", 111)                 # Flour sack: shows the back of the Lobby painting
    print(g.inventory(), [(t, n) for t, n, r in g.room_objects()])
    return finish(g, "w0_kitchen", "Blake's Hotel (warp to The Kitchen): beads, chili, pot", dict(room=15))


def w1_front_lot(g):
    """Ha Ha Carnival, natural start (The Front Lot): take the Help Wanted sign, walk into the Front Office."""
    g.boot(1)
    g.run(30)
    g.act("Take", 87)                  # Sign (job notice)       story $04 bit0
    assert g.exit_via(89)              # Office Door -> The Front Office (room 11)
    g.run(60)
    g.act("Look", 77)                  # 'Wash me.' window
    return finish(g, "w1_front_lot", "Ha Ha Carnival: natural start, take the sign, enter the Front Office",
                  dict(myst=1, room=11, inv=[84]))


def w1_boathouse(g):
    """Ha Ha Carnival, warp to The Boathouse (room 14): the message in a bottle, and the motor boat."""
    warp(g, 1, 14)
    g.act("Look", 111)
    g.act("Take", 111)                 # Bottle                  story $04 bit7
    g.act("Take", 113)                 # Boat                    story $01 bit6
    print(g.inventory())
    return finish(g, "w1_boathouse", "Ha Ha Carnival (warp to The Boathouse): bottle with note, boat", dict(myst=1, room=14))


def w0_full(g):
    """Blake's Hotel, power-on to the credits (story_full.hotel_1 .. hotel_12): every action decided from RAM."""
    story_full.hotel_all(g)
    return finish(g, "w0_full", "Blake's Hotel: complete walkthrough, power-on -> unmasking -> credits -> Sega logo (reset)",
                  dict(room=0))


def w1_full(g):
    """Ha Ha Carnival, power-on to the credits (story_full.carnival_*)."""
    story_full.carnival_all(g)
    return finish(g, "w1_full", "Ha Ha Carnival: complete walkthrough, power-on -> unmasking -> credits -> Sega logo (reset)",
                  dict(room=0))


ROUTES = {f.__name__: f for f in (w0_cafe_items, w0_mine_wheels, w0_office, w0_kitchen, w1_front_lot, w1_boathouse, w0_full, w1_full)}

if __name__ == "__main__":
    # usage: story_routes.py SESSION_DIR [--learn] NAME...
    args = sys.argv[1:]
    d = args.pop(0)
    learn = "--learn" in args
    cpf = args[args.index("--cp") + 1] if "--cp" in args else None
    if cpf:
        args.remove("--cp"); args.remove(cpf)
    args = [a for a in args if a != "--learn"]
    g = Game(d, strict=not learn)
    open(g.rpath, "w").close()
    g.route = []
    for n in args or [r for r in ROUTES if not r.endswith('_full')]:
        ROUTES[n](g)
        if learn:
            g.save_points()
    if cpf:
        import json
        json.dump(g.cp, open(cpf, "w"), indent=0)
