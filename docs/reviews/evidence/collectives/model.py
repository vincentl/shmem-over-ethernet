#!/usr/bin/env python3
"""
Exhaustive interleaving model of design §5.4-§5.5 (epoch counters, reduction
destination, barrier arrival) plus an abstract internode tree (§5.1, §5.3).

Per node n, per team (one team modeled):
  arrived, released, copied   -- shared atomics, modulo M
  futex wait queue per word   -- set of sleeping member indices
  lead dest tag               -- (epoch, stage) stage: 0 own-copied, 1 local-combined,
                                 2 subtree-combined, 3 final
Each PE i (member 0 is local lead): pc, k, d, v (loaded value / combine index / chunk).
Program: same sequence of 'B' (barrier/sync: no data) and 'R' (reduction) for all PEs.

wait_eq(c, t):  LOAD: v = c; if v == t done else WAIT
                WAIT: futex_wait(c, v): atomically { if c != v -> LOAD (EAGAIN) else sleep }
futex_wake(c, n): nondeterministically wake any min(n, |q|) sleepers -> LOAD.
Spurious variant: any sleeper may wake at any time.

Internode tree (parent[]), root 0.  A node's arrival for epoch e is sent upward
(in CH chunks; for R each chunk reads lead dest by reference, as §3.8 descriptors do)
once the node's local arrival is enqueued and all its children's arrivals for e have
been merged.  Root releases e when its own arrival is enqueued and all children merged.
Per-node release application: write final result to lead dest (R), released++,
futex_wake(released, INT_MAX); forwarding of the result to children by reference
either BEFORE the local bump (fwd='before') or AFTER local wake (fwd='after').

Checks (violations): stale source read, stale/clobbered dest read (copy-out chunk,
upward chunk, downward chunk), arrival epoch mismatch / double arrival, released
bump past expected epoch, waiter seeing aliased (wrapped) target, lead leaving
before all copy-outs of the epoch, deadlock (no enabled transitions, not done),
lost wakeup (a sleeper whose predicate holds in a deadlock), and liveness
(every reachable state can still reach the all-done state).
"""
import sys, itertools, collections, time

START, INC, WAKE_ARR, ENQ_B, ARR_LOAD, ARR_WAIT, ARR_SLEEP, COMB, ENQ_R, \
REL_LOAD, REL_WAIT, REL_SLEEP, COPYOUT, COPINC, WAKE_COP, COP_LOAD, COP_WAIT, \
COP_SLEEP, FINISH, DONE = range(20)
PCN = "START INC WAKE_ARR ENQ_B ARR_LOAD ARR_WAIT ARR_SLEEP COMB ENQ_R REL_LOAD REL_WAIT REL_SLEEP COPYOUT COPINC WAKE_COP COP_LOAD COP_WAIT COP_SLEEP FINISH DONE".split()
AW, RW, CW = 0, 1, 2  # futex words
CH = 2                # chunks per by-reference send / memcpy


class Cfg:
    def __init__(self, Ls, prog, M=2**32, parent=None, spurious=False, fwd='before',
                 mut=()):
        self.Ls = tuple(Ls); self.N = len(Ls); self.prog = prog; self.M = M
        self.parent = parent if parent is not None else [None] + [0] * (self.N - 1)
        self.children = [[c for c in range(self.N) if self.parent[c] == n] for n in range(self.N)]
        self.spurious = spurious; self.fwd = fwd; self.mut = set(mut)


def initial(cfg):
    nodes = []
    for n in range(cfg.N):
        L = cfg.Ls[n]
        pes = tuple((START, 0, 0, 0) for _ in range(L))
        src = tuple(-1 for _ in range(L))
        ctr = (0, 0, 0)        # arrived, released, copied (mod M)
        ghost = (0, 0, 0)      # unbounded true counts
        wq = (frozenset(), frozenset(), frozenset())
        dest = (-1, 3)
        # internode: enq, merged_from_children(tuple per child), sent(up epochs done),
        #            upchunk, recv_by_parent(int), relin, applied, rstep, downchunks(tuple per child)
        inter = (0, tuple(0 for _ in cfg.children[n]), 0, 0, 0, 0, 0, 0,
                 tuple(0 for _ in cfg.children[n]))
        nodes.append((pes, src, ctr, ghost, wq, dest, inter))
    return (tuple(nodes), 0)  # 0 = root epochs released


