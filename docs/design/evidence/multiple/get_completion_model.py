#!/usr/bin/env python3
"""Get completion on a shareable context under SHMEM_THREAD_MULTIPLE (design §4.11).

Spec §9.12.2: quiet "ensures completion of all operations ... issued by the
calling PE on the given context", and in its Notes: "A call to shmem_quiet by a
thread completes the operations posted prior to calling shmem_quiet".

Checked at every quiet return: every nonblocking get whose issuing call returned
before the quiet was called has landed all its bytes. Also checked: no lost wake
and no stuck thread.

Threads
  issuers  issue nonblocking gets one after another. Issuing adds the get's size
           to the context's issued counter, sends the request (from then on its
           reply may land), and returns.
  lander   the reply path. Picks any sent get with bytes outstanding and lands
           one byte: the progress thread writes it and the copy-out actor credits
           the landed counter (one step here; see below). Replies therefore land
           in any order and partially (§3.8 round-robin, several peers).
  quiets   shmem_ctx_quiet on the same context.

Counters are cumulative and never reset. The wake word is bumped after every
counter change a waiter may need, and a waiter loads it before the counters
(§4.6 "Waiting on 64-bit counters").

Designs
  equality   §4.6 as written: one issued/landed pair; wait for equality.
  snapshot   review D6 item 2: snapshot issued at entry; wait for landed >= it.
  design     §4.11: a generation word and two counter pairs chosen by its parity.
             The issuer adds to the pair of the generation it loaded, re-loads the
             generation, and if it changed, takes the add back, bumps the wake word
             and starts again. Quiets hold a per-context mutex. The holder bumps
             the generation and waits for equality on the pair it closed. A quiet
             that finds the generation past the value it loaded at entry returns
             at once (piggyback).
Breakages of the design
  order      loads issued before landed. Expected to pass: once late adds are
             taken back, no byte of a later get can land in the closed pair.
  noretag    the issuer never re-loads the generation.
  nolock     quiets do not take the mutex, and so cannot piggyback.
  nopiggy    no piggyback. Expected to pass: piggyback is only an optimization.

Starvation. The design's claim is that a quiet never waits for a get that began
issuing after the quiet's wait began: after its generation bump, or at the call for
equality, which has no bump. The ghost records when each issue starts, and the
table reports, over all interleavings, the most such gets one quiet waited for.

Reductions (engine.py): the call instant of a quiet is folded into its first
step (gpre), and the send and return of an issue into its last (gpost). Landing
a byte and crediting it are one step: in any schedule with steps between them,
moving the landing next to the credit keeps every failed check failing, since
checks fail on bytes not yet landed. Quiets are symmetric.

Every protocol store is a locked instruction or a CAS, so x86-TSO adds no
interleavings; runs are SC.

Usage: get_completion_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

GEN, WAKE, QLOCK = ('gen',), ('wake',), ('qlock',)
ISS = lambda s: ('iss', s)
LAN = lambda s: ('lan', s)
SIZE = lambda k: ('$size', k)
LAND = lambda k: ('$land', k)
TAG = lambda k: ('$tag', k)
SENT = lambda k: ('$sent', k)
RET = lambda k: ('$ret', k)
INSLOT = lambda k: ('$in', k)      # slot + 1 while the get's size is counted in a slot
STARTED = lambda k: ('$started', k)
EXTRA = ('$extra',)                 # most gets a quiet waited for that started after its wait began


def slot_of(design, g):
    return g % 2 if design == 'design' else 0


def issuer(design, ops, variant=''):
    B = []
    for n, (k, size) in enumerate(ops):
        p = f'k{k}_'
        nxt = f'@k{ops[n + 1][0]}_G' if n + 1 < len(ops) else '@END'
        send = (p + 'S', ('gpost', lambda M, L, k=k: (
            {TAG(k): L['t'] or 0, SENT(k): 1, RET(k): 1}, {'t': DEL, 't2': DEL}), nxt))
        B += [(p + 'G', ('gpre', lambda M, L, k=k: ({STARTED(k): 1}, {}), '@' + p + 'I0'))]
        if design != 'design':
            B += [(p + 'I0', ('nd', lambda M, L, k=k, size=size: [(
                {ISS(0): M[ISS(0)] + size, INSLOT(k): 1}, {}, f'add iss[0] += {size}')], '@' + p + 'S')), send]
            continue
        B += [
            (p + 'I0', ('load', lambda L: GEN, 't', '@' + p + 'I1')),
            # a locked add; the ghost "counted in slot" is recorded in the same step
            (p + 'I1', ('nd', lambda M, L, k=k, size=size: [(
                {ISS(L['t'] % 2): M[ISS(L['t'] % 2)] + size, INSLOT(k): L['t'] % 2 + 1},
                {}, f'add iss[{L["t"] % 2}] += {size}')], '@' + p + ('S' if variant == 'noretag' else 'I2'))),
        ]
        if variant != 'noretag':
            B += [
                (p + 'I2', ('load', lambda L: GEN, 't2', '@' + p + 'I3')),
                (p + 'I3', ('br', lambda L: L['t2'] == L['t'], '@' + p + 'S', '@' + p + 'I4')),
                (p + 'I4', ('nd', lambda M, L, k=k, size=size: [(
                    {ISS(L['t'] % 2): M[ISS(L['t'] % 2)] - size, INSLOT(k): 0},
                    {'t': DEL, 't2': DEL}, f'sub iss[{L["t"] % 2}] -= {size}')], '@' + p + 'I5')),
                (p + 'I5', ('add', lambda L: WAKE, lambda L: 1, '_', '@' + p + 'I6')),
                (p + 'I6', ('fwake', lambda L: WAKE, '@' + p + 'I0')),
            ]
        B.append(send)
    B.append(('END', ('done',)))
    return assemble(B)


def lander(design, allops):
    def pick(M, L):
        alts = []
        for k in allops:
            if M[SENT(k)] and M[LAND(k)] < M[SIZE(k)]:
                s = slot_of(design, M[TAG(k)])
                alts.append(({LAND(k): M[LAND(k)] + 1, LAN(s): M[LAN(s)] + 1}, {},
                             f'land a byte of get {k}, lan[{s}] += 1'))
        return alts
    return assemble([
        ('L0', ('nd', pick, '@L1')),
        ('L1', ('add', lambda L: WAKE, lambda L: 1, '_', '@L2')),
        ('L2', ('fwake', lambda L: WAKE, '@L0')),
    ])


def quiet(design, allops, variant='', measure=False):
    def call(M, L):
        return {}, {'pre': tuple(k for k in allops if M[RET(k)])}

    def wait_begins(M, L):
        return {}, {'old': tuple(k for k in allops if M[STARTED(k)])} if measure else {}

    def complete(M, L):
        bad = [k for k in L['pre'] if M[LAND(k)] != M[SIZE(k)]]
        return f'quiet returned with prior get(s) {bad} incomplete' if bad else None

    def extra(M, L):
        if not measure:
            return {}
        n = sum(1 for k in allops if M[INSLOT(k)] == L['s'] + 1 and k not in L['old'])
        return {EXTRA: max(M[EXTRA], n)}

    wait = [
        ('W0', ('load', lambda L: WAKE, 'w', '@W1')),
        ('W1', ('load', lambda L: ISS(L['s']) if variant == 'order' else LAN(L['s']), 'x', '@W2')),
        ('W2', ('load', lambda L: LAN(L['s']) if variant == 'order' else ISS(L['s']), 'y', '@W3')),
        ('W3', ('br', lambda L: L['x'] == L['y'], '@X', '@W4')),
        ('W4', ('set', 'x', lambda L: DEL, '@W5')),
        ('W5', ('set', 'y', lambda L: DEL, '@W6')),
        ('W6', ('fwait', lambda L: WAKE, lambda L: L['w'], '@W0')),
        ('X', ('gpost', lambda M, L: (extra(M, L), {'x': DEL, 'y': DEL, 'w': DEL}), '@DONEWAIT')),
    ]
    if design == 'snapshot':
        B = [('Q0', ('gpre', call, '@Q1')),
             ('Q1', ('load', lambda L: ISS(0), 'snap', '@S0')),
             ('S0', ('load', lambda L: WAKE, 'w', '@S1')),
             ('S1', ('load', lambda L: LAN(0), 'x', '@S2')),
             ('S2', ('br', lambda L: L['x'] >= L['snap'], '@RET', '@S3')),
             ('S3', ('fwait', lambda L: WAKE, lambda L: L['w'], '@S0'))]
    elif design == 'equality':
        B = [('Q0', ('gpre', call, '@Q1')),
             ('Q1', ('set', 's', lambda L: 0, '@Q2')),
             ('Q2', ('gpost', wait_begins, '@W0'))] + wait + [
             ('DONEWAIT', ('set', '_', lambda L: 0, '@RET'))]
    else:
        lock = variant != 'nolock'
        piggy = variant not in ('nolock', 'nopiggy')
        B = [('Q0', ('gpre', call, '@Q1')),
             ('Q1', ('load', lambda L: GEN, 'e', '@K0' if lock else '@G0'))]
        if lock:
            B += [('K0', ('cas', lambda L: QLOCK, lambda L: 0, lambda L: 1, 'ok', '@K1')),
                  ('K1', ('br', lambda L: L['ok'], '@G0', '@K2')),
                  ('K2', ('fwait', lambda L: QLOCK, lambda L: 1, '@K0'))]
        B += [('G0', ('load', lambda L: GEN, 'g', '@G1')),
              ('G1', ('br', lambda L: piggy and L['g'] > L['e'], '@DONEWAIT', '@G2')),
              ('G2', ('store', lambda L: GEN, lambda L: L['g'] + 1, True, '@G2b')),    # seq_cst store
              ('G2b', ('gpost', wait_begins, '@G3')),
              ('G3', ('set', 's', lambda L: L['g'] % 2, '@G4')),
              ('G4', ('set', 'g', lambda L: DEL, '@G5')),
              ('G5', ('set', 'e', lambda L: DEL, '@G6')),
              ('G6', ('set', 'ok', lambda L: DEL, '@W0'))] + wait
        if lock:
            B += [('DONEWAIT', ('store', lambda L: QLOCK, lambda L: 0, True, '@U1')),
                  ('U1', ('fwake', lambda L: QLOCK, '@RET'))]
        else:
            B += [('DONEWAIT', ('set', '_', lambda L: 0, '@RET'))]
    B += [('RET', ('check', complete, '@END')),
          ('END', ('done',))]
    return assemble(B)


def run(name, design, issuers_ops, nquiets, variant='', expect_ok=None, measure=False):
    res = run_res(name, design, issuers_ops, nquiets, variant, measure)
    return report(res, expect_ok=expect_ok), res


def run_res(name, design, issuers_ops, nquiets, variant='', measure=False):
    allops = [k for ops in issuers_ops for k, _ in ops]
    mem = {GEN: 0, WAKE: 0, QLOCK: 0}
    for ops in issuers_ops:
        for k, size in ops:
            mem[SIZE(k)] = size
    threads = [issuer(design, ops, variant) for ops in issuers_ops]
    threads.append(lander(design, allops))
    first_quiet = len(threads)
    threads += [quiet(design, allops, variant, measure) for _ in range(nquiets)]
    lander_ix = len(issuers_ops)
    sym = [list(range(first_quiet, len(threads)))] if nquiets > 1 else ()
    res = explore(name, threads, mem, idle_ok=lambda i, M, L: i == lander_ix,
                  track=(EXTRA,), symmetric=sym)
    return res


CASES = [
    # name, design, issuers' gets as (id, size), quiets, variant, expected to pass
    ('equality: 2 issuers, 1 quiet', 'equality', [[(0, 2)], [(1, 1)]], 1, '', True),
    ('snapshot: 2 issuers, 1 quiet', 'snapshot', [[(0, 2)], [(1, 1)]], 1, '', False),
    ('design: 2 issuers, 2 quiets', 'design', [[(0, 2)], [(1, 1)]], 2, '', True),
    ('design: 1 issuer x 2 gets, 2 quiets', 'design', [[(0, 1), (1, 2)]], 2, '', True),
    ('design: issuers of 2 gets and 1 get, 2 quiets', 'design', [[(0, 1), (1, 2)], [(2, 2)]], 2, '', True),
    ('design: 2 issuers x 2 gets, 2 quiets', 'design', [[(0, 1), (1, 1)], [(2, 1), (3, 1)]], 2, '', True),
    ('design: 1 issuer x 2 gets, 3 quiets', 'design', [[(0, 1), (1, 2)]], 3, '', True),
    ('design: 2 issuers, 3 quiets', 'design', [[(0, 1)], [(1, 2)]], 3, '', True),
    ('design/nopiggy: 2 issuers, 2 quiets', 'design', [[(0, 2)], [(1, 1)]], 2, 'nopiggy', True),
    ('design/order: 2 issuers, 2 quiets', 'design', [[(0, 2)], [(1, 1)]], 2, 'order', True),
    ('design/noretag: 1 issuer, 2 quiets', 'design', [[(0, 1)]], 2, 'noretag', False),
    ('design/nolock: 1 issuer, 2 quiets', 'design', [[(0, 1)]], 2, 'nolock', False),
]

if __name__ == '__main__':
    want = select(sys.argv)
    good = True
    print('== safety: does every quiet complete the gets issued before it was called?', flush=True)
    for name, design, ops, nq, variant, expect in CASES:
        if want(name):
            good &= run(name, design, ops, nq, variant, expect)[0]
    if want('starvation'):
        print()
        print('== starvation: most gets one quiet waited for that began issuing after its wait began')
        print('   (2 issuers, each issuing N one-byte gets; maximum over all interleavings)', flush=True)
        for design in ('equality', 'design'):
            row = []
            # N = 4 is about 1.7 M states for equality but, growing some 20x per step,
            # a few hundred million for the design: more memory than the table is worth
            for n in (1, 2, 3):
                ops = [[(k, 1) for k in range(n)], [(k, 1) for k in range(n, 2 * n)]]
                ok, res = run(f'starvation {design}: 2 issuers x {n} gets', design, ops, 1, measure=True)
                good &= ok and res.ok
                row.append(res.max_ghost.get(EXTRA, 0))
                print(f'   {design}, N = {n}: waited for {row[-1]} gets begun after the wait began', flush=True)
            print(f'   {design:<9} N = 1..3 -> {row}', flush=True)
    print()
    print('ALL AS EXPECTED' if good else 'SOME RESULT UNEXPECTED OR INCOMPLETE')
