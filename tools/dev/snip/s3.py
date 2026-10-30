from story_full import *
g.run(300); g.settle(maxframes=6000)
print(g.state())
g.act("Take", 95)
print(g.state()['inv'], g.said if 0 else '')
