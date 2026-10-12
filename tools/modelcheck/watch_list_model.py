#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Per-thread watch records for one PE under SHMEM_THREAD_MULTIPLE (design §4.11).

Replaces the §4.9 single watch slot per PE when several threads of the PE wait at
once. Each application thread owns one record in the node segment, found by the
thread's index, never by a key:

  rng[r]   one word: the watched byte range, (offset, length) packed; 0 is none
  seq[r]   the 32-bit futex word
  act      a bitmap of the PE's records that hold a published range

Waiter (thread r):   store rng[r] (seq_cst); set bit r in act (locked);
                     loop { load seq[r]; load the variables; if the predicate
                     holds, stop; futex_wait(seq[r]) };
                     clear bit r (locked); store rng[r] = 0 (seq_cst).
Writer (any thread of any local PE, or the progress thread):
                     update the variable with a locked AMO; load act; for each
                     set bit r: load rng[r], and if it overlaps the bytes
                     written, add 1 to seq[r] and futex_wake it. Every matching
                     record, not the first.

With no key there is no sharing between waiters, so the review's two slot-table
defects (V8, a reference count cleared under a joiner; V9, a probe hole) cannot
arise. What is new, and checked here: the bitmap as the writer's filter, several
waiters on overlapping ranges, a range spanning array elements (the
multiple-element waits), a thread that watches one range and then another
(reuse of its record), and the x86-TSO memory model.

Checked: no lost wake. Every waiter's predicate becomes true in every run, so a
terminal state with a thread asleep is a missed wake.

Breakages
  firstmatch   the writer stops after the first matching record
  checkfirst   the waiter loads the variables before publishing
  notifyfirst  the writer loads act before it updates the variable
  plainbit     the waiter sets its bit with a plain load and store (TSO)
  plainrng     the waiter publishes its range with a plain store (TSO)
  plainput     the writer updates the variable with a plain store, like a
               put's memcpy (TSO); §4.10 accepts this as best effort
  tworange     the range is two words (offset, then length), stored and loaded
               separately

Usage: watch_list_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

ACT = ('act',)
VAR = lambda o: ('var', o)
RNG = lambda r: ('rng', r)
LO = lambda r: ('lo', r)
LEN = lambda r: ('len', r)
SEQ = lambda r: ('seq', r)


def enc(lo, n):
    return lo * 8 + n              # n >= 1, so a published range is never 0


def overlaps(x, o):
    if x == 0:
        return False
    lo, n = divmod(x, 8)
    return lo <= o < lo + n


