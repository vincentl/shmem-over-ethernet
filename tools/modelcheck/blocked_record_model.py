#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The "blocked in" record: a multi-word record read without the job's help (§3.14).

§3.14 requires that everything an investigator needs is readable from the named
control segment by an external tool, with no cooperation from the job - because
the case that matters is a hung job, where the process that would cooperate is
the one that is stuck. The "blocked in" record is the first such record with
more than one word in it: a PE about to sleep publishes the routine it is inside,
a routine-specific detail, and when it started waiting. osoe-stat reads all three
from another process, while the PE may be publishing its next wait.

  gen        a generation counter: odd while the fields are being written
  routine    which API routine the PE is inside
  detail     routine-specific: a target PE, an epoch, an offset

Writer (the waiting PE, and the only writer):
    fetch_add(gen, 1)        -> odd: the fields are in flux
    store routine, detail    release stores of atomic words; single-writer
    fetch_add(gen, 1)        -> even: the fields are stable

Reader (osoe-stat or the deadlock detector, another process; never writes):
    load gen; if odd, retry
    load routine, detail     acquire loads
    load gen again; if it changed, retry; else accept

Two seq_cst read-modify-writes rather than a textbook seqlock. The usual form
uses relaxed stores with a release fence between them, and §3.16 permits
neither relaxed orders nor fences on protocol words; `fetch_add` naming seq_cst
is a rule the project already has. The cost is two locked instructions per
*sleeping* wait - the record is published only after the first predicate check
fails, so a wait that returns immediately pays nothing (§8.6 U1, U3).

What this model cannot see. The engine has two memory models, sequential
consistency and x86-TSO, and both keep a thread's loads in order; aarch64, the
project's target, does not. Until the round-three review (K1) the routine was a
plain character array, and a reader's plain loads of it could be satisfied
after its second load of the generation, pairing one wait's name with another's
detail - 28,000 to 218,000 times in 60 to 90 million reads - while this model
passed. The fields are now atomic words, stored with release and loaded with
acquire, whose order C11 states for every target (shmem/src/blocked.c gives the
argument), and shmem/tests/test-blocked-record.c checks it on the machine.

Checked: the reader never accepts a torn record. Publication k writes k to both
fields, so any accepted pair whose halves disagree came from two different
publications and the protocol has failed.

Variants, each of which must fail:
  nogen      no generation at all - the reader just loads the two fields
  bumpafter  the writer stores the fields first and bumps afterwards
  hoist      one field store is moved above the first bump. This is what a
             first bump weaker than seq_cst permits: the engine models SC and
             x86-TSO, where store-store order is kept and a release store would
             pass, so the reordering is exhibited directly rather than claimed
             of a memory model this engine does not have
  noverify   the reader omits its second load of the generation

Usage: blocked_record_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

GEN = ('gen',)
ROUTINE = ('routine',)
DETAIL = ('detail',)

PUBLICATIONS = 2        # the PE sleeps, wakes, and sleeps again
ATTEMPTS = 3            # bounded, so the reader gives up rather than spinning


def writer(variant=''):
    """Publish PUBLICATIONS successive records; record k writes k to both fields."""
    B = []
    for k in range(1, PUBLICATIONS + 1):
        p = f'W{k}'
        nxt = f'@W{k + 1}a' if k < PUBLICATIONS else '@DONE'

        if variant == 'bumpafter':
            # Fields first, then both bumps: the window never covers the writes.
            B += [(p + 'a', ('store', lambda L: ROUTINE, lambda L, k=k: k, False, '@' + p + 'b')),
                  (p + 'b', ('store', lambda L: DETAIL, lambda L, k=k: k, False, '@' + p + 'c')),
                  (p + 'c', ('add', lambda L: GEN, lambda L: 1, '_', '@' + p + 'd')),
                  (p + 'd', ('add', lambda L: GEN, lambda L: 1, '_', nxt))]
        elif variant == 'hoist':
            # One field store above the opening bump, which is exactly what a
            # first bump that is not a full barrier allows.
            B += [(p + 'a', ('store', lambda L: ROUTINE, lambda L, k=k: k, False, '@' + p + 'b')),
                  (p + 'b', ('add', lambda L: GEN, lambda L: 1, '_', '@' + p + 'c')),
                  (p + 'c', ('store', lambda L: DETAIL, lambda L, k=k: k, False, '@' + p + 'd')),
                  (p + 'd', ('add', lambda L: GEN, lambda L: 1, '_', nxt))]
        else:
            B += [(p + 'a', ('add', lambda L: GEN, lambda L: 1, '_', '@' + p + 'b')),
                  (p + 'b', ('store', lambda L: ROUTINE, lambda L, k=k: k, False, '@' + p + 'c')),
                  (p + 'c', ('store', lambda L: DETAIL, lambda L, k=k: k, False, '@' + p + 'd')),
                  (p + 'd', ('add', lambda L: GEN, lambda L: 1, '_', nxt))]
    B.append(('DONE', ('done',)))
    return assemble(B)


