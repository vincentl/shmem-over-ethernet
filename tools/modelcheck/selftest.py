#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Checks that engine.py's state-space reductions change no verdict.

Runs small cases of every model twice: with the reductions (folded gpre/gpost
ghost steps, thread symmetry) and without them, where every ghost is a step of
its own and no states merge by symmetry. The verdicts must agree, and the
reduced run must not visit more states.

It also holds the engine's own checks to a case each must catch. The trapped-
state check (engine.py, "Liveness") is the one that has no other witness: a
wire reader that takes a byte and never publishes that it did has no state
without a successor, so LOST WAKE and STUCK never fire and the engine used to
report it `ok`. It must be TRAPPED.

Exits nonzero on any mismatch, so ctest can run it.

Usage: selftest.py
"""
import sys

import engine
import dirty_set_model as ds
import get_completion_model as gc
import watch_list_model as wl
import wire_model


def wire_reader_never_advances():
    """wire_model's reader with the `consumed` store removed: it re-reads byte 0 forever."""
    cap, nbytes = 2, 2
    B = [('R', ('load', lambda L: wire_model.CONS, 'c', '@RDONE?')),
         ('RDONE?', ('br', lambda L: L['c'] >= nbytes, '@REND', '@ORDER')),
         ('ORDER', ('load', lambda L: wire_model.WAKE, 'w', '@PROD')),
         ('PROD', ('load', lambda L: wire_model.PROD, 'p', '@HAVE?')),
         ('HAVE?', ('br', lambda L: L['p'] > L['c'], '@TAKE', '@SLEEP')),
         ('SLEEP', ('fwait', lambda L: wire_model.WAKE, lambda L: L['w'], '@R')),
         ('TAKE', ('load', lambda L: wire_model.SLOT(L['c'] % cap), 'v', '@R')),
         ('REND', ('done',))]
    threads = [wire_model.writer('design', nbytes, cap), engine.assemble(B)]
    mem = {wire_model.PROD: 0, wire_model.CONS: 0, wire_model.WAKE: 0}
    return engine.explore('x', threads, mem, out=sys.stderr)


# Cases a check must catch, with the verdict it must give.
MUST_FAIL = [
    ('trapped: wire reader never advances', wire_reader_never_advances, 'TRAPPED'),
]

CASES = [
    ('get: design, 1 issuer x 2 gets, 2 quiets', lambda: gc.run_res('x', 'design', [[(0, 1), (1, 2)]], 2)),
    ('get: design, 2 issuers, 2 quiets', lambda: gc.run_res('x', 'design', [[(0, 1)], [(1, 1)]], 2)),
    ('get: snapshot', lambda: gc.run_res('x', 'snapshot', [[(0, 2)], [(1, 1)]], 1)),
    ('get: noretag', lambda: gc.run_res('x', 'design', [[(0, 1)]], 2, 'noretag')),
    ('get: nolock', lambda: gc.run_res('x', 'design', [[(0, 1)]], 2, 'nolock')),
    ('dirty: design, 2 issuers x 1 put, 2 quiets', lambda: ds.run_res('x', [[0], [1]], 2)),
    ('dirty: design, 1 issuer x 3 puts, 2 quiets', lambda: ds.run_res('x', [[0, 1, 2]], 2)),
    ('dirty: unlocked', lambda: ds.run_res('x', [[0], [1]], 2, 'unlocked')),
    ('dirty: naive', lambda: ds.run_res('x', [[0, 1]], 2, 'naive')),
    ('dirty: bitfirst', lambda: ds.run_res('x', [[0, 1]], 2, 'bitfirst')),
    ('watch: design, one thread watches twice', lambda: wl.run_res('x', *wl.REUSE)),
    ('watch: firstmatch', lambda: wl.run_res('x', *wl.SAME, variant='firstmatch')),
]

if __name__ == '__main__':
    good = True
    for name, fn in CASES:
        results = []
        for reduce in (True, False):
            engine.REDUCE = reduce
            results.append(fn())
        (r, u) = results
        same = r.verdict == u.verdict and r.states <= u.states
        good &= same
        print(f'{name:<48} reduced {r.verdict:<9} {r.states:>9,} states | '
              f'unreduced {u.verdict:<9} {u.states:>9,} states  {"ok" if same else "MISMATCH"}', flush=True)
    print()
    print('REDUCTIONS PRESERVE EVERY VERDICT' if good else 'MISMATCH')
    print()
    for name, fn, want in MUST_FAIL:
        engine.REDUCE = True
        r = fn()
        caught = r.verdict == want
        good &= caught
        print(f'{name:<48} {r.verdict:<9} {r.states:>9,} states  '
              f'{"caught" if caught else "NOT CAUGHT, expected " + want}', flush=True)
    sys.exit(0 if good else 1)
