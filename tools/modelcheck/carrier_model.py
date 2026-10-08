#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Two concurrent shmem_global_exit callers on two nodes, each its node's
carrier (abort.h; design §3.15, §6.4; v0.2 step 4, Part 2).

The carrier rule - progress-host.c's osoe_progress_host_abort; progress.c's
osoe_progress_peers_ended, flush_aborting and flush_control_aborting - with one
wire a pair and with two, written by the second adversarial read of Part 2
(S1). abort_exit_model.py sends every abortive goodbye whether or not the
peer's has arrived, a superset of the code's sends, which hides this case.

Each node's PE has declared its own global exit (its node's abort word names its
own PE, so it is a carrier). Its agent then aborts: for each wire, in any order
(a data wire's send may wait behind a segment in flight), it sends its abortive
goodbye - unless the peer's abortive goodbye has already arrived on that wire,
when it has nobody to tell. Then, a carrier, it waits until every peer wire has
ended, but a wire on which the peer's own abort - naming another caller -
arrived: peers_ended's carrier rule. Its process then ends, which ends its
streams (keep_stream: the end of stream comes with the process's end).

Checked: from every state, a state where both nodes have ended is reachable
(AG EF), the bound OSOE_ABORT_LEAD_WAIT_SECONDS left out. A state from which it
is not is two carriers each waiting for the other: both run to the bound.

Variants
  one_wire        one connection a pair, as before v0.2 step 4's Part 2.
  two_wires       two, every rule per wire, as first built: two goodbyes
                  that cross arrive on different wires, and each carrier waits
                  for the other's stream to the bound (a breakage).
  either_wire     two, peers_ended skipping both of a peer's wires once its
                  abort has arrived on either: the design.
"""
import sys
from collections import deque


def run(variant):
    wires = ('D',) if variant == 'one_wire' else ('C', 'D')
    W = len(wires)
    # node: (phase, sent[W], nobody[W], peer_aborted[W], ended_in[W]); phase:
    # 0 declared, 1 aborting, 2 waiting, 3 ended
    def node0():
        return (0, (False,) * W, (False,) * W, (False,) * W, (False,) * W)
    # channel x -> y per wire: tuple of items
    start = ((node0(), node0()), (((),) * W, ((),) * W))

    def succ(S):
        nodes, chans = S
        out = []
        for x in (0, 1):
            y = 1 - x
            phase, sent, nobody, pab, ended = nodes[x]

            def mk(nx, cx=None, cy=None, label=''):
                ns, cs = list(nodes), list(chans)
                ns[x] = nx
                if cx is not None:
                    cs[x] = cx
                if cy is not None:
                    cs[y] = cy
                out.append(((tuple(ns), tuple(cs)), label))

            if phase == 3:
                continue
            # receive the head of the peer's stream on wire k
            for k in range(W):
                if chans[y][k]:
                    item, rest = chans[y][k][0], chans[y][k][1:]
                    np, ne = list(pab), list(ended)
                    if item == 'ABORT':
                        np[k] = True
                    else:
                        ne[k] = True
                    cy = list(chans[y])
                    cy[k] = rest
                    mk((phase, sent, nobody, tuple(np), tuple(ne)), cy=tuple(cy),
                       label=f'{x} reads {item} on {wires[k]}')
            if phase == 0:
                mk((1, sent, nobody, pab, ended), label=f'{x} begins its abort')
            elif phase == 1:
                for k in range(W):
                    if sent[k] or nobody[k]:
                        continue
                    if pab[k]:
                        nb = list(nobody)
                        nb[k] = True
                        mk((1, sent, tuple(nb), pab, ended), label=f'{x}: nobody to tell on {wires[k]}')
                    else:
                        sn = list(sent)
                        sn[k] = True
                        cx = list(chans[x])
                        cx[k] = cx[k] + ('ABORT',)
                        mk((1, tuple(sn), nobody, pab, ended), cx=tuple(cx),
                           label=f'{x} sends its abort on {wires[k]}')
                if all(sent[k] or nobody[k] for k in range(W)):
                    mk((2, sent, nobody, pab, ended), label=f'{x} waits for its peers')
            elif phase == 2:
                if variant == 'either_wire' and any(pab):
                    done = True
                else:
                    done = all(pab[k] or ended[k] for k in range(W))
                if done:
                    cx = tuple(c + ('EOF',) for c in chans[x])
                    mk((3, sent, nobody, pab, ended), cx=cx, label=f'{x} ends')
        return out

    seen = {start: None}
    edges = {}
    q = deque([start])
    while q:
        S = q.popleft()
        edges[S] = [T for T, _ in succ(S)]
        for T, label in succ(S):
            if T not in seen:
                seen[T] = (S, label)
                q.append(T)
    goal = {S for S in edges if all(n[0] == 3 for n in S[0])}
    rev = {}
    for S, ts in edges.items():
        for T in ts:
            rev.setdefault(T, []).append(S)
    good = set(goal)
    work = deque(goal)
    while work:
        T = work.popleft()
        for S in rev.get(T, ()):
            if S not in good:
                good.add(S)
                work.append(S)
    bad = [S for S in edges if S not in good]
    if not bad:
        return True, len(seen), []
    S = bad[0]
    tr = []
    while seen.get(S):
        S, label = seen[S]
        tr.append(label)
    return False, len(seen), list(reversed(tr))


def main():
    ok = True
    for variant, expect in (('one_wire', True), ('two_wires', False), ('either_wire', True)):
        held, states, tr = run(variant)
        print(f'carrier/{variant}: {"holds" if held else "FAILS: both carriers wait for each other"}'
              f' ({states} states)', flush=True)
        if tr:
            print('   trace: ' + ' ; '.join(tr))
        if expect is not None:
            ok &= held == expect
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
