from story_full import *
g.mark("c11")
g.act("Take", 69)
g.act("Open", 71)
g.act("Take", 72)
print(g.state()['inv'])
g.use_inv(79, 69)
print(g.state()['inv'], story_bit(g,3,5))