def done_state(cfg, s):
    nodes, rel = s
    for n, nd in enumerate(nodes):
        if any(p[0] != DONE for p in nd[0]): return False
        inter = nd[6]
        if inter[6] != len(cfg.prog) or inter[7] != 0: return False
        if any(c != 0 for c in inter[8]): return False
        if n != 0 and inter[2] != len(cfg.prog): return False
    return True


class Viol(Exception):
    pass


def succ(cfg, s):
    """yield (label, newstate) ; raise violations via yielding ('VIOL', msg)"""
    nodes, relsent = s
    M = cfg.M; P = cfg.prog; T = len(P)
    out = []

    def setnode(n, nd, rs=None):
        nl = list(nodes); nl[n] = nd
        return (tuple(nl), relsent if rs is None else rs)

    for n, nd in enumerate(nodes):
        pes, src, ctr, ghost, wq, dest, inter = nd
        L = cfg.Ls[n]
        enq, merged, sent, upchunk, recvp, relin, applied, rstep, down = inter
        viol = lambda msg: out.append(("VIOL", f"node{n}: {msg}"))

        def upd(i=None, pe=None, src_=None, ctr_=None, ghost_=None, wq_=None, dest_=None, inter_=None):
            p = pes if pe is None else pes[:i] + (pe,) + pes[i + 1:]
            return (p, src if src_ is None else src_, ctr if ctr_ is None else ctr_,
                    ghost if ghost_ is None else ghost_, wq if wq_ is None else wq_,
                    dest if dest_ is None else dest_, inter if inter_ is None else inter_)

        def wake(word, cnt, label):
            q = wq[word]
            if not q:
                yield (label + "(nobody)", None)
                return
            m = min(cnt, len(q))
            for chosen in itertools.combinations(sorted(q), m):
                p = list(pes)
                for j in chosen:
                    pc, k, d, v = p[j]
                    back = {ARR_SLEEP: ARR_LOAD, REL_SLEEP: REL_LOAD, COP_SLEEP: COP_LOAD}[pc]
                    p[j] = (back, k, d, v)
                nq = list(wq); nq[word] = q - frozenset(chosen)
                yield (label + f"->woke{chosen}", (tuple(p), tuple(nq)))

        # ---------------- PE transitions ----------------
        for i, (pc, k, d, v) in enumerate(pes):
            lead = (i == 0)
            lab = f"n{n}.pe{i}[k={k},d={d}] {PCN[pc]}"
            if pc == START:
                if k == T:
                    out.append((lab + " return-all", setnode(n, upd(i, (DONE, k, d, 0)))))
                else:
                    s2 = src[:i] + (k,) + src[i + 1:]
                    # lead: in-place memcpy/clobber check happens in COMB
                    out.append((lab + f" source:=epoch{k}", setnode(n, upd(i, (INC, k, d, 0), src_=s2))))
            elif pc == INC:
                r = (ctr[0] + 1) % M
                c2 = (r, ctr[1], ctr[2]); g2 = (ghost[0] + 1, ghost[1], ghost[2])
                tgt = ((k + 1) * L) % M
                if P[k] == 'B':
                    npc = ENQ_B if r == tgt else REL_LOAD
                else:
                    if lead:
                        npc = ARR_LOAD
                        if r == tgt and 'noguard' in cfg.mut: npc = WAKE_ARR
                    else:
                        npc = WAKE_ARR if r == tgt else REL_LOAD
                        if r == tgt and 'nonlead_enq' in cfg.mut: npc = ENQ_B
                out.append((lab + f" arrived:={r}", setnode(n, upd(i, (npc, k, d, 0), ctr_=c2, ghost_=g2))))
            elif pc == WAKE_ARR:
                nxt = ARR_LOAD if lead else REL_LOAD
                for l2, res in wake(AW, 1, lab + " futex_wake(arrived,1)"):
                    if res is None:
                        out.append((l2, setnode(n, upd(i, (nxt, k, d, 0)))))
                    else:
                        p, q = res; pc2 = list(p); pc2[i] = (nxt, k, d, 0)
                        out.append((l2, setnode(n, (tuple(pc2), src, ctr, ghost, q, dest, inter))))
            elif pc == ENQ_B:
                if enq != k:
                    viol(f"arrival for epoch {k} enqueued but node expected epoch {enq} (double/wrong-epoch arrival) by pe{i}")
                i2 = (enq + 1,) + inter[1:]
                out.append((lab + f" enqueue arrival(epoch {k})", setnode(n, upd(i, (REL_LOAD, k, d, 0), inter_=i2))))
            elif pc in (ARR_LOAD, REL_LOAD, COP_LOAD):
                word = {ARR_LOAD: 0, REL_LOAD: 1, COP_LOAD: 2}[pc]
                tgt = {ARR_LOAD: (k + 1) * L, REL_LOAD: k + 1, COP_LOAD: (d + 1) * (L - 1)}[pc] % M
                truth = {ARR_LOAD: (k + 1) * L, REL_LOAD: k + 1, COP_LOAD: (d + 1) * (L - 1)}[pc]
                val = ctr[word]
                if val == tgt:
                    if ghost[word] != truth:
                        viol(f"pe{i} {PCN[pc]} saw target {tgt} but true count is {ghost[word]} (expected {truth}): wraparound alias / wrong epoch")
                    if pc == ARR_LOAD:
                        npc = COMB
                    elif pc == REL_LOAD:
                        if P[k] == 'B':
                            npc = FINISH
                        else:
                            npc = COP_LOAD if lead else COPYOUT
                            if lead and 'lead_no_copied_wait' in cfg.mut: npc = FINISH
                    else:
                        npc = FINISH
                    out.append((lab + f" load={val}==target", setnode(n, upd(i, (npc, k, d, 0)))))
                else:
                    if 'ge' in cfg.mut and word == 1 and ghost[1] > truth:
                        pass
                    out.append((lab + f" load={val}!=target{tgt}", setnode(n, upd(i, (pc + 1, k, d, val)))))
            elif pc in (ARR_WAIT, REL_WAIT, COP_WAIT):
                word = {ARR_WAIT: 0, REL_WAIT: 1, COP_WAIT: 2}[pc]
                if ctr[word] != v and 'nocmp' not in cfg.mut:
                    out.append((lab + f" futex_wait EAGAIN ({ctr[word]}!={v})", setnode(n, upd(i, (pc - 1, k, d, v)))))
                else:
                    nq = list(wq); nq[word] = wq[word] | {i}
                    out.append((lab + f" futex_wait sleeps (val {v})", setnode(n, upd(i, (pc + 1, k, d, v), wq_=tuple(nq)))))
            elif pc in (ARR_SLEEP, REL_SLEEP, COP_SLEEP):
                if cfg.spurious:
                    word = {ARR_SLEEP: 0, REL_SLEEP: 1, COP_SLEEP: 2}[pc]
                    nq = list(wq); nq[word] = wq[word] - {i}
                    out.append((lab + " spurious wake", setnode(n, upd(i, (pc - 2, k, d, v), wq_=tuple(nq)))))
            elif pc == COMB:
                # v = step index: 0 memcpy own source (chunk-free), 1..L-1 combine member j
                if v == 0:
                    # clobber check: every non-lead must have finished copying out every
                    # earlier data epoch (its d >= lead d)
                    if src[0] != k: viol(f"lead read own source tag {src[0]} in epoch {k}")
                    nd_ = (k, 0)
                else:
                    j = v
                    pj = pes[j]
                    if src[j] != k or pj[1] != k or pj[0] in (START, INC):
                        viol(f"lead combined pe{j}'s source (tag {src[j]}, pe{j} at k={pj[1]} {PCN[pj[0]]}) in epoch {k}: read before that PE called")
                    if dest[0] != k: viol(f"lead dest tag {dest} changed under lead combine in epoch {k}")
                    nd_ = (k, 0)
                nv = v + 1
                if nv >= L:
                    out.append((lab + f" combine step {v} -> dest=(e{k},local)", setnode(n, upd(i, (ENQ_R, k, d, 0), dest_=(k, 1)))))
                else:
                    out.append((lab + f" combine step {v}", setnode(n, upd(i, (COMB, k, d, nv), dest_=nd_))))
            elif pc == ENQ_R:
                if enq != k:
                    viol(f"reduction arrival epoch {k} but node expected {enq}")
                i2 = (enq + 1,) + inter[1:]
                out.append((lab + f" enqueue arrival(epoch {k}, dest ref)", setnode(n, upd(i, (REL_LOAD, k, d, 0), inter_=i2))))
            elif pc == COPYOUT:
                if dest != (k, 3):
                    viol(f"pe{i} copy-out chunk {v} of epoch {k} read lead dest tag {dest} (stale or clobbered)")
                if v + 1 >= CH:
                    out.append((lab + f" copy-out chunk {v}", setnode(n, upd(i, (COPINC, k, d, 0)))))
                else:
                    out.append((lab + f" copy-out chunk {v}", setnode(n, upd(i, (COPYOUT, k, d, v + 1)))))
            elif pc == COPINC:
                r = (ctr[2] + 1) % M
                tgt = ((d + 1) * (L - 1)) % M
                npc = WAKE_COP if r == tgt else FINISH
                out.append((lab + f" copied:={r}", setnode(n, upd(i, (npc, k, d, 0), ctr_=(ctr[0], ctr[1], r), ghost_=(ghost[0], ghost[1], ghost[2] + 1)))))
            elif pc == WAKE_COP:
                for l2, res in wake(CW, 1, lab + " futex_wake(copied,1)"):
                    if res is None:
                        out.append((l2, setnode(n, upd(i, (FINISH, k, d, 0)))))
                    else:
                        p, q = res; pl = list(p); pl[i] = (FINISH, k, d, 0)
                        out.append((l2, setnode(n, (tuple(pl), src, ctr, ghost, q, dest, inter))))
            elif pc == FINISH:
                nd2 = d + (1 if P[k] == 'R' else 0)
                if lead and P[k] == 'R':
                    for j in range(1, L):
                        pj = pes[j]
                        finished = pj[2] > d or (pj[1] == k and pj[0] in (COPINC, WAKE_COP, FINISH))
                        if not finished:
                            viol(f"lead leaving epoch {k} before pe{j} finished copy-out (pe{j} {PCN[pj[0]]} k={pj[1]})")
                out.append((lab + " return", setnode(n, upd(i, (START, k + 1, nd2, 0)))))

        # ---------------- internode / progress thread transitions ----------------
        kids = cfg.children[n]
        # merge child arrival
        for ci, c in enumerate(kids):
            e = merged[ci]
            cinter = nodes[c][6]
            if cinter[4] > e and enq > e and applied == e:
                # child arrival of epoch e received; parent's own local arrival enqueued
                if P[e] == 'R' and dest[0] != e:
                    viol(f"merging child {c} partial of epoch {e} into dest tagged {dest}")
                m2 = merged[:ci] + (e + 1,) + merged[ci + 1:]
                i2 = (enq, m2) + inter[2:]
                nd_ = (e, 2) if P[e] == 'R' else dest
                out.append((f"n{n}.prog merge child n{c} epoch {e}", setnode(n, upd(dest_=nd_, inter_=i2))))
        allmerged_e = lambda e: all(mm > e for mm in merged)
        # upward send (non-root)
        if n != 0:
            e = sent
            if e < T and enq > e and allmerged_e(e):
                if P[e] == 'R' and (dest[0] != e or dest[1] < 1):
                    viol(f"upward send chunk {upchunk} of epoch {e} read dest tagged {dest}")
                if upchunk + 1 >= CH:
                    i2 = (enq, merged, sent + 1, 0, recvp + 1) + inter[5:]
                else:
                    i2 = (enq, merged, sent, upchunk + 1, recvp) + inter[5:]
                out.append((f"n{n}.prog up-send epoch {e} chunk {upchunk}", setnode(n, upd(inter_=i2))))
        else:
            e = relsent
            if e < T and enq > e and allmerged_e(e):
                i2 = inter[:5] + (relin + 1,) + inter[6:]
                out.append((f"n0.root release epoch {e}", setnode(n, upd(inter_=i2), rs=relsent + 1)))
        # local release application
        if applied < relin:
            e = applied
            if rstep == 0:
                if P[e] == 'R':
                    if dest[0] != e or dest[1] < 1:
                        viol(f"release epoch {e}: writing final result over dest tagged {dest}")
                    nd_ = (e, 3)
                else:
                    nd_ = dest
                i2 = inter[:7] + (1,) + inter[8:]
                out.append((f"n{n}.prog release e{e}: write final dest", setnode(n, upd(dest_=nd_, inter_=i2))))
            elif rstep == 1:
                if cfg.fwd == 'before' and any(nodes[c][6][5] <= e for c in kids):
                    pass  # must finish forwarding first
                else:
                    if ctr[1] != e % M or ghost[1] != e:
                        viol(f"released bump for epoch {e} but released={ctr[1]} (true {ghost[1]})")
                    r = (ctr[1] + 1) % M
                    i2 = inter[:7] + (2,) + inter[8:]
                    out.append((f"n{n}.prog released:={r}", setnode(n, upd(ctr_=(ctr[0], r, ctr[2]), ghost_=(ghost[0], ghost[1] + 1, ghost[2]), inter_=i2))))
            elif rstep == 2:
                cnt = 1 if 'wake1' in cfg.mut else 10**9
                for l2, res in wake(RW, cnt, f"n{n}.prog futex_wake(released,{'1' if cnt == 1 else 'ALL'})"):
                    i2 = inter[:6] + (applied + 1, 0) + inter[8:]
                    if res is None:
                        out.append((l2, setnode(n, upd(inter_=i2))))
                    else:
                        p, q = res
                        out.append((l2, setnode(n, (p, src, ctr, ghost, q, dest, i2))))
        # downward forward to children, by reference to lead dest
        for ci, c in enumerate(kids):
            crelin = nodes[c][6][5]
            if cfg.fwd == 'before':
                ok = applied < relin and rstep == 1 and crelin == applied
                e = applied
            else:
                ok = applied > crelin
                e = crelin
            if ok and e < T:
                ch = down[ci]
                if P[e] == 'R' and dest != (e, 3):
                    viol(f"downward forward to n{c} epoch {e} chunk {ch} read dest tagged {dest}")
                if ch + 1 >= CH:
                    d2 = down[:ci] + (0,) + down[ci + 1:]
                    # deliver to child
                    cn = nodes[c]
                    ci2 = cn[6][:5] + (crelin + 1,) + cn[6][6:]
                    nl = list(nodes)
                    nl[n] = upd(inter_=inter[:8] + (d2,))
                    nl[c] = cn[:6] + (ci2,)
                    out.append((f"n{n}.prog fwd release e{e} to n{c} chunk {ch} (delivered)", (tuple(nl), relsent)))
                else:
                    d2 = down[:ci] + (ch + 1,) + down[ci + 1:]
                    out.append((f"n{n}.prog fwd release e{e} to n{c} chunk {ch}", setnode(n, upd(inter_=inter[:8] + (d2,)))))
    return out


