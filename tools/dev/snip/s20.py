from story_full import *
g.mark("c17")
print([(t,n) for t,n,r in g.room_objects()], g.w(0xFF06CA))
g.act("Open", 21)
g.act("Take", 22)
print(g.state()['inv'])
g.use(154, 20)
print(story_bit(g,8,2), g.state()['inv'])
g.act("Use", 18)
for i in range(30):
    print(i, [story_bit(g,8,b) for b in range(5)], g.state()['room'])
    g.run(30)
    if g.state()['room']!=8: break
g.settle(maxframes=3000)
print(g.state())
