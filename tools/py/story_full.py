#!/usr/bin/env python3
"""Full story walkthroughs (issue #29): Blake's Hotel (hotel_*) and Ha Ha Carnival (carnival_*), chained segments.

Every segment is a function of a `Game` (story.py) that decides each next input from RAM, so it works unchanged on
the harness and on the native port (--hle none / all).  A segment asserts its end condition with `need()`.
Segments chain: segment N+1 starts where N stopped (same session), so the full route is
    hotel_1 .. hotel_12       (power-on to the credits)
Object numbers are hit-test ids (object index + 3, `tools/py/objects.py`); the puzzle logic comes from the script
disassembly (`tools/py/scriptdis.py`, `blk.py`), see docs/walkthrough.md.
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from story import *


def need(g, cond, what):
    """assert a RAM-derived condition; raises with the current state for diagnosis"""
    if not cond:
        st = g.state()
        raise AssertionError(f"{what}: room={st['room']} inv={st['inv']} story={st['story']}")


def story_bit(g, byte, bit, r=None):
    return bool(g.b(0xFF2A00 + byte, r) >> bit & 1)


def has(g, tid):
    """is object `tid` (hit-test id) in the inventory"""
    return (tid - 3) in g.inventory()


def in_room(g, room):
    return g.w(0xFF06AC) == room


def calm(g, frames=90):
    g.run(frames)
    g.settle()


# ---------------------------------------------------------------- Blake's Hotel
def hotel_1(g):
    """Lobby -> Cafe: Bottle of Oil, Can Opener (cabinet), Battery (radio)."""
    g.boot(0)
    need(g, g.exit_via(45), "lobby archway")
    g.run(60)
    g.press("D", 30, 30)
    g.press("L", 20, 30)
    g.act("Take", 137)
    g.act("Open", 126)
    g.act("Take", 127)
    g.act("Push", 133)
    g.act("Open", 133)
    g.act("Take", 135)
    need(g, has(g, 127) and has(g, 135), "cafe items")


def hallway(g):
    """Lobby -> Hallway.  (With the Gardener's door open the spawn point lies on it and the walk-on script carries Shaggy
    straight into room 14; step back out through the Gardener's own door.)"""
    need(g, g.exit_via(46), "lobby stairs")
    g.run(90)
    if in_room(g, 14):
        need(g, g.exit_via(86), "gardener door -> hallway")
        g.run(90)
    need(g, in_room(g, 9), "hallway")


def hotel_2(g):
    """Lobby -> Hallway: open the six doors (the Ancient Chieftan scare scenes) which unlocks the Gardener's door;
    Gardener's Room: Air Freshener, Bed Spring, drawer 1 -> Antacid, drawer 2 -> Book."""
    need(g, g.exit_via(136) and in_room(g, 16), "cafe archway")
    g.run(60)
    need(g, g.exit_via(46) and in_room(g, 9), "lobby stairs")
    g.run(120)
    for d in (74, 75, 76, 77, 79, 78):
        g.act("Open", d)
        g.run(30)
    g.run(120)
    need(g, story_bit(g, 0x0B, 3) and not story_bit(g, 0x0E, 2), "hallway doors puzzle done (Gardener door unlocked)")
    need(g, g.exit_via(81) and in_room(g, 14), "gardener door")
    g.run(90)
    g.act("Take", 85)                  # Air Freshener
    g.act("Take", 90)                  # Bed Spring
    g.act("Open", 87)
    g.act("Take", 92)                  # Antacid
    g.act("Open", 88)
    g.act("Take", 91)                  # Book
    need(g, all(has(g, t) for t in (85, 90, 92, 91)), "gardener items")


def hotel_3(g):
    """Cafe: eat the Antacid in front of the Cook (he goes to the toilet), take the Key from the shelf."""
    for t in (86, 82, 45):
        need(g, g.exit_via(t), f"exit {t}")
        g.run(60)
    need(g, in_room(g, 13), "cafe")
    g.act_inv("Eat", 92)
    g.run(200)
    g.settle()
    g.act("Take", 129)                 # Key
    need(g, has(g, 129), "key")


def hotel_4(g):
    """Kitchen through the Hallway dumb waiter (the Cook guards the door): Chili -> Empty Chili Can, Pot of water, Soda Tab
    (push the fridge), peephole scene; Lobby: put the fire out, take the Crumpled Note."""
    need(g, g.exit_via(136), "cafe archway")
    g.run(60)
    hallway(g)
    g.act("Use", 80)                   # dumb waiter
    calm(g, 100)
    need(g, in_room(g, 15), "kitchen")
    g.use(127, 103)                    # Can Opener on the Chili -> Empty Chili Can
    g.act("Take", 124)                 # Pot
    g.use(124, 106)                    # Pot on the Kitchen Sink -> Pot o' Water
    g.act("Push", 111)                 # Flour sack: the peephole behind the Lobby painting
    g.act("Look", 112)
    g.settle(maxframes=6000, quiet=90)
    need(g, story_bit(g, 0x0B, 2), "peephole scene seen")
    g.act("Push", 119)                 # fridge away from the wall: Soda Tab
    g.act("Take", 121)
    g.act("Use", 110)                  # dumb waiter back
    calm(g, 100)
    need(g, in_room(g, 9), "hallway")
    need(g, g.exit_via(82) and in_room(g, 16), "hallway stairs")
    g.run(60)
    g.use(123, 40)                     # Pot o' Water on the Fire
    calm(g, 100)
    g.act("Take", 42)                  # Crumpled Note
    need(g, has(g, 42) and has(g, 121) and has(g, 104), "kitchen items")


def hotel_5(g):
    """Office: Heater, Scissors (drawer)."""
    g.act("Open", 44)
    need(g, g.exit_via(44) and in_room(g, 17), "office door")
    calm(g, 120)
    g.act("Take", 99)                  # Heater
    g.act("Open", 94)
    g.act("Take", 95)                  # Scissors
    need(g, has(g, 99) and has(g, 95), "heater + scissors")
    need(g, g.exit_via(101) and in_room(g, 16), "office -> lobby")
    g.run(90)


def outside(g):
    """Lobby -> Outside the Hotel"""
    g.act("Open", 47)
    need(g, g.exit_via(47) and in_room(g, 11), "outside door")
    calm(g)


def hotel_6(g):
    """Outside: the Shed (Key on the Lock; Crowbar, Work Gloves, Weed Killer, Shovel), dig out and unlock the cellar doors,
    Basement: Screwdriver, Extension Cord (locker), the note on the wine rack opens the way into the mine."""
    outside(g)
    need(g, g.exit_via(62) and in_room(g, 12), "path to shed")
    calm(g)
    g.use(129, 70)                     # Key on the Lock
    g.act("Open", 64)                  # Shed
    for t in (67, 65, 66, 72):
        g.act("Take", t)               # Crowbar, Work Gloves, Weed Killer, Shovel
    need(g, all(has(g, t) for t in (67, 65, 66, 72)), "shed items")
    need(g, g.exit_via(69) and in_room(g, 11), "shed -> front")
    g.run(90)
    g.use(72, 59)                      # Shovel on the Snow Covered Doors
    g.use(67, 58)                      # Crowbar on the Locked Doors
    g.run(60)
    g.act("Open", 57)
    need(g, g.exit_via(57) and in_room(g, 2), "cellar doors -> basement")
    calm(g)
    g.act("Take", 140)                 # Screwdriver
    g.act("Open", 144)                 # Locker
    g.act("Take", 176)                 # Extension Cord
    g.use(42, 145)                     # Crumpled Note on the Rack
    need(g, has(g, 140) and has(g, 176) and story_bit(g, 4, 4), "basement items + rack")


def hotel_7(g):
    """Mine: the three wheels fix the Mine Car; Shaft: hose -> engine -> gas, Pond: Air Freshener; back through the lift."""
    need(g, g.exit_via(143) and in_room(g, 6), "secret door -> mine")
    calm(g)
    for t in (30, 31, 32):
        g.act("Take", t)
    g.use(28, 29)                      # Wheels on the Mine Car
    calm(g, 60)
    g.act("Use", 29)                   # ride down
    calm(g, 120)
    need(g, in_room(g, 18), "shaft")
    g.act("Take", 148)                 # Hose
    g.use(148, 147)                    # on the Engine
    calm(g, 60)
    g.use_on(148, 150)                 # other end on the Gas
    calm(g, 60)
    need(g, story_bit(g, 8, 1), "hose connected")
    need(g, g.exit_via(152) and in_room(g, 19), "stone path -> pond")
    calm(g)
    g.act_inv("Use", 85)               # Air Freshener: the pond stops stinking, the bridge is usable
    calm(g, 100)
    need(g, not has(g, 85), "air freshener placed")
    need(g, g.exit_via(155) and in_room(g, 18), "pond -> shaft")
    g.run(90)
    g.act("Use", 149)                  # Switch: start the lift engine
    calm(g, 120)
    need(g, story_bit(g, 0x0B, 1), "lift engine running")
    g.act("Use", 29)                   # ride up
    calm(g, 150)
    need(g, in_room(g, 6), "back in the mine")
    need(g, g.exit_via(33) and in_room(g, 2), "mine -> basement")
    g.run(90)


def hotel_8(g):
    """Basement -> Outside: Extension Cord + Heater by the sleeping Bear, Work Gloves on the Poison Oak (Gardener's Room),
    Bed Spring -> Light Bulb, Poison Oak on the Bear drops the totem pole bridge; across the river Scissors cut the Rope."""
    need(g, g.exit_via(142) and in_room(g, 11), "basement stairs -> outside")
    calm(g)
    g.use(176, 60)                     # Extension Cord into the Outlet
    calm(g, 60)
    g.use(99, 176)                     # Heater on the Extension Cord: the Bear's cave warms up
    calm(g, 100)
    need(g, story_bit(g, 0x0F, 2), "heater running")
    need(g, g.exit_via(61) and in_room(g, 16), "outside -> lobby")
    g.run(90)
    hallway(g)
    need(g, g.exit_via(81) and in_room(g, 14), "hallway -> gardener")
    g.run(90)
    g.use(65, 89)                      # Work Gloves on the Poison Oak
    need(g, has(g, 89), "poison oak")
    need(g, g.exit_via(86) and in_room(g, 9), "gardener -> hallway")
    g.run(90)
    need(g, g.exit_via(82) and in_room(g, 16), "hallway -> lobby")
    g.run(90)
    outside(g)
    g.act_inv("Use", 90)               # Bed Spring: Scooby bounces up to the porch lights
    g.run(60)
    g.act("Take", 53)                  # Christmas Lights -> Light Bulb
    calm(g, 100)
    need(g, has(g, 179), "light bulb")
    g.use(89, 177)                     # Poison Oak on the Bear: he leaves, the totem pole falls across the river
    calm(g, 300)
    need(g, story_bit(g, 0x0F, 5), "totem pole bridge")
    need(g, g.exit_via(56) and in_room(g, 21), "bridge -> across the river")
    calm(g)
    g.use(95, 169)                     # Scissors on the Rope
    g.act("Take", 169)
    need(g, has(g, 169), "rope")


def hotel_9(g):
    """Back through the bridge, Lobby, Hallway dumb waiter -> Kitchen: Screwdriver on the Vent Cover, Empty Chili Can on the
    Termites; Light Bulb + Battery + Soda Tab -> Flashlight."""
    for t in (174, 61):
        need(g, g.exit_via(t), f"exit {t}")
        g.run(90)
    need(g, in_room(g, 16), "lobby")
    hallway(g)
    g.act("Use", 80)
    calm(g, 100)
    need(g, in_room(g, 15), "kitchen")
    g.use(140, 108)                    # Screwdriver on the Vent Cover
    g.use(104, 107)                    # Empty Chili Can on the Termites
    need(g, has(g, 105), "termites")
    g.use_inv(135, 179)                # Battery + Light Bulb
    g.use_inv(181, 121)                # + Soda Tab
    need(g, has(g, 183), "flashlight")


def hotel_10(g):
    """Kitchen -> Hallway -> Lobby -> Outside -> Basement -> Mine -> Shaft -> Pond -> dark Maze: the Flashlight lights it, the
    walk planner solves it, Tomb."""
    g.act("Use", 110)
    calm(g, 100)
    for t in (82, 47, 57, 143):
        need(g, g.exit_via(t), f"exit {t}")
        calm(g)
    need(g, in_room(g, 6), "mine")
    g.act("Use", 29)
    calm(g, 120)
    for t in (152, 156):
        need(g, g.exit_via(t), f"exit {t}")
        calm(g)
    need(g, in_room(g, 7), "dark maze")
    g.act_inv("Use", 183)              # Flashlight
    calm(g, 120)
    need(g, in_room(g, 8), "lit maze")
    need(g, g.exit_via(161) and in_room(g, 20), "maze exit -> tomb")
    calm(g)


def hotel_11(g):
    """Tomb: Weed Killer on the Killer Lettuce, Dungeon: Rope on the Cuffs, down to Uncle Blake, Termites free him; back in
    the Tomb the Book (Give) lets the Statue be talked to: XYZZY gives the Medallion."""
    g.use(66, 168)                     # Weed Killer on the Killer Lettuce
    need(g, g.exit_via(167) and in_room(g, 4), "tomb archway -> dungeon")
    calm(g)
    g.use(169, 23)                     # Rope on the Cuffs
    g.act("Use", 169)                  # climb down
    calm(g, 150)
    need(g, in_room(g, 5), "lower dungeon")
    g.use(105, 26)                     # Termites on Uncle Blake
    g.run(300)
    g.settle(maxframes=6000)
    need(g, in_room(g, 4) and story_bit(g, 0x0C, 5), "uncle blake freed")
    need(g, g.exit_via(20) and in_room(g, 20), "passage -> tomb")
    calm(g, 150)
    g.use(91, 26, verb="Give")         # Book to Uncle Blake: he explains the statue
    g.run(200)
    g.settle(maxframes=6000)
    need(g, story_bit(g, 0x0C, 0), "uncle blake has the book")
    g.talk_to(166, "XYZZY")
    g.run(200)
    g.settle(maxframes=6000)
    g.act("Take", 165)                 # Medallion
    need(g, has(g, 165), "medallion")


def hotel_12(g):
    """Tomb -> Pond -> Shaft (Switch, lift) -> Mine -> Basement: the Medallion on the Hook ends the mystery (credits)."""
    for t in (163, 155):
        need(g, g.exit_via(t), f"exit {t}")
        calm(g)
    g.act("Use", 149)                  # Switch again (riding resets it)
    calm(g, 120)
    g.act("Use", 29)
    calm(g, 150)
    need(g, in_room(g, 6), "mine")
    need(g, g.exit_via(33) and in_room(g, 2), "basement")
    calm(g)
    g.use(165, 146)                    # Medallion on the Hook
    ending(g)


def ending(g):
    """the unmasking cutscene plays, the credits roll and the game restarts at the Sega logo (room word 0)"""
    for _ in range(200):
        g.run(120)
        if g.w(0xFF06AC) == 0:
            break
    need(g, g.w(0xFF06AC) == 0, "ending: game restarted after the credits")
    g.checkpoint("ending: credits done, back at the Sega logo")


HOTEL = [hotel_1, hotel_2, hotel_3, hotel_4, hotel_5, hotel_6, hotel_7, hotel_8, hotel_9, hotel_10, hotel_11, hotel_12]


def hotel_all(g):
    for f in HOTEL:
        f(g)


def crane_game(g):
    """Crane Game minigame (room 31, script op minigame_throw $3F58): the claw (x $FF04E8, 100..148, D-pad L/R) drops on A/B and
    descends 2 px per frame to y=$4A; it catches the Water Pistol iff, at the bottom, the pistol's path offset ($FF0610) lies
    in $F8..$114 (claw x != $94) or $5C..$78 (claw at x=$94).  The offset advances ~1.66 per frame and wraps at ~$140.
    Closed loop: read the offset every frame and press B when the predicted offset at the bottom is inside the window."""
    for _ in range(2400):
        g.run(1)
        p = g.w(0xFF0610)
        frames = max(0, (0x4A - g.w(0xFF0500)) // 2)
        end = (p + 1.66 * frames) % 0x140
        if 0xFC <= end <= 0x110:
            g.press("B", 3, 0)
            break
    g.run(300)
    g.settle()


def bumper_tug(g, max_frames=6000):
    """Bumper Car duel (script op minigame_tug $3142): top-down arena, Shaggy's car (x/y $FF04DC/$FF04F4, heading = actor anim
    0..7 clockwise from north) against the Clown's (x/y $FF04E4/$FF04FC).  D-pad L/R turn, A/B gas (speed meter $FF0A90 0..7); each
    side has a 63-point meter ($FF0A98 ours, $FF0A9A theirs); the first below zero loses.  Naive closed loop: steer at the
    opponent, hold the gas."""
    import math
    for _ in range(300):                             # intro: wait for the arena (room 6) with both meters full
        if g.w(0xFF06AC) == 6 and g.w(0xFF0A98) == 0x3F and g.w(0xFF0A9A) == 0x3F:
            break
        g.run(10)
    for i in range(max_frames // 3):
        r = g.ram()
        mine, theirs = g.w(0xFF0A98, r), g.w(0xFF0A9A, r)
        if mine >= 0x8000 or theirs >= 0x8000:
            break
        sx, sy = g.w(0xFF04DC, r), g.w(0xFF04F4, r)
        ox, oy = g.w(0xFF04E4, r), g.w(0xFF04FC, r)
        h = g.w(0xFF0488, r) & 7
        ang = math.degrees(math.atan2(ox - sx, -(oy - sy))) % 360        # 0 = north, clockwise
        hd = int(((ang + 22.5) % 360) // 45)
        d = (hd - h) % 8
        btn = "A"
        if 1 <= d <= 3:
            btn += "R"
        elif 5 <= d <= 7:
            btn += "L"
        g.press(btn, 3, 0)
    return g.w(0xFF0A98), g.w(0xFF0A9A)


def start_bumper(g):
    """click USE on the Bumper Car (room 5) without waiting for the script: the duel needs input from its first frame"""
    g.leave_pointer()
    pt = g.locate(11)
    g.select_verb("Use")
    g.set_pointer(*pt)
    for _ in range(15):
        if g.w(0xFF06AE) == 11:
            break
        g.run(4)
    g.press("B", 4, 4)


# ---------------------------------------------------------------- Ha Ha Carnival
def carnival_1(g):
    """Front Lot -> Boardwalk: Taffy (the taffy machine); Hammer Game: pull the pole instead of playing (the 'cheat': the Orbit
    cutscene sends the Magnet to the Ferris wheel, the attendant hands over the hammer)."""
    g.boot(1)
    g.run(60)
    g.settle()
    need(g, g.exit_via(91) and in_room(g, 24), "front lot -> boardwalk")
    calm(g)
    g.act("Take", 101)                 # Taffy
    need(g, has(g, 102), "taffy")
    for t in (104, 92):
        need(g, g.exit_via(t), f"exit {t}")
        calm(g, 120)
    need(g, in_room(g, 16), "hammer game")
    g.act("Pull", 151)                 # the pole: Orbit scene, the Magnet ends up on the Ferris wheel
    g.run(200)
    g.settle(maxframes=9000, quiet=90)
    need(g, story_bit(g, 1, 1) and in_room(g, 16), "pole pulled (Magnet on the Ferris wheel)")


def carnival_2(g):
    """Back Lot: the Balloons carry Scooby up to the Ferris wheel for the Magnet; Diving Bell: Valve down, Magnet on the Treasure
    chest -> Tokens, Valve up."""
    for t in (149, 91, 107):
        need(g, g.exit_via(t), f"exit {t}")
        calm(g)
    need(g, in_room(g, 13), "back lot")
    g.run(240)                         # Scooby catches up
    g.use_on(116, 2)                   # Balloons with Scooby
    g.run(300)
    g.settle(maxframes=9000, quiet=90)
    need(g, has(g, 147), "magnet")
    need(g, g.exit_via(125) and in_room(g, 20), "back lot -> diving bell")
    calm(g)
    g.act("Use", 137)                  # Valve: dive
    g.run(300)
    g.settle(maxframes=6000, quiet=60)
    need(g, in_room(g, 18), "diving bell (under water)")
    g.use(147, 138)                    # Magnet on the Treasure
    g.run(200)
    g.settle(maxframes=6000, quiet=60)
    need(g, has(g, 139), "tokens")
    g.act("Use", 140)                  # Valve: back up
    g.run(300)
    g.settle(maxframes=6000, quiet=60)
    need(g, in_room(g, 20), "surface")


def carnival_3(g):
    """Game Tent: Tokens in the Crane Game (claw minigame: timing from the pistol's path offset), Water Pistol -> the Kid (Front Lot)
    for the Tickets -> Dart Attendant: three Tickets, three throws -> Teddy Bear."""
    for t in (132, 124, 104, 90):
        need(g, g.exit_via(t), f"exit {t}")
        calm(g)
    need(g, in_room(g, 19), "game tent")
    g.use(139, 62)                     # Tokens in the Crane Game
    g.run(120)
    need(g, in_room(g, 31), "crane game")
    crane_game(g)
    need(g, in_room(g, 19), "back in the tent")
    g.act("Take", 63)                  # Water Pistol
    need(g, has(g, 63), "water pistol")
    need(g, g.exit_via(65) and in_room(g, 12), "tent -> front lot")
    calm(g)
    g.use(63, 85, verb="Give")         # to the Kid for his Tickets
    g.run(120)
    g.settle()
    need(g, has(g, 64), "tickets")
    need(g, g.exit_via(90) and in_room(g, 19), "front lot -> tent")
    calm(g)
    g.use(64, 53, verb="Give")         # Tickets to the Dart Attendant
    g.run(100)
    g.settle()
    for i in range(3):
        g.act("Use", 55)               # Darts
        g.run(60)
        g.settle()
        if i < 2:
            g.use(64, 53, verb="Give")
            g.run(60)
            g.settle()
    need(g, has(g, 54), "teddy bear")


def carnival_4(g):
    """Back Lot: Teddy Bear to the Balancing Bear -> Ball."""
    for t in (65, 91, 107):
        need(g, g.exit_via(t), f"exit {t}")
        calm(g)
    need(g, in_room(g, 13), "back lot")
    g.use(54, 122, verb="Give")        # Teddy Bear to the Balancing Bear
    g.run(200)
    g.settle(maxframes=6000)
    g.act("Take", 121)                 # Ball
    need(g, has(g, 121), "ball")


def carnival_5(g):
    """Boardwalk -> Fun House (steer down at the prompt) -> Bumper Cars: Taffy on the Engine, Switch on, win the car duel
    against the Clown, take the Dart."""
    for t in (124, 105):
        need(g, g.exit_via(t), f"exit {t}")
        g.run(90)
    need(g, in_room(g, 23), "fun house")
    g.run(200)
    g.press("D", 40, 0)                # the prompt (script op wait_dpad_choice): D = the bumper cars
    g.run(300)
    g.settle(maxframes=3000, quiet=60)
    need(g, in_room(g, 5), "bumper cars")
    g.use(102, 9)                      # Taffy on the Engine (it has to be off): drive belt
    calm(g, 100)
    g.act("Use", 8)                    # Switch on
    calm(g, 60)
    need(g, story_bit(g, 7, 0) and story_bit(g, 7, 1), "engine on with the taffy belt")
    start_bumper(g)
    bumper_tug(g)
    g.run(200)
    g.settle(maxframes=6000)
    need(g, in_room(g, 7) and story_bit(g, 0, 7), "won the duel (Dart on the floor)")
    g.act("Take", 12)                  # Dart
    need(g, has(g, 12), "dart")


CARNIVAL = [carnival_1, carnival_2, carnival_3, carnival_4, carnival_5]


def carnival_all(g):
    for f in CARNIVAL:
        f(g)
