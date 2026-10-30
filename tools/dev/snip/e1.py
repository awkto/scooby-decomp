from story_full import *
g.send("load c16")
g.route = open(g.d + "/c16.route").read().split("\n")[:-1]
g.use(153, 85, verb="Give")
for i in range(400):
    if g.state()['room'] == 8: break
    g.run(3)
def stage():
    r = {t:rr for t,n,rr in g.room_objects()}
    return r[17][7]
t0=[0]
def log(tag):
    print(tag, "stage", stage(), "cam", g.w(0xFF06CA), "F", [int(story_bit(g,8,b)) for b in range(5)], "room", g.w(0xFF06AC), "06AE", g.w(0xFF06AE), "ptr", g.w(0xFF06DC), g.w(0xFF06DE))
quick_use(g, 18)
for _ in range(300):
    if story_bit(g, 8, 0): break
    g.run(2)
g.use(154, 20)
log("fixed")
r19 = g.hot_rect(19)
pt19 = ((r19[0] + r19[2]) // 2 - 256 - 8, (r19[1] + r19[3]) // 2 - g.w(0xFF06CE) + 8)
print("pt19", pt19, r19)
quick_use(g, 18)
log("released")
pts=[(x,y) for y in range(24,110,12) for x in range(60,248,16)]
print(len(pts))
hits={}
for i in range(130):
    if g.w(0xFF06AC) != 8: break
    x,y = pts[i % len(pts)]
    g.set_pointer(x, y, settle=0)
    g.run(1)
    v = g.w(0xFF06AE)
    hits.setdefault(v, []).append((i,x,y))
for v,l in hits.items():
    print(v, len(l), l[:6], l[-3:])
