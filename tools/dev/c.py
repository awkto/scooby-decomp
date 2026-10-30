from story_full import *
g.back("carnival_1")
g.run(20)
for t in (149, 91, 107):
    need(g, g.exit_via(t), f"exit {t}")
    calm(g)
g.run(240)
g.use_on(116, 2)
print(g.inventory())
