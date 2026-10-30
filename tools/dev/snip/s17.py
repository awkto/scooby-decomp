from story_full import *
g.mark("c16")
g.use(153, 85, verb="Give")
for i in range(40):
    g.run(30)
    print(g.state()['room'], g.w(0xFF09DC) & 0x80, [story_bit(g,8,b) for b in range(5)], g.w(0xFF04DC))
    if g.state()['room'] == 8: break
g.shot("r0")
