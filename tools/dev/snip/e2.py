from story_full import *
g.send("load c16")
g.route = open(g.d + "/c16.route").read().split("\n")[:-1]
g.use(153, 85, verb="Give")
for i in range(100):
    if in_room(g, 8): break
    g.run(3)
roller_coaster(g)
print(g.state())
