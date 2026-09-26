#!/usr/bin/env python3
"""The dirty set of a shareable context under SHMEM_THREAD_MULTIPLE (design §4.11).

One node pair, one context. Several threads issue puts to the node on the
context while several threads call shmem_ctx_quiet on it.

Checked at every quiet return: every put whose issuing call returned before the
quiet was called has been applied at the target. Also checked: no stuck thread,
such as a quiet waiting on an acknowledgement that can never come.

Threads
  issuers     reserve in the connection's one reservable buffer and read its
              sequence number (§3.5), copy, raise the context's high_seg for the
              node to at least that number, and set the node's dirty bit.
  connection  the lead's progress thread and the peer, abstracted. Seals the
              reservable buffer when it holds anything (the next buffer opens
              with the next number), and applies and acknowledges sealed buffers
              in order once every reservation in them is copied (§3.4, §3.5).
  quiets      take the context's quiet mutex (the one get completion uses). If
              the dirty bit is set: load high_seg, wait for acked >= it, clear the
              bit, re-load high_seg and acked, and if the node is still behind,
              set the bit again and repeat. Release the mutex.

The flush request and the wake word are abstracted: a quiet's wait is an 'await'
on acked, and the connection may seal at any time. Both are §4.6 patterns the
review's models cover. What is new here is the bit and high_seg words with many
writers and several clearers.

Breakages
  unlocked  quiets do not take the mutex. Another quiet can load the bit in the
            window between a clear and the re-set that follows it.
  naive     §4.6 as written: clear after the wait, no re-check (ndirty = 0).
  bitfirst  the issuer sets the bit before raising high_seg.
  plainmax  high_seg raised by load, compare, store instead of a CAS loop.

Variants of the design
  resetstop the quiet, finding the node behind after its clear, sets the bit
            again and returns rather than waiting again (the review of phase 1
            step 8 slice 3, S1: waiting again lets a thread that keeps putting
            to the node hold the quiet, and the mutex, forever). Every put that
            returned before the call raised high_seg before the quiet loaded
            it, so the one wait covers them; the re-set leaves later puts to
            the next quiet.

Reductions (engine.py): a quiet's call instant is folded into its first step
(gpre); an issuer's copy into its reservation and its return into its last step
(gpost). Folding the copy only makes buffers sendable sooner, which keeps every
failing check failing. Quiets are symmetric.

Every protocol store is a locked instruction or a CAS, so runs are SC.

Usage: dirty_set_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

OPEN, SEALED, ACKED = ('open',), ('sealed',), ('acked',)
HS, BIT, QLOCK = ('hs',), ('bit',), ('qlock',)
BUF = lambda k: ('$buf', k)          # sequence number of the buffer put k reserved in
COPIED = lambda k: ('$copied', k)
APPLIED = lambda k: ('$applied', k)
RET = lambda k: ('$ret', k)


def issuer(ops, variant=''):
    B = []
    for n, k in enumerate(ops):
        p = f'k{k}_'
        nxt = f'@k{ops[n + 1]}_R0' if n + 1 < len(ops) else '@END'
        hs_first = variant != 'bitfirst'
        hs = [(p + 'H0', ('load', lambda L: HS, 'h', '@' + p + 'H1')),
              (p + 'H1', ('br', lambda L: L['s'] <= L['h'], '@' + p + 'HX', '@' + p + 'H2'))]
        if variant == 'plainmax':
            hs += [(p + 'H2', ('store', lambda L: HS, lambda L: L['s'], True, '@' + p + 'HX'))]
        else:
            hs += [(p + 'H2', ('cas', lambda L: HS, lambda L: L['h'], lambda L: L['s'], 'ok', '@' + p + 'H3')),
                   (p + 'H3', ('br', lambda L: L['ok'], '@' + p + 'HX', '@' + p + 'H0'))]
        hs += [(p + 'HX', ('set', 'h', lambda L: DEL, '@' + p + 'HY')),
               (p + 'HY', ('set', 'ok', lambda L: DEL, '@' + p + ('B0' if hs_first else 'RET')))]
        bit = [(p + 'B0', ('load', lambda L: BIT, 'b', '@' + p + 'B1')),
               (p + 'B1', ('br', lambda L: L['b'] & 1, '@' + p + 'BX', '@' + p + 'B2')),
               (p + 'B2', ('or', lambda L: BIT, lambda L: 1, '_', '@' + p + 'BX')),
               (p + 'BX', ('set', 'b', lambda L: DEL, '@' + p + ('RET' if hs_first else 'H0')))]
        B += [(p + 'R0', ('nd', lambda M, L, k=k: [({BUF(k): M[OPEN], COPIED(k): 1}, {'s': M[OPEN]},
                                                     f'reserve and copy put {k} in seg {M[OPEN]}')],
                          '@' + p + ('H0' if hs_first else 'B0')))]
        B += hs + bit
        B += [(p + 'RET', ('gpost', lambda M, L, k=k: ({RET(k): 1}, {'s': DEL}), nxt))]
    B.append(('END', ('done',)))
    return assemble(B)


def connection(allops):
    def step(M, L):
        alts = []
        if any(M.get(BUF(k), -1) == M[OPEN] for k in allops):
            alts.append(({SEALED: M[OPEN], OPEN: M[OPEN] + 1}, {}, f'seal seg {M[OPEN]}'))
        nxt = M[ACKED] + 1
        if nxt <= M[SEALED]:
            inseg = [k for k in allops if M.get(BUF(k), -1) == nxt]
            if all(M[COPIED(k)] for k in inseg):
                upd = {APPLIED(k): 1 for k in inseg}
                upd[ACKED] = nxt
                alts.append((upd, {}, f'apply and ack seg {nxt}'))
        return alts
    return assemble([('C0', ('nd', step, '@C0'))])


def quiet(allops, variant=''):
    def call(M, L):
        return {}, {'pre': tuple(k for k in allops if M[RET(k)])}

    def complete(M, L):
        bad = [k for k in L['pre'] if not M[APPLIED(k)]]
        return f'quiet returned with prior put(s) {bad} not applied' if bad else None

    lock = variant != 'unlocked'
    B = [('Q0', ('gpre', call, '@K0' if lock else '@Q1')),
         ('K0', ('cas', lambda L: QLOCK, lambda L: 0, lambda L: 1, 'ok', '@K1')),
         ('K1', ('br', lambda L: L['ok'], '@K3', '@K2')),
         ('K2', ('fwait', lambda L: QLOCK, lambda L: 1, '@K0')),
         ('K3', ('set', 'ok', lambda L: DEL, '@Q1')),
         ('Q1', ('load', lambda L: BIT, 'b', '@Q1b')),
         ('Q1b', ('br', lambda L: L['b'] & 1, '@Q2', '@UNL')),
         ('Q2', ('load', lambda L: HS, 'h', '@Q3')),
         ('Q3', ('await', lambda M, L: M[ACKED] >= L['h'], '@Q4')),
         ('Q4', ('andn', lambda L: BIT, lambda L: 1, '_', '@UNL' if variant == 'naive' else '@Q5')),
         ('Q5', ('load', lambda L: HS, 'h', '@Q6')),
         ('Q6', ('load', lambda L: ACKED, 'a', '@Q7')),
         ('Q7', ('br', lambda L: L['a'] >= L['h'], '@UNL', '@Q8')),
         ('Q8', ('or', lambda L: BIT, lambda L: 1, '_', '@Q9')),
         ('Q9', ('set', 'a', lambda L: DEL, '@UNL' if variant == 'resetstop' else '@Q2')),
         ('UNL', ('gpost', lambda M, L: ({}, {'a': DEL, 'b': DEL, 'h': DEL}), '@U0' if lock else '@RET')),
         ('U0', ('store', lambda L: QLOCK, lambda L: 0, True, '@U1')),
         ('U1', ('fwake', lambda L: QLOCK, '@RET')),
         ('RET', ('check', complete, '@END')),
         ('END', ('done',))]
    return assemble(B)


def run(name, issuers_ops, nquiets, variant='', expect_ok=None):
    return report(run_res(name, issuers_ops, nquiets, variant), expect_ok=expect_ok)


def run_res(name, issuers_ops, nquiets, variant=''):
    allops = [k for ops in issuers_ops for k in ops]
    mem = {OPEN: 1, SEALED: 0, ACKED: 0, HS: 0, BIT: 0, QLOCK: 0}
    threads = [issuer(ops, variant) for ops in issuers_ops]
    conn = len(threads)
    threads.append(connection(allops))
    first_quiet = len(threads)
    threads += [quiet(allops, variant) for _ in range(nquiets)]
    sym = [list(range(first_quiet, len(threads)))] if nquiets > 1 else ()
    res = explore(name, threads, mem, idle_ok=lambda i, M, L: i == conn, symmetric=sym)
    return res


CASES = [
    # name, issuers' puts, quiets, variant, expected to pass
    ('design: 2 issuers x 1 put, 2 quiets', [[0], [1]], 2, '', True),
    ('design: issuers of 2 puts and 1 put, 2 quiets', [[0, 1], [2]], 2, '', True),
    ('design: 1 issuer x 3 puts, 2 quiets', [[0, 1, 2]], 2, '', True),
    ('design: 2 issuers x 2 puts, 2 quiets', [[0, 1], [2, 3]], 2, '', True),
    ('design: 1 issuer x 3 puts, 3 quiets', [[0, 1, 2]], 3, '', True),
    ('design: issuers of 2 puts and 1 put, 3 quiets', [[0, 1], [2]], 3, '', True),
    ('design: 3 issuers x 1 put, 2 quiets', [[0], [1], [2]], 2, '', True),
    ('resetstop: 2 issuers x 1 put, 2 quiets', [[0], [1]], 2, 'resetstop', True),
    ('resetstop: issuers of 2 puts and 1 put, 2 quiets', [[0, 1], [2]], 2, 'resetstop', True),
    ('resetstop: 1 issuer x 3 puts, 2 quiets', [[0, 1, 2]], 2, 'resetstop', True),
    ('resetstop: 2 issuers x 2 puts, 2 quiets', [[0, 1], [2, 3]], 2, 'resetstop', True),
    ('resetstop: 1 issuer x 3 puts, 3 quiets', [[0, 1, 2]], 3, 'resetstop', True),
    ('resetstop: 3 issuers x 1 put, 2 quiets', [[0], [1], [2]], 2, 'resetstop', True),
    ('unlocked: 2 issuers x 1 put, 2 quiets', [[0], [1]], 2, 'unlocked', False),
    ('naive: 1 issuer x 2 puts, 2 quiets', [[0, 1]], 2, 'naive', False),
    ('bitfirst: 1 issuer x 2 puts, 2 quiets', [[0, 1]], 2, 'bitfirst', False),
    ('plainmax: 2 issuers x 1 put, 1 quiet', [[0], [1]], 1, 'plainmax', False),
]

if __name__ == '__main__':
    want = select(sys.argv)
    good = True
    print('== does every quiet wait for the puts issued before it was called?', flush=True)
    for name, ops, nq, variant, expect in CASES:
        if want(name):
            good &= run(name, ops, nq, variant, expect)
    print()
    print('ALL AS EXPECTED' if good else 'SOME RESULT UNEXPECTED OR INCOMPLETE')
    sys.exit(0 if good else 1)
