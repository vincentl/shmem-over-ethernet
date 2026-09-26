#!/usr/bin/env python3
"""One copier per landed nonblocking get (design §4.6 as built, §4.11).

A nonblocking get's reply lands in the landing arena under a record, and is
copied to the caller's destination later: by the PE's agent, or by any thread
that quiets meanwhile. Under SHMEM_THREAD_MULTIPLE several threads can quiet at
once, so the author's decision of 2026-09-25 gives each record one copier,
claimed by a compare-and-swap on the record's state: LANDED to COPYING. The
winner copies, credits the landed count that quiets wait on, and frees the
record, which the issuer may then reuse for its next get.

Checked
  ONCE      each get's destination is written exactly once.
  OWN       a copy reads the reply of the get it claimed, not a later get's
            that reused the record.
  COMPLETE  a quiet returns only when every get issued before it has been
            copied to its destination.
  and, by the engine: no stuck thread, and from every reachable state the run
  can still finish (AG EF done).

Threads
  issuer   G nonblocking gets, each on a free record (waiting for one), then a
           quiet of its own.
  quieter  a second application thread's quiet on the same context, called once
           the gets are issued.
  agent    the PE's agent, copying out any landed reply.
  lander   the progress thread: writes a reply into its record's arena, then
           marks the record landed.

A quiet here waits for the landed count to reach the issued count, which is
§4.6's equality with nobody issuing during the wait. The generation word that
makes it sound while threads keep issuing is get_completion_model.py's
subject, not this model's; what this one adds is who copies, and when a record
may be reused.

Designs
  design            as above.
Breakages
  no_claim          a copier tests for LANDED and marks COPYING with a plain
                    store: two copiers write one destination (ONCE).
  credit_first      the landed count is credited before the copy: a quiet
                    returns with a destination still unwritten (COMPLETE).
  free_first        the record is freed as soon as it is claimed: the issuer
                    reuses it, and the copy reads the next get's reply (OWN).

Every protocol step is a locked instruction or a CAS, and the arena write is
ordered before the landed mark by the lander's own store order, so x86-TSO adds
no interleavings; runs are SC.

Usage: copy_out_claim_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

FREE, ISSUED, LANDED, COPYING = 0, 1, 2, 3
STATE = lambda r: ('state', r)
TAG = lambda r: ('tag', r)        # the get using record r
ARENA = lambda r: ('arena', r)    # the reply landed under record r
ISSUED_N = ('issued',)            # gets issued
CREDIT = ('landed',)              # gets copied out and credited: what quiets wait on
COPIES = lambda t: ('$copies', t) # times get t's destination was written
DONE = ('$done',)                 # threads finished


def copier(design, p, records, again, then):
    """Claim a landed record and copy it out; `again` when there is none."""
    def pick(M, L):
        out = [({}, {'r': r}, f'pick r{r}') for r in range(records)
               if M[STATE(r)] == LANDED]
        return out or [({}, {'r': None}, 'none landed')]

    B = [(p + 'P', ('nd', pick, '@' + p + 'P1')),
         (p + 'P1', ('br', lambda L: L['r'] is None, again, '@' + p + 'T')),
         (p + 'T', ('load', lambda L: TAG(L['r']), 't', '@' + p + 'C'))]
    if design == 'no_claim':
        B += [(p + 'C', ('load', lambda L: STATE(L['r']), 's', '@' + p + 'C1')),
              (p + 'C1', ('br', lambda L: L['s'] == LANDED, '@' + p + 'C2', again)),
              (p + 'C2', ('store', lambda L: STATE(L['r']), lambda L: COPYING, True,
                          '@' + p + 'G'))]
    else:
        B += [(p + 'C', ('cas', lambda L: STATE(L['r']), lambda L: LANDED,
                         lambda L: COPYING, 'ok', '@' + p + 'C1')),
              (p + 'C1', ('br', lambda L: L['ok'], '@' + p + 'G', again))]

    copy = [(p + 'Y', ('check', lambda M, L:
                       None if M[ARENA(L['r'])] == L['t'] else
                       f'copied reply {M[ARENA(L["r"])]} for get {L["t"]}: the record was reused',
                       '@' + p + 'Y1')),
            (p + 'Y1', ('ghost', lambda M, L: ({COPIES(L['t']): M[COPIES(L['t'])] + 1}, {}),
                        '@' + p + 'Y2')),
            (p + 'Y2', ('check', lambda M, L:
                        None if M[COPIES(L['t'])] == 1 else
                        f'get {L["t"]} copied {M[COPIES(L["t"])]} times', '@' + p + 'Y3'))]
    if design == 'credit_first':
        # claim, credit, copy, free
        B += [(p + 'G', ('br', lambda L: True, '@' + p + 'K', '@' + p + 'K'))]
        B += [(p + 'K', ('add', lambda L: CREDIT, lambda L: 1, '_', '@' + p + 'Y'))]
        B += copy[:2] + [(p + 'Y2', copy[2][1][:2] + ('@' + p + 'F',))]
        B += [(p + 'F', ('store', lambda L: STATE(L['r']), lambda L: FREE, True, '@' + p + 'E'))]
    elif design == 'free_first':
        # claim, free, copy, credit
        B += [(p + 'G', ('store', lambda L: STATE(L['r']), lambda L: FREE, True, '@' + p + 'Y'))]
        B += copy[:2] + [(p + 'Y2', copy[2][1][:2] + ('@' + p + 'K',))]
        B += [(p + 'K', ('add', lambda L: CREDIT, lambda L: 1, '_', '@' + p + 'E'))]
    else:
        # claim, copy, credit, free
        B += [(p + 'G', ('br', lambda L: True, '@' + p + 'Y', '@' + p + 'Y'))]
        B += copy[:2] + [(p + 'Y2', copy[2][1][:2] + ('@' + p + 'K',))]
        B += [(p + 'K', ('add', lambda L: CREDIT, lambda L: 1, '_', '@' + p + 'F'))]
        B += [(p + 'F', ('store', lambda L: STATE(L['r']), lambda L: FREE, True, '@' + p + 'E'))]
    B += [(p + 'E', ('set', 'r', lambda L: DEL, then))]
    return B


def quiet(design, p, gets, records, then):
    """Wait for every issued get to be copied, copying landed ones meanwhile."""
    B = [(p + 'Q', ('load', lambda L: CREDIT, 'c', '@' + p + 'Q1')),
         (p + 'Q1', ('br', lambda L: L['c'] >= gets, '@' + p + 'RET', '@' + p + 'Q2')),
         # Something to do: a landed reply to copy, or another copier's credit.
         (p + 'Q2', ('await', lambda M, L: M[CREDIT] != L['c'] or
                     any(M[STATE(r)] == LANDED for r in range(records)), '@' + p + 'P'))]
    B += copier(design, p, records, '@' + p + 'Q', '@' + p + 'Q')
    B += [(p + 'RET', ('check', lambda M, L, g=gets:
                       next((f'quiet returned with get {t} not yet copied'
                             for t in range(1, g + 1) if M[COPIES(t)] != 1), None),
                       then))]
    return B


def issuer(design, gets, records):
    B = []
    for k in range(gets):
        tag = k + 1
        p = f'i{k}_'
        nxt = f'@i{k + 1}_W' if k + 1 < gets else '@QQ'

        def free_record(M, L):
            return [({}, {'r': r}, f'take r{r}') for r in range(records)
                    if M[STATE(r)] == FREE]
        B += [(p + 'W', ('nd', free_record, '@' + p + 'S')),
              (p + 'S', ('store', lambda L: TAG(L['r']), lambda L, tag=tag: tag, True,
                         '@' + p + 'S1')),
              (p + 'S1', ('store', lambda L: STATE(L['r']), lambda L: ISSUED, True,
                          '@' + p + 'S2')),
              (p + 'S2', ('add', lambda L: ISSUED_N, lambda L: 1, '_', '@' + p + 'S3')),
              (p + 'S3', ('set', 'r', lambda L: DEL, nxt))]
    B += [('QQ', ('br', lambda L: True, '@q_Q', '@q_Q'))]
    B += quiet(design, 'q_', gets, records, '@END')
    B += [('END', ('gpost', lambda M, L: ({DONE: M[DONE] + 1}, {}), '@FIN')),
          ('FIN', ('done',))]
    return assemble(B)


def quieter(design, gets, records):
    B = [('S', ('await', lambda M, L, g=gets: M[ISSUED_N] == g, '@q_Q'))]
    B += quiet(design, 'q_', gets, records, '@END')
    B += [('END', ('gpost', lambda M, L: ({DONE: M[DONE] + 1}, {}), '@FIN')),
          ('FIN', ('done',))]
    return assemble(B)


def agent(design, gets, records):
    B = [('A', ('load', lambda L: CREDIT, 'c', '@A1')),
         ('A1', ('br', lambda L: L['c'] >= gets, '@END', '@A2')),
         ('A2', ('await', lambda M, L: M[CREDIT] != L['c'] or
                 any(M[STATE(r)] == LANDED for r in range(records)), '@a_P'))]
    B += copier(design, 'a_', records, '@A', '@A')
    B += [('END', ('gpost', lambda M, L: ({DONE: M[DONE] + 1}, {}), '@FIN')),
          ('FIN', ('done',))]
    return assemble(B)


def lander(gets, records):
    def issued(M, L):
        return [({}, {'r': r}, f'land r{r}') for r in range(records)
                if M[STATE(r)] == ISSUED]
    return assemble([
        ('L', ('br', lambda L, g=gets: (L['n'] or 0) >= g, '@END', '@P')),
        ('P', ('nd', issued, '@W')),
        ('W', ('load', lambda L: TAG(L['r']), 't', '@W1')),
        ('W1', ('store', lambda L: ARENA(L['r']), lambda L: L['t'], True, '@W2')),
        ('W2', ('store', lambda L: STATE(L['r']), lambda L: LANDED, True, '@W3')),
        ('W3', ('set', 'n', lambda L: (L['n'] or 0) + 1, '@W4')),
        ('W4', ('set', 'r', lambda L: DEL, '@L')),
        ('END', ('gpost', lambda M, L: ({DONE: M[DONE] + 1}, {}), '@FIN')),
        ('FIN', ('done',))])


def case(design, gets, records, quieters):
    threads = [issuer(design, gets, records), agent(design, gets, records),
               lander(gets, records)]
    threads += [quieter(design, gets, records) for _ in range(quieters)]
    mem = {ISSUED_N: 0, CREDIT: 0, DONE: 0}
    for r in range(records):
        mem[STATE(r)] = FREE
        mem[TAG(r)] = mem[ARENA(r)] = 0
    for t in range(1, gets + 1):
        mem[COPIES(t)] = 0
    name = f'copy_out_claim/{design} gets={gets} records={records} quieters={quieters}'
    return name, threads, mem, 3 + quieters


# (design, gets, records, quieters, expect_ok)
CASES = [
    ('design', 2, 1, 1, True),
    ('design', 2, 2, 1, True),
    ('design', 3, 2, 1, True),
    ('no_claim', 2, 2, 1, False),
    ('credit_first', 2, 2, 1, False),
    ('free_first', 2, 1, 1, False),
]


def main():
    want = select(sys.argv)
    ok = True
    for design, gets, records, quieters, expect in CASES:
        name, threads, mem, nthreads = case(design, gets, records, quieters)
        if not want(name):
            continue
        res = explore(name, threads, mem, max_seconds=600, liveness_max_states=3_000_000,
                      done=lambda M, n=nthreads: M[DONE] == n)
        ok &= report(res, expect_ok=expect)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
