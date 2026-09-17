#!/usr/bin/env python3
"""Checks that engine.py's state-space reductions change no verdict.

Runs small cases of every model twice: with the reductions (folded gpre/gpost
ghost steps, thread symmetry) and without them, where every ghost is a step of
its own and no states merge by symmetry. The verdicts must agree, and the
reduced run must not visit more states.

Usage: selftest.py
"""
import engine
import dirty_set_model as ds
import get_completion_model as gc
import watch_list_model as wl

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
