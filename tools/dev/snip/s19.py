from story_full import *
g.send("load c16")
g.route = open(g.d + "/c16.route").read().split("\n")[:-1]
g.use(153, 85, verb="Give")
for i in range(400):
    if g.state()['room'] == 8: break
    g.run(3)
print("in 8 at", i)
f0 = 0
g.leave_pointer()
pt = g.locate(18)
print("pt", pt, [story_bit(g,8,b) for b in range(5)])
g.select_verb("Use")
g.set_pointer(*pt)
for _ in range(15):
    if g.w(0xFF06AE) == 18: break
    g.run(2)
g.press("B", 4, 4)
for i in range(30):
    r = {t:rr for t,n,rr in g.room_objects()}
    print(i, [story_bit(g,8,b) for b in range(5)], r.get(17)[7:9], g.state()['room'])
    g.run(20)
