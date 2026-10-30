from story_full import *
g.mark("c8")
g.use(114, 33)
g.run(300); g.settle(maxframes=9000, quiet=90)
print(g.state()['room'], story_bit(g,7,3), [(t,n) for t,n,r in g.room_objects()])
g.act("Take", 87)
print(g.state()['inv'])
