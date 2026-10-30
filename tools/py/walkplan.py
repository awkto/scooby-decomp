#!/usr/bin/env python3
"""Walk planner for the story driver: a distance field over a room's walkable pixels (walkmap.py = the game's own collision
test), so Shaggy can be steered to a target with the D-pad around obstacles and *without* stepping on other exits.

    f = Planner(myst, room)
    d = f.field(goal_rects, avoid_rects)      # BFS from the goal pixels over free pixels not inside avoid rects
    f.path(d, (x, y), n)                      # the next n pixels downhill from (x, y)
"""
import os, sys
from collections import deque
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import walkmap

# D-pad geometry (docs/subsystems/walking.md 5.1): x moves by the speed s, y by s/2, so one "step" is
# (+-2, 0) for L/R, (0, +-1) for U/D and (+-2, +-1) for the diagonals
NB = ((2, 0), (-2, 0), (0, 1), (0, -1), (2, 1), (2, -1), (-2, 1), (-2, -1))
_ROOMS = {}


class Planner:
    def __init__(self, myst, room):
        wm = walkmap.WalkMap(myst, room)
        self.W, self.H = wm.w * 8, wm.h * 8
        self.free = bytearray(self.W * self.H)
        for y in range(self.H):
            row = y * self.W
            for x in range(self.W):
                if not wm.blocked(x, y):
                    self.free[row + x] = 1
        self._cache = {}
        self._rows = None
        self._er = {}

    def eroded(self, m):
        """free pixels at least m px (Chebyshev) away from any blocked pixel: the game's collision is a thin fence and
        a position a pixel off the planner's model (sub-pixel speeds) must not end up on the wrong side of it"""
        if m == 0:
            return self.free
        if m in self._er:
            return self._er[m]
        W, H = self.W, self.H
        if self._rows is None:
            # each byte is 0/1: widen to one bit per pixel
            self._rows = [sum(1 << x for x in range(W) if self.free[y * W + x]) for y in range(H)]
        full = (1 << W) - 1
        hor = []
        for r in self._rows:
            a = r
            for k in range(1, m + 1):
                a &= (r << k) & full
                a &= r >> k
            hor.append(a)
        out = bytearray(W * H)
        for y in range(H):
            a = hor[y]
            for k in range(1, m + 1):
                a &= hor[y - k] if y - k >= 0 else 0
                a &= hor[y + k] if y + k < H else 0
            if a:
                for x in range(W):
                    if a >> x & 1:
                        out[y * W + x] = 1
        self._er[m] = out
        return out

    @classmethod
    def get(cls, myst, room):
        k = (myst, room)
        if k not in _ROOMS:
            _ROOMS[k] = cls(myst, room)
        return _ROOMS[k]

    def field(self, goals, avoid=(), margin=0):
        """goals/avoid: lists of world-pixel rects (x0, y0, x1, y1), x1/y1 exclusive.  Returns list of distances (-1 = unreachable)"""
        key = (tuple(goals), tuple(avoid), margin)
        if key in self._cache:
            return self._cache[key]
        W, H = self.W, self.H
        ok = bytearray(self.eroded(margin))
        for (x0, y0, x1, y1) in avoid:
            for y in range(max(y0, 0), min(y1, H)):
                for x in range(max(x0, 0), min(x1, W)):
                    ok[y * W + x] = 0
        dist = [-1] * (W * H)
        q = deque()
        for (x0, y0, x1, y1) in goals:
            for y in range(max(y0, 0), min(y1, H)):
                for x in range(max(x0, 0), min(x1, W)):
                    i = y * W + x
                    if self.free[i] and dist[i] < 0:        # goals count even inside avoid (it is where we want to be)
                        dist[i] = 0
                        q.append(i)
        while q:
            i = q.popleft()
            d = dist[i] + 1
            y, x = divmod(i, W)
            for dx, dy in NB:
                nx, ny = x - dx, y - dy        # BFS runs backwards from the goal: j -> i is the move (dx, dy)
                if 0 <= nx < W and 0 <= ny < H:
                    j = ny * W + nx
                    if ok[j] and dist[j] < 0 and (dx == 0 or ok[ny * W + x - dx // 2]):
                        dist[j] = d
                        q.append(j)
        if len(self._cache) > 12:
            self._cache.clear()
        self._cache[key] = dist
        return dist

    def nearest_reachable(self, pos, rects, avoid=()):
        """the pixel reachable from `pos` (not through avoid rects) that is closest to the centre of the first goal rect"""
        W, H = self.W, self.H
        dist = self.field([(pos[0], pos[1], pos[0] + 1, pos[1] + 1)], avoid)
        cx = sum((r[0] + r[2]) / 2 for r in rects) / len(rects)
        cy = sum((r[1] + r[3]) / 2 for r in rects) / len(rects)
        best = None
        for i, d in enumerate(dist):
            if d >= 0:
                y, x = divmod(i, W)
                e = (x - cx) ** 2 + (y - cy) ** 2
                if best is None or e < best[0]:
                    best = (e, x, y)
        return (best[1], best[2]) if best else None

    def path(self, dist, pos, n=24):
        """greedy descent of the distance field from pos; returns up to n pixels (excluding pos)"""
        W, H = self.W, self.H
        x, y = pos
        if not (0 <= x < W and 0 <= y < H):
            return []
        if dist[y * W + x] < 0:
            # standing on a pixel the field does not know (or outside): look for the nearest known neighbour
            best = None
            for r in range(1, 9):
                for dy in range(-r, r + 1):
                    for dx in range(-r, r + 1):
                        nx, ny = x + dx, y + dy
                        if 0 <= nx < W and 0 <= ny < H and dist[ny * W + nx] >= 0:
                            if best is None or dist[ny * W + nx] < best[0]:
                                best = (dist[ny * W + nx], nx, ny)
                if best:
                    break
            if not best:
                return []
            x, y = best[1], best[2]
        out = []
        pd = None
        for _ in range(n):
            cur = dist[y * W + x]
            if cur == 0:
                break
            cand = None
            for dx, dy in NB:
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and 0 <= ny < H:
                    d = dist[ny * W + nx]
                    if d >= 0 and d < cur:
                        # prefer to keep going straight
                        score = (d, 0 if (dx, dy) == pd else 1)
                        if cand is None or score < cand[0]:
                            cand = (score, nx, ny, (dx, dy))
            if cand is None:
                break
            _, x, y, pd = cand
            out.append((x, y))
        return out


PAD = {(2, 0): "R", (-2, 0): "L", (0, 1): "D", (0, -1): "U", (2, 1): "DR", (2, -1): "UR", (-2, 1): "DL", (-2, -1): "UL"}


def pad_for(dx, dy):
    """D-pad string for a move vector (8 directions)"""
    import math
    if dx == 0 and dy == 0:
        return ""
    a = math.degrees(math.atan2(dy, dx))          # 0 = right, 90 = down
    sect = int(((a + 22.5) % 360) // 45)
    return ["R", "DR", "D", "DL", "L", "UL", "U", "UR"][sect]
