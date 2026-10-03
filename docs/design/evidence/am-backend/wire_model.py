#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The node-pair wire: a byte stream and its wake protocol (design §3.1, §3.10).

§7.1 step 2 runs the AM layer's queues in memory rather than on sockets, so that
every internode semantic is validated before a socket exists. What replaces the
socket is a single-producer single-consumer byte ring in memory shared by the two
node leads, with a futex word so the reader sleeps instead of spinning. Step 3
replaces it with io_uring over TCP and nothing above it changes.

Two properties have to hold, and neither is obvious enough to take on faith:

  STREAM     the reader receives exactly the bytes the writer sent, in order,
             once each - the property §3.1 leans on when it says stream order
             plus in-order application gives per-pair ordering for free.
  OVERWRITE  the writer never writes over a byte the reader has not taken. The
             ring is small on purpose (§8.2: loopback hides short writes, so
             this one is sized to produce them), which is exactly the condition
             under which an off-by-one here is reachable.

and a lost wake is a hung job: the reader sleeps while bytes are available and
nothing will wake it again.

The wake protocol is §4.6's, the one used for every 64-bit counter in this
design: the reader loads the wake word **before** the data it is waiting on, and
the writer publishes the data **before** bumping the wake word. The kernel closes
the remaining race, as §3.10 says of the io_uring form: futex_wait compares the
word at submission, so a bump between the reader's load and its wait makes the
wait return at once rather than sleep on a stale value.

Designs
  design     as above: reader loads wake, then produced; writer publishes
             produced, then bumps wake and wakes.
Breakages
  wake_after the reader loads produced *first* and the wake word after. The
             writer can then publish and bump in the window between the two, and
             the reader sleeps on a wake value that already accounts for the
             bytes it has not seen. Nothing wakes it.
  nowake     the writer never bumps the wake word. The first reader sleep is
             permanent.
  nospace    the writer does not check that the ring has room, which is the
             off-by-one the small ring exists to catch.
  consumed_early
             the reader publishes `consumed` before copying the byte out of the
             slot, so the writer may refill it mid-copy. This is the defect a
             single-threaded test cannot see at all and a two-process test sees
             only sometimes - about half of runs, measured - which is the reason
             it is settled here instead: the model reaches it in every
             interleaving that contains it.

Every protocol store here is a locked instruction, a seq_cst store or a futex
call, so x86-TSO adds no interleavings; runs are SC. The wire is
single-producer single-consumer by construction - only the two node leads touch
it (§3.12: non-lead agents never touch a ring or a socket) - so one writer
thread and one reader thread is the whole configuration, not a reduction of it.

Usage: wire_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

PROD, CONS, WAKE = ('produced',), ('consumed',), ('wake',)
SLOT = lambda i: ('slot', i)            # the ring itself, CAP bytes
SENT = lambda i: ('$sent', i)           # value of the i'th byte written
GOT = lambda i: ('$got', i)             # value of the i'th byte delivered


def writer(design, nbytes, cap):
    """Send nbytes bytes, values 1..nbytes so that 0 is 'nothing here'."""
    B = []
    for k in range(nbytes):
        p = f'w{k}_'
        nxt = f'@w{k + 1}_L' if k + 1 < nbytes else '@WEND'
        # Load the reader's progress, then this direction's own produced count.
        B += [(p + 'L', ('load', lambda L: CONS, 'c', '@' + p + 'P')),
              (p + 'P', ('load', lambda L: PROD, 'p', '@' + p + 'R'))]
        if design == 'nospace':
            # No room check at all: write wherever produced points.
            B += [(p + 'R', ('br', lambda L: True, '@' + p + 'W', '@' + p + 'W'))]
        else:
            # Blocked while the ring is full; the real writer reports a short
            # write to its caller and comes back, which is the same thing here.
            B += [(p + 'R', ('await', lambda M, L, cap=cap: M[PROD] - M[CONS] < cap,
                             '@' + p + 'L2')),
                  (p + 'L2', ('load', lambda L: CONS, 'c', '@' + p + 'P2')),
                  (p + 'P2', ('load', lambda L: PROD, 'p', '@' + p + 'W'))]
        # The payload byte, then publish it, then the wake.
        B += [(p + 'W', ('store', lambda L, cap=cap: SLOT(L['p'] % cap),
                         lambda L, k=k: k + 1, False, '@' + p + 'C'))]
        # OVERWRITE is checked at the moment of the store, against what the
        # reader has actually taken: a byte written into a slot still holding an
        # undelivered byte is the defect, whether or not anyone later notices.
        # The store has happened but `produced` has not moved yet, so the
        # bytes outstanding at this instant must be strictly below the ring's
        # capacity: at capacity, the slot just written still held a byte the
        # reader had not taken.
        B += [(p + 'C', ('check', lambda M, L, cap=cap:
                         None if M[PROD] - M[CONS] < cap else
                         f'wrote byte {L["p"]} over a full ring: {M[PROD] - M[CONS]} outstanding, capacity {cap}',
                         '@' + p + 'G'))]
        B += [(p + 'G', ('gpre', lambda M, L, k=k: ({SENT(k): k + 1}, {}), '@' + p + 'S')),
              (p + 'S', ('store', lambda L: PROD, lambda L: L['p'] + 1, True, '@' + p + 'K'))]
        if design == 'nowake':
            B += [(p + 'K', ('set', 'p', lambda L: DEL, nxt))]
        else:
            B += [(p + 'K', ('add', lambda L: WAKE, lambda L: 1, '_', '@' + p + 'F')),
                  (p + 'F', ('fwake', lambda L: WAKE, nxt))]
    B += [('WEND', ('done',))]
    return assemble(B)


