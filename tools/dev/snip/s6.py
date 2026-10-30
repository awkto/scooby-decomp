from story_full import *
g.act("Push", 26)
g.run(200); g.settle(maxframes=3000)
print(g.state()['room'], [(t,n) for t,n,r in g.room_objects()], story_bit(g,7,6))
g.act("Take", 29)
print(g.state()['inv'])
g.use_inv  # noqa
g.use(29, 113, ) if 0 else None
