from story_full import *
print(g.state())
for t,n,r in g.room_objects(): print(t,n,r)
for i in range(40):
    r = {t:rr for t,n,rr in g.room_objects()}
    print(i, [story_bit(g,8,b) for b in range(5)], r.get(17), g.w(0xFF04DC), g.w(0xFF04F4), hex(g.w(0xFF09DC)))
    g.run(20)
    if g.state()['room']!=8: break
