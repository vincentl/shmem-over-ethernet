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
report it `ok`. It must be TRAPPED. And a failed check must be named by its
message (engine.py, "Properties"): a writer that does not wait for room fails
BOUND, which `report` must take where a case names BOUND and refuse where it
names another property.

Exits nonzero on any mismatch, so ctest can run it.

Usage: selftest.py
"""
import io
import sys

import engine
import dirty_set_model as ds
import get_completion_model as gc
import watch_list_model as wl

# A ring for the engine's own case below: the writer of the shared-memory wire
# that phase 1 step 3 slice 1 replaced with a socket - py's, as it
# was when that model was retired with the wire (2026-10-12) - kept only as a
# subject whose reader can be broken.
PROD, CONS, WAKE = ('produced',), ('consumed',), ('wake',)
SLOT = lambda i: ('slot', i)            # the ring itself, cap bytes
SENT = lambda i: ('$sent', i)           # value of the i'th byte written


def ring_writer(nbytes, cap, wait=True):
    """Send nbytes bytes, values 1..nbytes so that 0 is 'nothing here'; blocked
    while the ring is full - unless not `wait`, the engine's own named case
    below; each byte stored, checked against overwriting, published, then
    woken."""
    B = []
    for k in range(nbytes):
        p = f'w{k}_'
        nxt = f'@w{k + 1}_L' if k + 1 < nbytes else '@WEND'
        B += [(p + 'L', ('load', lambda L: CONS, 'c', '@' + p + 'P')),
              (p + 'P', ('load', lambda L: PROD, 'p', '@' + p + 'R')),
              (p + 'R', ('await', lambda M, L, cap=cap, wait=wait:
                         not wait or M[PROD] - M[CONS] < cap, '@' + p + 'L2')),
              (p + 'L2', ('load', lambda L: CONS, 'c', '@' + p + 'P2')),
              (p + 'P2', ('load', lambda L: PROD, 'p', '@' + p + 'W')),
              (p + 'W', ('store', lambda L, cap=cap: SLOT(L['p'] % cap),
                         lambda L, k=k: k + 1, False, '@' + p + 'C')),
              (p + 'C', ('check', lambda M, L, cap=cap:
                         None if M[PROD] - M[CONS] < cap else
                         f'BOUND: wrote byte {L["p"]} over a full ring: {M[PROD] - M[CONS]} '
                         f'outstanding, capacity {cap}',
                         '@' + p + 'G')),
              (p + 'G', ('gpre', lambda M, L, k=k: ({SENT(k): k + 1}, {}), '@' + p + 'S')),
              (p + 'S', ('store', lambda L: PROD, lambda L: L['p'] + 1, False, '@' + p + 'K')),
              (p + 'K', ('add', lambda L: WAKE, lambda L: 1, '_', '@' + p + 'F')),
              (p + 'F', ('fwake', lambda L: WAKE, nxt))]
    B += [('WEND', ('done',))]
    return engine.assemble(B)


def wire_reader_never_advances():
    """The ring's reader with the `consumed` store removed: it re-reads byte 0 forever."""
    cap, nbytes = 2, 2
    B = [('R', ('load', lambda L: CONS, 'c', '@RDONE?')),
         ('RDONE?', ('br', lambda L: L['c'] >= nbytes, '@REND', '@ORDER')),
         ('ORDER', ('load', lambda L: WAKE, 'w', '@PROD')),
         ('PROD', ('load', lambda L: PROD, 'p', '@HAVE?')),
         ('HAVE?', ('br', lambda L: L['p'] > L['c'], '@TAKE', '@SLEEP')),
         ('SLEEP', ('fwait', lambda L: WAKE, lambda L: L['w'], '@R')),
         ('TAKE', ('load', lambda L: SLOT(L['c'] % cap), 'v', '@R')),
         ('REND', ('done',))]
    threads = [ring_writer(nbytes, cap), engine.assemble(B)]
    mem = {PROD: 0, CONS: 0, WAKE: 0}
    return engine.explore('x', threads, mem, out=sys.stderr)


def ring_overrun():
    """The ring's writer that does not wait for room, and no reader: its second
    byte overwrites the first."""
    threads = [ring_writer(2, 1, wait=False)]
    mem = {PROD: 0, CONS: 0, WAKE: 0}
    return engine.explore('x', threads, mem, out=sys.stderr)


# Cases a check must catch, with the property it must fail (engine.py, "Properties").
MUST_FAIL = [
    ('trapped: wire reader never advances', wire_reader_never_advances, 'TRAPPED'),
    ('named: ring writer does not wait', ring_overrun, 'BOUND'),
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
        caught = r.failed == want
        good &= caught
        print(f'{name:<48} {r.failed or r.verdict:<9} {r.states:>9,} states  '
              f'{"caught" if caught else "NOT CAUGHT, expected " + want}', flush=True)
    # report's comparison: the variant named for what it fails is expected, and
    # named for anything else - a property it does not fail, or none - is not.
    print()
    r = ring_overrun()
    for want, expected in (('BOUND', True), ('STUCK', False), (True, False)):
        judged = engine.report(r, expect=want, out=io.StringIO())
        good &= judged == expected
        print(f'report: ring writer named {str(want):<6} '
              f'{"expected" if judged else "UNEXPECTED"}  '
              f'{"right" if judged == expected else "WRONG"}', flush=True)
    sys.exit(0 if good else 1)
