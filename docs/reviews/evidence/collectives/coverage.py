# Coverage (counts every reachable edge, not just BFS-tree edges): confirm interesting transitions are actually reachable in the passing runs.
import model, collections
def cov(cfg, pats):
    r = model.explore(cfg)
    hits = collections.Counter()
    for s in r['parent']:
      for lab,_ in model.succ(cfg, s):
        for pat in pats:
            if all(x in lab for x in pat): hits[" & ".join(pat)] += 1
    return r['states'], hits
for name, cfg, pats in [
  ("N=1 L=3 BRB", model.Cfg([3],"BRB"), [("n0.pe0[k=0","ENQ_B"),("n0.pe2[k=2","ENQ_B"),("ARR_WAIT","sleeps"),("ARR_WAIT","EAGAIN"),("REL_WAIT","EAGAIN"),("COP_WAIT","sleeps"),("futex_wake(arrived,1)(nobody)",),("futex_wake(arrived,1)->woke",),("futex_wake(copied,1)(nobody)",),("pe1[k=2","INC","arrived:=")]),
  ("N=1 L=3 RBRR M=4", model.Cfg([3],"RBRR",M=4), [("arrived:=0",),("released:=0",),("copied:=0",),("ARR_LOAD","load=0==target",)]),
]:
    st, h = cov(cfg, pats)
    print(name, "states", st)
    for p in pats:
        k = " & ".join(p); print("   reachable transitions matching", repr(k), ":", h[k])