def torn(M, L):
    """Publication k wrote k to both fields, so halves that disagree are torn."""
    if L['r'] != L['d']:
        return f"torn read: routine={L['r']} from one publication, detail={L['d']} from another"
    return None


def reader(variant=''):
    """Read the record, retrying at most ATTEMPTS times, then give up."""
    B = []
    for a in range(ATTEMPTS):
        p = f'R{a}'
        retry = f'@R{a + 1}0' if a + 1 < ATTEMPTS else '@GIVEUP'

        if variant == 'nogen':
            B += [(p + '0', ('load', lambda L: ROUTINE, 'r', '@' + p + '1')),
                  (p + '1', ('load', lambda L: DETAIL, 'd', '@' + p + 'ok'))]
        else:
            B += [(p + '0', ('load', lambda L: GEN, 'g1', '@' + p + '1')),
                  # An odd generation means the fields are being written now.
                  (p + '1', ('br', lambda L: L['g1'] % 2 == 0, '@' + p + '2', retry)),
                  (p + '2', ('load', lambda L: ROUTINE, 'r', '@' + p + '3')),
                  (p + '3', ('load', lambda L: DETAIL, 'd', '@' + p + '4'))]
            if variant == 'noverify':
                B += [(p + '4', ('br', lambda L: True, '@' + p + 'ok', '@' + p + 'ok'))]
            else:
                B += [(p + '4', ('load', lambda L: GEN, 'g2', '@' + p + '5')),
                      # Unchanged means no publication overlapped the read.
                      (p + '5', ('br', lambda L: L['g2'] == L['g1'], '@' + p + 'ok', retry))]

        B += [(p + 'ok', ('check', torn, '@ACCEPTED'))]
    B += [('ACCEPTED', ('gpost', lambda M, L: ({}, {'r': DEL, 'd': DEL, 'g1': DEL, 'g2': DEL}),
                        '@DONE')),
          ('GIVEUP', ('done',)),
          ('DONE', ('done',))]
    return assemble(B)


def run(name, tso=False, variant='', expect_ok=None):
    threads = [writer(variant), reader(variant)]
    res = explore(name, threads, {GEN: 0, ROUTINE: 0, DETAIL: 0}, tso=tso)
    return report(res, expect_ok=expect_ok, tso=tso)


CASES = [
    # name, tso, variant, expected to pass
    ('design: two seq_cst bumps', False, '', True),
    ('design: two seq_cst bumps', True, '', True),
    ('nogen:     no generation word', False, 'nogen', False),
    ('nogen:     no generation word', True, 'nogen', False),
    ('bumpafter: fields written before the window', False, 'bumpafter', False),
    ('hoist:     a field store above the opening bump', True, 'hoist', False),
    ('noverify:  reader does not re-read the generation', False, 'noverify', False),
]

if __name__ == '__main__':
    want = select(sys.argv)
    good = True
    print('== can an external reader see a torn "blocked in" record?', flush=True)
    for name, tso, variant, expect in CASES:
        if want(name):
            good &= run(name, tso, variant, expect)
    print()
    print('ALL AS EXPECTED' if good else 'SOME RESULT UNEXPECTED OR INCOMPLETE')
    sys.exit(0 if good else 1)
