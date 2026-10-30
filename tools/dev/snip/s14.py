from story_full import *
g.act("Take", 128)
print(g.state()['room'], g.state()['inv'], story_bit(g,6,3), hex(g.w(0xFF09DC)))
for i in range(30):
    g.run(30)
    print(g.state()['room'], story_bit(g,6,3), story_bit(g,2,3), g.inventory()[-2:], hex(g.w(0xFF09DC)))
    if story_bit(g,2,3) or g.state()['room'] != 15: break
