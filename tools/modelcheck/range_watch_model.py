#!/usr/bin/env python3
"""The §4.9 watch slot, one per local PE, watching a byte range (design §4.9).

Phase 1 provides up to SHMEM_THREAD_SERIALIZED, so a PE is in at most one wait
at a time and has one slot. What it must watch is a *range*, not one offset,
because shmem_wait_until_{all,any,some} and their vector forms wait on an array
(spec §9.11.2-§9.11.7).

  blk[p]   one word: the watched range, packed as region | first grain |
           saturating grain count (shmem/src/watch_range.h); 0 is none
  seq[p]   the 32-bit futex word

Waiter (PE p):  store blk[p] (seq_cst, before the first check);
                loop { load seq[p]; load the variables; if the predicate holds,
                stop; futex_wait(seq[p]) };
                store blk[p] = 0 (seq_cst).
Writer (another local PE, or the progress thread applying a remote operation):
                update the variable with a locked AMO; load blk[p]; if the range
                covers the offset written, add 1 to seq[p] and futex_wake it.

One word is published and read atomically, so no combination of a stale half can
be read, and the writer's test is two shifts and two compares. Elements here are
one grain apart, so a predicate that covers a range's ends and not its middle is
reachable - see the `endpoints` variant. An earlier draft's cases were all
within one grain, which is exactly why it could not see that.

docs/design/evidence/watch-options/ is the separate question of which coverage
predicate to use; this model is the protocol around whichever one is chosen.

Checked: no lost wake. Every waiter's predicate becomes true in every run, so a
terminal state with a PE asleep is a missed wake.

Variants
  tworange     the alternative: two words (offset, length), stored and loaded
               separately. Expected to hold too, for the reason in §4.9: the
               writer updates the variable before it reads the slot, so if it
               reads the new offset the update is already visible to the
               waiter's next check
  endpoints    a predicate covering only a range's two ends. It passes every
               scalar case and every two-element case, and loses a wake on a
               three-element array: the demonstration that these cases can see
               a hole in the middle of a range
  checkfirst   the waiter checks the variables before publishing its range
  notifyfirst  the writer reads the slot before updating the variable
  plainblk     the waiter publishes with a plain store (TSO)
  plainput     the writer updates with a plain store, as a put's memcpy does
               (TSO); §4.10 makes notify for puts best effort for this reason

Usage: range_watch_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

VAR = lambda o: ('var', o)
BLK = ('blk',)
LO = ('lo',)
LEN = ('len',)
SEQ = ('seq',)


# shmem/src/watch_range.h, mirrored. An element of the model is one grain, so
# element e lives at byte offset e * GRAIN.
GRAIN, LEN_BITS, OFF_BITS = 4, 22, 40
LEN_MAX = (1 << LEN_BITS) - 1
OFF_MAX = (1 << OFF_BITS) - 1


def enc(lo, n, variant=''):
    """Pack the range of n elements starting at element lo."""
    first, last = lo, lo + n - 1
    grains = last - first + 1
    if variant == 'endpoints':
        # Covers the two ends and not the middle: sound only for n <= 2.
        return ('ends', first, last)
    if first > OFF_MAX:
        return LEN_MAX
    if grains >= LEN_MAX:
        return (first << LEN_BITS) | LEN_MAX
    return (first << LEN_BITS) | grains


def in_block(word, o, variant=''):
    if variant == 'endpoints':
        return word != 0 and o in (word[1], word[2])
    if word == 0:
        return False
    wlen = word & LEN_MAX
    wfirst = (word >> LEN_BITS) & OFF_MAX
    wlast = (1 << 64) if wlen == LEN_MAX else wfirst + wlen - 1
    return o <= wlast and wfirst <= o


def waiter(waits, variant=''):
    """waits: [(lo, n)]; each predicate is 'every element of the range >= 1'."""
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

        # publish, or - in the checkfirst variant - test once and then publish
        after_publish = p + 'L0'
        if variant == 'checkfirst':
            # Test, then publish, then sleep: the one ordering §4.9 calls easy
            # to get backwards. A wake landing in the window is lost.
            B += [(p + 'A', ('br', lambda L: True, '@' + p + 'F0', '@' + p + 'F0'))]
            B += loads('F', 'FC')
            B += [(p + 'FC', ('br', pred, '@' + p + 'FX', '@' + p + 'FK')),
                  (p + 'FX', ('gpost', lambda M, L, f=forget: ({}, f), nxt)),
                  (p + 'FK', ('gpost', lambda M, L, f=forget: ({}, f), '@' + p + 'P0')),
                  (p + 'Z0', ('load', lambda L: SEQ, 'q', '@' + p + 'S'))]
            after_publish = p + 'Z0'
        else:
            B += [(p + 'A', ('br', lambda L: True, '@' + p + 'P0', '@' + p + 'P0'))]

        if variant == 'tworange':
            B += [(p + 'P0', ('store', lambda L: LO, lambda L, lo=lo: lo, True, '@' + p + 'P0b')),
                  (p + 'P0b', ('store', lambda L: LEN, lambda L, n=n: n, True,
                               '@' + after_publish))]
        else:
            B += [(p + 'P0', ('store', lambda L: BLK,
                              lambda L, lo=lo, n=n, v=variant: enc(lo, n, v),
                              variant != 'plainblk', '@' + after_publish))]

        B += [(p + 'L0', ('load', lambda L: SEQ, 'q', '@' + p + 'V0'))]
        B += loads('V', 'C')
        B += [(p + 'C', ('br', pred, '@' + p + 'U0', '@' + p + 'K0')),
              (p + 'K0', ('gpost', lambda M, L, f=forget: ({}, f), '@' + p + 'S')),
              (p + 'S', ('fwait', lambda L: SEQ, lambda L: L['q'], '@' + p + 'L0')),
              (p + 'U0', ('gpost', lambda M, L, f=forget: ({}, {'q': DEL, **f}), '@' + p + 'U1'))]
        if variant == 'tworange':
            B += [(p + 'U1', ('store', lambda L: LO, lambda L: 0, True, '@' + p + 'U2')),
                  (p + 'U2', ('store', lambda L: LEN, lambda L: 0, True, nxt))]
        else:
            B += [(p + 'U1', ('store', lambda L: BLK, lambda L: 0, True, nxt))]
    B.append(('END', ('done',)))
    return assemble(B)


def writer(o, variant=''):
    """Writes element o, then wakes the slot if its block covers o."""
    update = [('X0', ('add', lambda L: VAR(o), lambda L: 1, '_', '@X1'))]
    if variant == 'plainput':
        update = [('X0', ('load', lambda L: VAR(o), 'v', '@X0b')),
                  ('X0b', ('store', lambda L: VAR(o), lambda L: L['v'] + 1, False, '@X0c')),
                  ('X0c', ('set', 'v', lambda L: DEL, '@X1'))]

    if variant == 'tworange':
        scan = [('X1', ('load', lambda L: LO, 'lo', '@X1b')),
                ('X1b', ('load', lambda L: LEN, 'n', '@X2')),
                ('X2', ('br', lambda L: L['n'] and L['lo'] <= o < L['lo'] + L['n'],
                        '@X3', '@END'))]
    else:
        scan = [('X1', ('load', lambda L: BLK, 'x', '@X2')),
                ('X2', ('br', lambda L, v=variant: in_block(L['x'], o, v), '@X3', '@END'))]

    scan += [('X3', ('add', lambda L: SEQ, lambda L: 1, '_', '@X4')),
             ('X4', ('fwake', lambda L: SEQ, '@END'))]

    if variant == 'notifyfirst':
        scan = [(lab, tuple('@X0' if x == '@END' else x for x in ins)) for lab, ins in scan]
        update = [(lab, tuple('@DONE' if x == '@X1' else x for x in ins)) for lab, ins in update]
        B = scan + update + [('DONE', ('done',))]
        B.insert(0, ('START', ('br', lambda L: True, '@X1', '@X1')))
    else:
        B = update + scan + [('END', ('done',))]
    return assemble(B)


def run(name, waits, writes, tso=False, variant='', expect_ok=None):
    threads = [waiter(waits, variant)] + [writer(o, variant) for o in writes]
    res = explore(name, threads, {BLK: 0}, tso=tso)
    return report(res, expect_ok=expect_ok, tso=tso)


# (the ranges one PE waits on in turn, the elements written)
SCALAR = ([(0, 1)], [0])                    # one variable, one writer
TWO_WRITERS = ([(0, 1)], [0, 0])            # two writers, e.g. a local PE and the progress thread
ARRAY = ([(0, 3)], [0, 1, 2])               # wait_until_all over three elements
STRADDLE = ([(3, 2)], [3, 4])               # [3,5) rounds out to the block [0,8)
REUSE = ([(0, 1), (5, 1)], [0, 5])          # one wait, then another on a different element

CASES = [
    # name, (waits, writes), tso, variant, expected to pass
    ('design: one variable', SCALAR, False, '', True),
    ('design: one variable', SCALAR, True, '', True),
    ('design: two writers', TWO_WRITERS, False, '', True),
    ('design: two writers', TWO_WRITERS, True, '', True),
    ('design: array wait', ARRAY, False, '', True),
    ('design: array wait', ARRAY, True, '', True),
    ('design: range straddling a block boundary', STRADDLE, False, '', True),
    ('design: range straddling a block boundary', STRADDLE, True, '', True),
    ('design: two waits in turn', REUSE, False, '', True),
    ('design: two waits in turn', REUSE, True, '', True),
    ('tworange: two waits in turn', REUSE, False, 'tworange', True),
    ('tworange: two waits in turn', REUSE, True, 'tworange', True),
    ('tworange: array wait', ARRAY, True, 'tworange', True),
    ('checkfirst: one variable', SCALAR, False, 'checkfirst', False),
    ('notifyfirst: one variable', SCALAR, False, 'notifyfirst', False),
    ('plainblk: one variable', SCALAR, True, 'plainblk', False),
    ('plainput: one variable', SCALAR, True, 'plainput', False),
    # A predicate that covers only a range's ends passes every case an earlier
    # draft of this model had, and loses a wake on a three-element array. That
    # is what makes these cases able to see a hole in the middle of a range.
    ('endpoints: one variable (passes)', SCALAR, False, 'endpoints', True),
    ('endpoints: range straddling a boundary (passes)', STRADDLE, False, 'endpoints', True),
    ('endpoints: array wait', ARRAY, False, 'endpoints', False),
]

if __name__ == '__main__':
    want = select(sys.argv)
    good = True
    print('== can a wake be lost with one watch slot per PE over a byte range?', flush=True)
    for name, (waits, writes), tso, variant, expect in CASES:
        if want(name):
            good &= run(name, waits, writes, tso, variant, expect)
    print()
    print('ALL AS EXPECTED' if good else 'SOME RESULT UNEXPECTED OR INCOMPLETE')
