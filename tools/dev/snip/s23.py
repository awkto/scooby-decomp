from story_full import *
g.mark("c19")
need(g, g.exit_via(107) and in_room(g, 13), "back lot")
calm(g)
g.act_inv("Use", 88)
g.run(100); g.settle()
print(g.state()['inv'], story_bit(g,4,2), [(t,n) for t,n,r in g.room_objects()])
for t in (124, 104, 90, 66):
    need(g, g.exit_via(t), f"exit {t}")
    calm(g)
print(g.state()['room'])
g.use(70, 67)
g.run(300); g.settle(maxframes=6000, quiet=60)
print(g.state()['room'], story_bit(g,5,5), [(t,n) for t,n,r in g.room_objects()])