def waiter(r, waits, variant=''):
    """waits: [(lo, n)]; each wait's predicate is 'every element in range >= 1'."""
    bit = 1 << r
    B = []
    for w, (lo, n) in enumerate(waits):
        p = f'w{w}_'
        nxt = f'@w{w + 1}_A' if w + 1 < len(waits) else '@END'
        pred = lambda L, n=n: all(L[f'v{e}'] >= 1 for e in range(n))
        forget = {f'v{e}': DEL for e in range(n)}

        def loads(tag, then, lo=lo, n=n, p=p):
            return [(p + f'{tag}{e}', ('load', lambda L, o=lo + e: VAR(o), f'v{e}',
                                      '@' + p + (f'{tag}{e + 1}' if e + 1 < n else then)))
                    for e in range(n)]

        if variant == 'checkfirst':
            # the review's V1 order: test once, publish, then sleep before re-testing
            B += [(p + 'A', ('br', lambda L: True, '@' + p + 'F0', '@' + p + 'F0'))]
            B += loads('F', 'FC')
            B += [(p + 'FC', ('br', pred, '@' + p + 'FX', '@' + p + 'FK')),
                  (p + 'FX', ('gpost', lambda M, L, f=forget: ({}, f), nxt)),
                  (p + 'FK', ('gpost', lambda M, L, f=forget: ({}, f), '@' + p + 'P0'))]
            after_publish = p + 'Z0'
            B += [(p + 'Z0', ('load', lambda L: SEQ(r), 'q', '@' + p + 'S'))]
        else:
            B += [(p + 'A', ('br', lambda L: True, '@' + p + 'P0', '@' + p + 'P0'))]
            after_publish = p + 'L0'
        if variant == 'tworange':
            B += [(p + 'P0', ('store', lambda L: LO(r), lambda L, lo=lo: lo, True, '@' + p + 'P0b')),
                  (p + 'P0b', ('store', lambda L: LEN(r), lambda L, n=n: n, True, '@' + p + 'P1'))]
        else:
            B += [(p + 'P0', ('store', lambda L: RNG(r), lambda L, lo=lo, n=n: enc(lo, n),
                              variant != 'plainrng', '@' + p + 'P1'))]
        if variant == 'plainbit':
            B += [(p + 'P1', ('load', lambda L: ACT, 'a', '@' + p + 'P2')),
                  (p + 'P2', ('store', lambda L: ACT, lambda L: L['a'] | bit, False, '@' + p + 'P3')),
                  (p + 'P3', ('set', 'a', lambda L: DEL, '@' + after_publish))]
        else:
            B += [(p + 'P1', ('or', lambda L: ACT, lambda L: bit, '_', '@' + after_publish))]
        B += [(p + 'L0', ('load', lambda L: SEQ(r), 'q', '@' + p + 'V0'))]
        B += loads('V', 'C')
        B += [(p + 'C', ('br', pred, '@' + p + 'U0', '@' + p + 'K0')),
              (p + 'K0', ('gpost', lambda M, L, f=forget: ({}, f), '@' + p + 'S')),
              (p + 'S', ('fwait', lambda L: SEQ(r), lambda L: L['q'], '@' + p + 'L0')),
              (p + 'U0', ('gpost', lambda M, L, f=forget: ({}, {'q': DEL, **f}), '@' + p + 'U1')),
              (p + 'U1', ('andn', lambda L: ACT, lambda L: bit, '_', '@' + p + 'U2'))]
        if variant == 'tworange':
            B += [(p + 'U2', ('store', lambda L: LO(r), lambda L: 0, True, '@' + p + 'U3')),
                  (p + 'U3', ('store', lambda L: LEN(r), lambda L: 0, True, nxt))]
        else:
            B += [(p + 'U2', ('store', lambda L: RNG(r), lambda L: 0, True, nxt))]
    B.append(('END', ('done',)))
    return assemble(B)


def writer(o, nrec, variant=''):
    update = [('X0', ('add', lambda L: VAR(o), lambda L: 1, '_', '@X1'))]
    if variant == 'plainput':
        update = [('X0', ('load', lambda L: VAR(o), 'v', '@X0b')),
                  ('X0b', ('store', lambda L: VAR(o), lambda L: L['v'] + 1, False, '@X0c')),
                  ('X0c', ('set', 'v', lambda L: DEL, '@X1'))]
    scan = [('X1', ('load', lambda L: ACT, 'b', '@X2')),
            ('X2', ('set', 'r', lambda L: 0, '@N0')),
            ('N0', ('br', lambda L: L['r'] < nrec, '@N1', '@END')),
            ('N1', ('br', lambda L: L['b'] >> L['r'] & 1, '@N2', '@N9'))]
    if variant == 'tworange':
        scan += [('N2', ('load', lambda L: LO(L['r']), 'lo', '@N2b')),
                 ('N2b', ('load', lambda L: LEN(L['r']), 'n', '@N3')),
                 ('N3', ('br', lambda L: L['n'] and L['lo'] <= o < L['lo'] + L['n'], '@N4', '@N9'))]
    else:
        scan += [('N2', ('load', lambda L: RNG(L['r']), 'x', '@N3')),
                 ('N3', ('br', lambda L: overlaps(L['x'], o), '@N4', '@N9'))]
    scan += [('N4', ('add', lambda L: SEQ(L['r']), lambda L: 1, '_', '@N5')),
             ('N5', ('fwake', lambda L: SEQ(L['r']), '@END' if variant == 'firstmatch' else '@N9')),
             ('N9', ('gpost', lambda M, L: ({}, {'r': L['r'] + 1, 'x': DEL, 'lo': DEL, 'n': DEL}), '@N0'))]
    if variant == 'notifyfirst':
        scan = [(lab, tuple('@X0' if x == '@END' else x for x in ins)) for lab, ins in scan]
        update = [(lab, tuple('@DONE' if x == '@X1' else x for x in ins)) for lab, ins in update]
        B = scan + update + [('DONE', ('done',))]
        B.insert(0, ('START', ('br', lambda L: True, '@X1', '@X1')))
    else:
        B = update + scan + [('END', ('done',))]
    return assemble(B)


