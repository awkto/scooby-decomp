from story_full import *
g.talk_choose("Wanna ride")
g.run(300); g.settle(maxframes=9000, quiet=90)
print(g.state()['room'], [(t,n) for t,n,r in g.room_objects()], g.talk_choices(), story_bit(g,4,6))
