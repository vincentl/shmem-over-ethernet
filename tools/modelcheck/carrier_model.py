#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Two concurrent shmem_global_exit callers on two nodes, each its node's
carrier (abort.h; design §3.15, §6.4; v0.2 step 4, Part 2; P1).

The carrier rule - progress-host.c's osoe_progress_host_abort; progress.c's
osoe_progress_outranked, osoe_progress_peers_ended, flush_aborting and
flush_control_aborting - with one wire a pair and with two, written by the
second adversarial read of Part 2 (S1) and made the job's single carrier's by
P1 (2026-10-30). abort_exit_model.py has one wire a pair, which hides what two
make of the order goodbyes arrive in.

Each node's PE has declared its own global exit (its node's abort word names its
own PE, so it is a carrier); node 0's caller is ranked below node 1's. Its
agent then aborts: for each wire, in any order (a data wire's send may wait
behind a segment in flight), it sends its abortive goodbye - unless the peer's
has already arrived on that wire naming a caller not ranked above its own,
when it has nobody to tell: node 1 tells nobody where node 0's has arrived,
node 0 tells node 1 even there (progress.c's peer_outranked_here). Then, a
carrier, it waits: outranked - a goodbye from the lower caller's node arrived on
a wire it judges - it waits no longer; otherwise until every wire it judges has
ended. Its process then ends, which ends its streams (keep_stream: the end of
stream comes with the process's end).

Checked: from every state, a state where both nodes have ended is reachable
(AG EF), the bound OSOE_ABORT_LEAD_WAIT_SECONDS left out. A state from which it
is not is two carriers each waiting for the other: both run to the bound.

Variants
  one_wire        one connection a pair, as before v0.2 step 4's Part 2.
  two_wires       two, with no helpers: one owner holds both of a peer's wires,
                  so a goodbye from the lower caller on either outranks it, and
                  it otherwise waits for both to end.
  control_only    two, with helpers (v0.2 step 4, Part 3): the data wire is a
                  helper's, whose sends the lead's process waits for - so a node
                  waits once every wire's send or silence is done, as here - and
                  the lead judges the peers by their control wires alone, the
                  wires it holds: the lower caller's goodbye on its control
                  wire, or its control wire's end. A process's end ends all its
                  streams at once, so a control wire's end says what a data
                  wire's would; and the lower carrier's lead tells the higher on
                  its control wire whatever arrived first.
  old_send, old_send_one_wire
                  the send before P1, nothing to a peer whose goodbye has
                  arrived: node 1's arriving first on every wire, node 0 tells
                  it nothing, and each waits for the other (a breakage, as
                  abort_exit_model.py's single_old_send).

Until P1 the rule was each carrier its own node's, a node excluding a peer whose
goodbye named another caller: per wire (two_wires as first built) that failed
LIVE_END, and judged on either wire, or on the control wire with helpers, it
held; its variants went with it.
"""
import sys
from collections import deque

import engine


def run(variant):
    wires = ('D',) if variant in ('one_wire', 'old_send_one_wire') else ('C', 'D')
    old_send = variant.startswith('old_send')
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
            lower = y < x               # the peer's caller ranked below this node's

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
                    if pab[k] and (old_send or not y > x):
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
                if variant == 'control_only':
                    done = (lower and pab[0]) or ended[0]
                else:
                    done = (lower and any(pab)) or all(ended)
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
    for variant, expect in (('one_wire', True), ('two_wires', True), ('control_only', True),
                            ('old_send', 'LIVE_END'), ('old_send_one_wire', 'LIVE_END')):
        held, states, tr = run(variant)
        failure = None if held else 'LIVE_END: both carriers wait for each other'
        wrong = engine.judge(held, engine.named(failure), expect)
        mark = '' if wrong is None else '   <-- UNEXPECTED' + wrong
        print(f'carrier/{variant}: {"holds" if held else "FAILS: " + failure}'
              f' ({states} states){mark}', flush=True)
        if tr:
            print('   trace: ' + ' ; '.join(tr))
        ok &= wrong is None
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
