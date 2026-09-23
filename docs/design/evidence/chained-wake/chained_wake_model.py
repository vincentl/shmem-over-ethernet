#!/usr/bin/env python3
"""The chained release wake (design §5.3, base/epoch.c; phase 1 step 2 slice 3, E).

A team's release on a node used to be one futex_wake(INT_MAX) on the release
counter, issued by the releaser - on a multi-node job the node's progress
thread, which is the node's network: O(L) kernel work there per barrier. The
chained wake has the releaser wake 2, and every waiter that is woken and finds
its target reached wakes 2 more before returning. Depth O(log L); the progress
thread does O(1).

**The premise the design was first argued on is false, and this model is where
that was found.** The argument was that every waiter on a `released` word waits
for the same value, by §5.4's bounded skew. It does not hold: a member woken
early by the chain returns, runs to the next barrier, arrives, and sleeps on the
*same* word waiting for k+2 while members still asleep wait for k+1. A wake
meant for epoch k+1 can then pick it; it finds its own target unreached, goes
back to sleep, and passes nothing on - and if both of a wake's two picks are
such members, the chain ends with a member stranded until the safety sweep
(`unmasked` below).

Bounded skew does limit the targets to **two**, k+1 and k+2, so they are told
apart by parity: a waiter for target t sleeps with futex2 bitset 1 << (t & 1),
and a chained wake for epoch e wakes only bitset 1 << (e & 1). A next-epoch
waiter is then never picked by this epoch's chain, every waiter a chain wakes
has its target reached and passes the wake on, and none is stranded:

  - a waiter that arrives late loads the word before sleeping and never sleeps,
    and one whose load raced the release is refused by the kernel's compare;
  - the word cannot move past e while any member still waits for e, because the
    next release needs that member's own next arrival;
  - so the chain's frontier is non-empty for as long as a waiter for e sleeps.

Designs
  design      parity bitsets; the releaser wakes 2, and every waiter whose wait
              returned and whose target is reached wakes 2 more.
Breakages
  unmasked    the same chain with no bitsets: a next-epoch waiter absorbs a
              wake and a member is stranded. -> LOST WAKE
  no_pass     a woken waiter returns without passing the wake on. -> LOST WAKE
  wake_one    the chain wakes 1 instead of 2: correct, and O(L) deep - kept to
              show the model separates depth from correctness, not failing.

The releaser here is the progress thread of a multi-node job (§5.3): it waits
for every member's arrival and releases. On one node the last arriver releases;
the wake after it is the same chain.

Usage: chained_wake_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

ARRIVED = ('arrived',)
RELEASED = ('released',)
ALL = 0b11


def mask_for(design, target):
    return ALL if design == 'unmasked' else 1 << (target & 1)


def member(design, members, epochs):
    width = 1 if design == 'wake_one' else 2
    B = []
    for k in range(epochs):
        e, p = k + 1, f'e{k}_'
        nxt = f'@e{k + 1}_A' if k + 1 < epochs else '@MEND'
        B += [(p + 'A', ('add', lambda L: ARRIVED, lambda L: 1, '_', '@' + p + 'L')),
              # §5.4's equality wait, with the chain added.
              (p + 'L', ('load', lambda L: RELEASED, 'v', '@' + p + 'T')),
              (p + 'T', ('br', lambda L, e=e: L['v'] == e, '@' + p + 'R', '@' + p + 'S')),
              (p + 'S', ('fwaitm', lambda L: RELEASED, lambda L: L['v'],
                         lambda L, e=e: mask_for(design, e), '@' + p + 'K')),
              (p + 'K', ('set', 'woken', lambda L: 1, '@' + p + 'L')),
              # Reached. A waiter whose wait returned passes the wake on; one
              # that found the target on its first load never slept and owes
              # nothing.
              (p + 'R', ('br', lambda L: L['woken'] == 1 and design != 'no_pass',
                         '@' + p + 'P', '@' + p + 'X')),
              (p + 'P', ('fwaken', lambda L: RELEASED, lambda L: width,
                         lambda L, e=e: mask_for(design, e), '@' + p + 'X')),
              (p + 'X', ('set', 'woken', lambda L: DEL, '@' + p + 'Y')),
              (p + 'Y', ('set', 'v', lambda L: DEL, nxt))]
    B += [('MEND', ('done',))]
    return assemble(B)


def releaser(design, members, epochs):
    """The progress thread: release each epoch once every member has arrived."""
    width = 1 if design == 'wake_one' else 2
    B = []
    for k in range(epochs):
        e, p = k + 1, f'r{k}_'
        nxt = f'@r{k + 1}_W' if k + 1 < epochs else '@REND'
        B += [(p + 'W', ('await', lambda M, L, e=e, m=members: M[ARRIVED] == e * m, '@' + p + 'B')),
              (p + 'B', ('add', lambda L: RELEASED, lambda L: 1, '_', '@' + p + 'K')),
              # O(1) on this thread: two wakes, whatever L is.
              (p + 'K', ('fwaken', lambda L: RELEASED, lambda L: width,
                         lambda L, e=e: mask_for(design, e), nxt))]
    B += [('REND', ('done',))]
    return assemble(B)


def case(design, members, epochs):
    one = member(design, members, epochs)
    threads = [one] * members + [releaser(design, members, epochs)]
    mem = {ARRIVED: 0, RELEASED: 0}
    return (f'chained_wake/{design} members={members} epochs={epochs}', threads, mem,
            [list(range(members))])


# (design, members, epochs, expect_ok). Four and five waiters, two epochs: the
# second epoch is what puts a next-epoch waiter on the word.
CASES = [
    ('design', 4, 2, True),
    ('design', 5, 2, True),
    ('design', 4, 3, True),
    ('wake_one', 4, 2, True),
    ('unmasked', 4, 1, True),        # one epoch cannot show it: no next-epoch waiter
    ('unmasked', 4, 2, False),
    ('unmasked', 5, 2, False),
    ('no_pass', 4, 1, False),
    ('no_pass', 4, 2, False),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, members, epochs, expect in CASES:
        name, threads, mem0, sym = case(design, members, epochs)
        if not keep(name):
            continue
        res = explore(name, threads, mem0, symmetric=sym, max_seconds=900,
                      liveness_max_states=4_000_000)
        ok &= report(res, expect_ok=expect)
    sys.exit(0 if ok else 1)