def run(name, waiters, writes, tso=False, variant='', expect=None):
    return report(run_res(name, waiters, writes, tso, variant), expect=expect, tso=tso)


def run_res(name, waiters, writes, tso=False, variant=''):
    nrec = len(waiters)
    threads = [waiter(r, waits, variant) for r, waits in enumerate(waiters)]
    threads += [writer(o, nrec, variant) for o in writes]
    res = explore(name, threads, {ACT: 0}, tso=tso)
    return res


# waiters: per thread, the ranges it waits on in turn; writes: one writer per element written
TWO_OVERLAP = ([[(0, 2)], [(1, 1)]], [0, 1])          # an array wait and a scalar wait sharing element 1
SAME = ([[(0, 1)], [(0, 1)]], [0])                     # two threads on one variable
REUSE = ([[(0, 1), (1, 1)], [(1, 1)]], [0, 1])         # thread 0 watches element 0, then element 1
THREE = ([[(0, 2)], [(1, 1)], [(0, 1)]], [0, 1])       # three records

CASES = [
    # name, (waiters, writes), tso, variant, expect: True, the property it must fail, or None
    ('design: array and scalar waits overlap', TWO_OVERLAP, False, '', True),
    ('design: array and scalar waits overlap', TWO_OVERLAP, True, '', True),
    ('design: two threads on one variable', SAME, False, '', True),
    ('design: two threads on one variable', SAME, True, '', True),
    ('design: one thread watches twice', REUSE, False, '', True),
    ('design: one thread watches twice', REUSE, True, '', True),
    ('design: three records', THREE, False, '', True),
    ('design: three records', THREE, True, '', True),
    ('firstmatch: two threads on one variable', SAME, False, 'firstmatch', 'LOST WAKE'),
    ('checkfirst: array and scalar waits overlap', TWO_OVERLAP, False, 'checkfirst', 'LOST WAKE'),
    ('notifyfirst: array and scalar waits overlap', TWO_OVERLAP, False, 'notifyfirst', 'LOST WAKE'),
    ('plainbit: array and scalar waits overlap', TWO_OVERLAP, True, 'plainbit', 'LOST WAKE'),
    ('plainrng: array and scalar waits overlap', TWO_OVERLAP, True, 'plainrng', None),
    ('plainput: array and scalar waits overlap', TWO_OVERLAP, True, 'plainput', 'LOST WAKE'),
    ('tworange: one thread watches twice', REUSE, False, 'tworange', None),
    ('tworange: one thread watches twice', REUSE, True, 'tworange', None),
]

if __name__ == '__main__':
    want = select(sys.argv)
    good = True
    print('== is any wake lost when several threads of one PE wait at once?', flush=True)
    for name, (waiters, writes), tso, variant, expect in CASES:
        if want(name):
            good &= run(name, waiters, writes, tso, variant, expect)
    print()
    print('ALL AS EXPECTED' if good else 'SOME RESULT UNEXPECTED OR INCOMPLETE')
    sys.exit(0 if good else 1)
