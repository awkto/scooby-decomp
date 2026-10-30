from story_full import *
g.mark("c18")
g.act("Use", 19)
for i in range(60):
    g.run(30)
    if g.state()['room'] != 8: break
g.run(100); g.settle(maxframes=3000)
print(g.state()['room'])
g.use_inv(22, 131)
print(g.state()['inv'])
g.use_inv(23, 87)
print(g.state()['inv'], story_bit(g,4,1))