def reader(design, nbytes, cap):
    """Take every byte, checking each against what the writer put there."""
    B = [('R', ('load', lambda L: CONS, 'c', '@RDONE?'))]
    B += [('RDONE?', ('br', lambda L, n=nbytes: L['c'] >= n, '@REND', '@ORDER'))]
    if design == 'wake_after':
        # The data first, the wake word second: the broken order.
        B += [('ORDER', ('load', lambda L: PROD, 'p', '@WAKE')),
              ('WAKE', ('load', lambda L: WAKE, 'w', '@HAVE?'))]
    else:
        # §4.6: the wake word BEFORE the counters it guards.
        B += [('ORDER', ('load', lambda L: WAKE, 'w', '@PROD')),
              ('PROD', ('load', lambda L: PROD, 'p', '@HAVE?'))]
    B += [('HAVE?', ('br', lambda L: L['p'] > L['c'], '@TAKE', '@SLEEP')),
          # Nothing yet: sleep on the value read above. The kernel compares, so a
          # bump since that load returns at once instead of sleeping (§3.10).
          ('SLEEP', ('fwait', lambda L: WAKE, lambda L: L['w'], '@R')),
          ('TAKE', ('load', lambda L, cap=cap: SLOT(L['c'] % cap), 'v', '@CHECK')),
          ('CHECK', ('check', lambda M, L:
                     None if L['v'] == M[SENT(L['c'])] else
                     f'byte {L["c"]} read as {L["v"]}, written as {M[SENT(L["c"])]}',
                     '@GOT')),
          ('GOT', ('gpre', lambda M, L: ({GOT(L['c']): L['v']}, {}), '@ADV')),
          ('ADV', ('store', lambda L: CONS, lambda L: L['c'] + 1, True, '@R')),
          ('REND', ('done',))]
    if design == 'consumed_early':
        # Release the slot before the byte is out of it: the reader publishes
        # `consumed` and only then copies. The writer is free to fill that slot
        # the moment it sees the space, so the copy takes the next byte.
        B = [(lab, ins) for lab, ins in B if lab not in ('TAKE', 'ADV')]
        B += [('ADV', ('store', lambda L: CONS, lambda L: L['c'] + 1, True, '@TAKE')),
              ('TAKE', ('load', lambda L, cap=cap: SLOT(L['c'] % cap), 'v', '@CHECK'))]
        B = [('HAVE?', ('br', lambda L: L['p'] > L['c'], '@ADV', '@SLEEP'))
             if lab == 'HAVE?' else (lab, ins) for lab, ins in B]
        B = [('GOT', ('gpre', lambda M, L: ({GOT(L['c']): L['v']}, {}), '@R'))
             if lab == 'GOT' else (lab, ins) for lab, ins in B]
    return assemble(B)


def case(design, nbytes, cap):
    return (f'wire/{design} bytes={nbytes} cap={cap}',
            [writer(design, nbytes, cap), reader(design, nbytes, cap)],
            {PROD: 0, CONS: 0, WAKE: 0})


CASES = [
    # The design, at ring sizes that force wrap-around and a full ring.
    ('design', 2, 1), ('design', 3, 1), ('design', 3, 2), ('design', 4, 2),
    # Each breakage, at the smallest size that can show it.
    ('wake_after', 2, 2), ('wake_after', 3, 2),
    ('nowake', 2, 2),
    ('nospace', 2, 1), ('nospace', 3, 2),
    ('consumed_early', 2, 1), ('consumed_early', 3, 2),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, nbytes, cap in CASES:
        name, threads, mem0 = case(design, nbytes, cap)
        if not keep(name):
            continue
        res = explore(name, threads, mem0, max_seconds=120)
        ok &= report(res, expect_ok=(design == 'design'))
    sys.exit(0 if ok else 1)
