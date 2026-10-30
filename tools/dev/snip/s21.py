from story_full import *
g.send("load c17")
g.route = open(g.d + "/c17.route").read().split("\n")[:-1]
def stage():
    r = {t:rr for t,n,rr in g.room_objects()}
    return r[17][7], r[17][8]
g.use(154, 20)
g.leave_pointer()
pt = g.locate(18)
g.select_verb("Use")
g.set_pointer(*pt)
for _ in range(15):
    if g.w(0xFF06AE) == 18: break
    g.run(2)
g.press("B", 4, 0)

for i in range(100):
    if g.w(0xFF06CA) >= 256: break
    g.run(2)
print("cam", g.w(0xFF06CA), stage())
pt = g.locate(19)
print("pt19", pt, stage())
g.select_verb("Use")
g.set_pointer(*pt)
for _ in range(15):
    if g.w(0xFF06AE) == 19: break
    g.run(2)
g.press("B", 4, 0)
print("clicked", stage(), story_bit(g,8,4))
last=None
for i in range(400):
    g.run(1)
    s = (stage()[0], story_bit(g,8,4), story_bit(g,8,1), story_bit(g,8,0))
    if s != last:
        print(i, s, g.state()['room'], hex(g.w(0xFF09DC)))
        last = s
    if g.state()['room'] != 8: break
    if story_bit(g,8,0): break
g.shot("u1")
g.act("Open", 21)
g.act("Take", 22)
print(g.state()['inv'])