def lost_wakeups(cfg, s):
    msgs = []
    for n, nd in enumerate(s[0]):
        pes, src, ctr, ghost, wq, dest, inter = nd
        L = cfg.Ls[n]
        for i, (pc, k, d, v) in enumerate(pes):
            if pc in (ARR_SLEEP, REL_SLEEP, COP_SLEEP):
                word = {ARR_SLEEP: 0, REL_SLEEP: 1, COP_SLEEP: 2}[pc]
                tgt = {ARR_SLEEP: (k + 1) * L, REL_SLEEP: k + 1, COP_SLEEP: (d + 1) * (L - 1)}[pc] % cfg.M
                if ctr[word] == tgt:
                    msgs.append(f"n{n}.pe{i} asleep in {PCN[pc]} while predicate holds (counter={ctr[word]})")
    return msgs


def explore(cfg, maxstates=20_000_000):
    s0 = initial(cfg)
    parent = {s0: None}
    q = collections.deque([s0])
    edges = {}
    viols = []
    deadlocks = []
    dones = []
    t0 = time.time()
    while q:
        s = q.popleft()
        outs = succ(cfg, s)
        nxt = []
        for lab, ns in outs:
            if lab == "VIOL":
                if len(viols) < 3:
                    viols.append((ns, s))
                continue
            nxt.append(ns)
            if ns not in parent:
                parent[ns] = (s, lab)
                q.append(ns)
                if len(parent) > maxstates:
                    raise RuntimeError("state limit")
        edges[s] = nxt
        if not nxt:
            if done_state(cfg, s):
                dones.append(s)
            else:
                deadlocks.append(s)
        if viols and len(viols) >= 1 and 'stop_on_viol' in cfg.mut:
            break
    # liveness: backward reachability to done
    rev = collections.defaultdict(list)
    for s, ns in edges.items():
        for t in ns:
            rev[t].append(s)
    good = set(dones)
    stack = list(dones)
    while stack:
        t = stack.pop()
        for p in rev[t]:
            if p not in good:
                good.add(p); stack.append(p)
    stuck = [s for s in edges if s not in good]
    return dict(states=len(parent), viols=viols, deadlocks=deadlocks, dones=len(dones),
                stuck=stuck, parent=parent, secs=time.time() - t0)


