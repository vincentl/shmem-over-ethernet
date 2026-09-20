#!/usr/bin/env python3
"""Extra checks on the recorded models, run from the review's evidence directory.

1. AG EF termination. engine.explore() reports LOST WAKE / STUCK only at states
   with *no* successor. A state graph in which one thread sleeps forever while
   another can cycle forever has no such state and is reported `ok`. This script
   builds the full reachable graph of each small design case and checks that a
   good terminal state (every thread at 'done' or idle_ok) is reachable from
   EVERY reachable state.
2. engine blind spot, demonstrated: a wire reader that never advances `consumed`.
3. arrival_model's comment claims its TSO rows "would fail" were the wake bump a
   plain store. Tried here.
4. arrival_model's order() check has a 'lost' branch. Is it reachable?

Usage: python3 liveness-and-variants.py   (imports tools/modelcheck read-only)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '../../../../../../tools/modelcheck')))
sys.dont_write_bytecode = True

import engine                                   # noqa: E402
from engine import DEL, assemble, Explorer, explore, report, RUN, SLEEP   # noqa: E402
import wire_model, credit_model, arrival_model, blocked_record_model, range_watch_model  # noqa: E402


def graph_liveness(name, threads, mem0, tso=False, symmetric=(), limit=2_000_000):
    ex = Explorer(threads, mem0, tso, symmetric)
    index = {ex.init: 0}
    states = [ex.init]
    preds = [[]]
    good = []
    work = [0]
    while work:
        n = work.pop()
        succ, bad = ex.successors(states[n])
        if bad is not None:
            print(f'{name:<60} check failed during graph build: {bad}')
            return False
        if not succ:
            mem, ths = states[n]
            if all(ex.progs[i][pc][0] == 'done' for i, (pc, _, st, _, _) in enumerate(ths)):
                good.append(n)
            continue
        for _, _, ns in succ:
            ns = ex._canon(ns)
            m = index.get(ns)
            if m is None:
                m = index[ns] = len(states)
                states.append(ns)
                preds.append([])
                work.append(m)
                if m > limit:
                    print(f'{name:<60} too large')
                    return False
            preds[m].append(n)
    can = set(good)
    work = list(good)
    while work:
        n = work.pop()
        for p in preds[n]:
            if p not in can:
                can.add(p)
                work.append(p)
    trapped = len(states) - len(can)
    print(f'{name:<60} {"TSO" if tso else "SC "} states={len(states):>8,} good terminals={len(good):>4} '
          f'states that cannot reach termination={trapped}')
    return trapped == 0


def liveness_all():
    print('== 1. can every reachable state of each design still terminate?')
    ok = True
    for design, nbytes, cap in wire_model.CASES:
        if design == 'design':
            name, threads, mem0 = wire_model.case(design, nbytes, cap)
            ok &= graph_liveness(name, threads, mem0)
    for design, npes, nops, C, bound, nbi, expect in credit_model.CASES:
        if design == 'design':
            name, threads, mem0, sym = credit_model.case(design, npes, nops, C, bound, nbi)
            ok &= graph_liveness(name, threads, mem0, symmetric=sym)
    for design, members, epochs, tso, expect in arrival_model.CASES:
        if design == 'design':
            name, threads, mem0, sym = arrival_model.case(design, members, epochs)
            ok &= graph_liveness(name, threads, mem0, tso=tso, symmetric=sym)
    for name, (waits, writes), tso, variant, expect in range_watch_model.CASES:
        if variant == '':
            threads = [range_watch_model.waiter(waits, variant)] + \
                      [range_watch_model.writer(o, variant) for o in writes]
            ok &= graph_liveness('range_watch/' + name, threads, {range_watch_model.BLK: 0}, tso=tso)
    print('   every design state can terminate' if ok else '   SOME DESIGN STATE IS TRAPPED')
    return ok


def wire_noadvance():
    """A reader that takes a byte and never publishes `consumed`: it re-reads byte 0 forever."""
    print('== 2. engine blind spot: a wire reader that never advances')
    PROD, CONS, WAKE, SLOT, SENT = (wire_model.PROD, wire_model.CONS, wire_model.WAKE,
                                    wire_model.SLOT, wire_model.SENT)
    cap, nbytes = 2, 2
    B = [('R', ('load', lambda L: CONS, 'c', '@RDONE?')),
         ('RDONE?', ('br', lambda L: L['c'] >= nbytes, '@REND', '@ORDER')),
         ('ORDER', ('load', lambda L: WAKE, 'w', '@PROD')),
         ('PROD', ('load', lambda L: PROD, 'p', '@HAVE?')),
         ('HAVE?', ('br', lambda L: L['p'] > L['c'], '@TAKE', '@SLEEP')),
         ('SLEEP', ('fwait', lambda L: WAKE, lambda L: L['w'], '@R')),
         ('TAKE', ('load', lambda L: SLOT(L['c'] % cap), 'v', '@CHECK')),
         ('CHECK', ('check', lambda M, L: None if L['v'] == M[SENT(L['c'])] else 'mismatch', '@R')),
         ('REND', ('done',))]
    threads = [wire_model.writer('design', nbytes, cap), assemble(B)]
    mem0 = {PROD: 0, CONS: 0, WAKE: 0}
    res = explore('wire/noadvance bytes=2 cap=2 (reader delivers byte 0 forever)', threads, mem0)
    report(res)
    graph_liveness('wire/noadvance bytes=2 cap=2, AG EF termination', threads, mem0)


def arrival_plainwake():
    print('== 3. arrival: the wake bump as a plain load+store, under TSO and SC')
    A = arrival_model

    def arriver(epochs, members):
        B = []
        for k in range(epochs):
            e, p = k + 1, f'a{k}_'
            nxt = f'@a{k + 1}_A' if k + 1 < epochs else '@AEND'
            B += [(p + 'A', ('add', lambda L: A.ARRIVED, lambda L: 1, 'n', '@' + p + 'T')),
                  (p + 'T', ('br', lambda L, e=e, m=members: L['n'] == e * m, '@' + p + 'C', '@' + p + 'W')),
                  (p + 'C', ('check', lambda M, L: None if M[A.POSTED] == M[A.CONSUMED] else 'posted over', '@' + p + 'G')),
                  (p + 'G', ('gpre', lambda M, L: ({A.POSTED: M[A.POSTED] + 1}, {}), '@' + p + 'S')),
                  (p + 'S', ('store', lambda L: A.HANDOFF, lambda L, e=e: e, False, '@' + p + 'K')),
                  # the bump: a plain load and a plain store instead of a locked add
                  (p + 'K', ('load', lambda L: A.WAKE, 'wk', '@' + p + 'K2')),
                  (p + 'K2', ('store', lambda L: A.WAKE, lambda L: L['wk'] + 1, False, '@' + p + 'K3')),
                  (p + 'K3', ('set', 'wk', lambda L: DEL, '@' + p + 'F')),
                  (p + 'F', ('fwake', lambda L: A.WAKE, '@' + p + 'W')),
                  (p + 'W', ('set', 'n', lambda L: DEL, '@' + p + 'V')),
                  (p + 'V', ('await', lambda M, L, e=e: M[A.RELEASED] == e, nxt))]
        B += [('AEND', ('done',))]
        return assemble(B)

    for members, epochs, tso in ((2, 2, False), (2, 2, True), (3, 2, True), (2, 3, True)):
        one = arriver(epochs, members)
        threads = [one] * members + [A.lead('design', epochs), A.root(epochs)]
        mem = {A.ARRIVED: 0, A.RELEASED: 0, A.HANDOFF: 0, A.WAKE: 0, A.FORWARDED: 0}
        sym = (list(range(members)),) if members > 1 else ()
        res = explore(f'arrival/plainwake members={members} epochs={epochs}', threads, mem, tso=tso, symmetric=sym)
        report(res, tso=tso)


def arrival_lost_branch():
    print("== 4. arrival: is order()'s 'lost' branch reachable? consume_reread with the POSTED check removed")
    A = arrival_model
    for members, epochs in ((2, 2), (2, 3), (3, 3)):
        one = A.arriver('consume_reread', epochs, members)
        one = [('check', (lambda M, L: None), ins[2]) if ins[0] == 'check' else ins for ins in one]
        threads = [one] * members + [A.lead('consume_reread', epochs), A.root(epochs)]
        mem = {A.ARRIVED: 0, A.RELEASED: 0, A.HANDOFF: 0, A.WAKE: 0, A.FORWARDED: 0}
        sym = (list(range(members)),) if members > 1 else ()
        res = explore(f'arrival/consume_reread-no-posted-check members={members} epochs={epochs}',
                      threads, mem, symmetric=sym)
        report(res)


if __name__ == '__main__':
    liveness_all()
    print()
    wire_noadvance()
    print()
    arrival_plainwake()
    print()
    arrival_lost_branch()