def trace(parent, s, limit=80):
    steps = []
    while parent[s] is not None:
        p, lab = parent[s]
        steps.append(lab)
        s = p
    steps.reverse()
    if len(steps) > limit:
        steps = ["..."] + steps[-limit:]
    return steps


def run(name, cfg, expect_ok=True, show=True):
    r = explore(cfg)
    ok = not r['viols'] and not r['deadlocks'] and not r['stuck']
    status = "PASS" if ok else "FAIL"
    tag = "" if ok == expect_ok else "   <-- UNEXPECTED"
    print(f"[{status}] {name}: states={r['states']} terminal-done={r['dones']} deadlocks={len(r['deadlocks'])} "
          f"cannot-finish={len(r['stuck'])} violations={'yes' if r['viols'] else 'no'} ({r['secs']:.1f}s){tag}")
    if not ok and show:
        if r['viols']:
            msg, s = r['viols'][0]
            print("   first violation:", msg)
            for st in trace(r['parent'], s):
                print("     ", st)
        elif r['deadlocks']:
            s = min(r['deadlocks'], key=lambda x: len(trace(r['parent'], x, 10**6)))
            print("   deadlock; lost wakeups:", lost_wakeups(cfg, s))
            for st in trace(r['parent'], s):
                print("     ", st)
        elif r['stuck']:
            print("   states that cannot reach completion (livelock/starvation):", len(r['stuck']))
    sys.stdout.flush()
    return r


if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    progs = ["BBB", "RRR", "BRB", "RBR", "RRB", "BRR"]
    if which in ("all", "single"):
        print("== Single node (loopback release), design as written, M = 2^32 ==")
        for L in (1, 2, 3):
            for p in progs:
                run(f"N=1 L={L} prog={p}", Cfg([L], p))
        print("== Single node, spurious wakeups ==")
        for L in (2, 3):
            for p in ("RRR", "BRB", "RBR"):
                run(f"N=1 L={L} prog={p} spurious", Cfg([L], p, spurious=True))
        print("== Wraparound: small moduli ==")
        for L in (2, 3):
            for M in (16, 8, 4, L):
                run(f"N=1 L={L} prog=RBRR M={M}", Cfg([L], "RBRR", M=M))
        run("N=1 L=3 prog=RBRR M=2 (M < L)", Cfg([3], "RBRR", M=2), expect_ok=False)
    if which in ("all", "multi"):
        print("== Two nodes, release needs both arrivals ==")
        for Ls in ((2, 1), (2, 2), (3, 1), (1, 3)):
            for p in ("BRB", "RBR", "RRR"):
                run(f"N=2 L={Ls} prog={p} fwd=before", Cfg(list(Ls), p))
        run("N=2 L=(2,2) prog=RBR M=4 spurious", Cfg([2, 2], "RBR", M=4, spurious=True))
        print("== Three-node chain 0<-1<-2 ==")
        run("N=3 chain L=(1,2,1) prog=RRB fwd=before", Cfg([1, 2, 1], "RRB", parent=[None, 0, 1]))
        run("N=3 chain L=(2,1,2) prog=BRR fwd=before", Cfg([2, 1, 2], "BRR", parent=[None, 0, 1]))
        print("== Downward forward by reference AFTER local release (§3.8 descriptor semantics) ==")
        run("N=2 L=(1,1) prog=RR fwd=after", Cfg([1, 1], "RR", fwd='after'), expect_ok=False)
        run("N=3 chain L=(1,1,1) prog=RR fwd=after", Cfg([1, 1, 1], "RR", parent=[None, 0, 1], fwd='after'), expect_ok=False)
    if which in ("all", "mut"):
        print("== Mutations (the checker must catch these) ==")
        run("mut: no !is_lead guard (lead wakes arrived itself) L=3 RRR", Cfg([3], "RRR", mut={'noguard'}))
        run("mut: lead does not wait on copied L=2 RR", Cfg([2], "RR", mut={'lead_no_copied_wait'}), expect_ok=False)
        run("mut: futex_wake(released,1) L=3 BB", Cfg([3], "BB", mut={'wake1'}), expect_ok=False)
        run("mut: futex_wait without kernel compare L=2 RR", Cfg([2], "RR", mut={'nocmp'}), expect_ok=False)
        run("mut: non-lead also enqueues arrival in reduction L=2 RR", Cfg([2], "RR", mut={'nonlead_enq'}), expect_ok=False)
