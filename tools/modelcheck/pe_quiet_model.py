#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""A full quiet never piggybacks on a pe_quiet (design §4.11 item 2, §10).

Spec §9.12.3: shmem_ctx_pe_quiet completes the operations the calling PE issued
on the context to the listed PEs, with shmem_quiet's semantics. §4.11 gives a
shareable context one quiet mutex and one generation word for both: a pe_quiet
bumps the generation, since gets have no per-PE counters and it waits for every
get (§4.6), but it flushes only its listed nodes' dirty bits. A quiet that
finds the generation moved past the value it loaded at its call returns at
once (piggyback) - sound for gets whichever quiet moved it, and unsound for puts
when a pe_quiet did: a put to a node the pe_quiet did not list is still in
flight. So the piggyback test reads a second word, FULL, the generation the
last *full* quiet opened; only a full quiet writes it. A pe_quiet may piggyback
on a full quiet too, since a full quiet covers every node.

Checked at every quiet's return
  GETS  every nonblocking get whose issuing call returned before the quiet was
        called has landed all its bytes (get_completion_model.py's check);
  PUTS  every put whose call returned before the quiet was called, to a node
        the quiet covers - every node for a full quiet, the listed ones for a
        pe_quiet - has been flushed;
  and, by the engine: no lost wake, no stuck thread, AG EF done.

Threads
  getter  issues nonblocking gets on the shareable context, as §4.11 item 1:
          load the generation, add to its pair, re-load, retag on a change.
  putter  issues a put to a node: sets the node's dirty bit, one locked
          fetch_or (§4.11 item 2), and returns.
  lander  the reply path: lands any sent get a byte at a time and credits the
          landed counter of the get's pair, then bumps and wakes the wake word.
  full    shmem_ctx_quiet.
  pe      shmem_ctx_pe_quiet listing some nodes.

A quiet: load the generation (e); take the mutex; load FULL; if FULL > e,
piggyback - unlock and return. Else load the generation (g), store g + 1, and
a full quiet stores FULL = g + 1; wait for equality on pair g mod 2; flush its
nodes; unlock. FULL > e means a full quiet opened a generation above e, so it
bumped after this quiet's call, and completed its wait and its flush of every
node before releasing the mutex this quiet now holds.

The flush of a node is one step here: it clears the node's bit, and every put
whose bit-set preceded the clear counts as flushed. dirty_set_model.py checks
what that step abstracts - the high_seg word, the acknowledgement wait, the
clear and the re-check under the mutex; this model checks only who may skip
the flush.

Designs
  design        as above.
Breakages
  any_bump      the piggyback test reads the generation, as §4.11 item 1 wrote
                it before pe_quiet: a full quiet returns on a pe_quiet's bump
                with an unlisted node's put in flight (PUTS).
  pe_full       a pe_quiet writes FULL too: the same (PUTS).
  at_least      piggyback when FULL >= e: FULL is one past the generation a
                full quiet closed, so this piggybacks on no quiet at all at the
                first call (PUTS, GETS).
Expected to pass
  full_first    no pe_quiet runs: FULL and the generation move together, and
                the rule is item 1's.

Every protocol store is a locked instruction, a CAS, a seq_cst store, or a
store read only under the mutex; runs are SC.

Usage: pe_quiet_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

GEN, FULL, WAKE, QLOCK = ('gen',), ('full',), ('wake',), ('qlock',)
ISS = lambda s: ('iss', s)
LAN = lambda s: ('lan', s)
DIRTY = lambda n: ('dirty', n)
SIZE = lambda k: ('$size', k)
LAND = lambda k: ('$land', k)
TAG = lambda k: ('$tag', k)
SENT = lambda k: ('$sent', k)
RET = lambda k: ('$ret', k)         # a get's or a put's call has returned
BITSET = lambda k: ('$bitset', k)   # a put's dirty bit has been set
FLUSHED = lambda k: ('$flushed', k)


def getter(gets):
    """gets: [(id, size)], issued one after another (get_completion_model.py's design)."""
    B = []
    for n, (k, size) in enumerate(gets):
        p = f'k{k}_'
        nxt = f'@k{gets[n + 1][0]}_I0' if n + 1 < len(gets) else '@END'
        B += [
            (p + 'I0', ('load', lambda L: GEN, 't', '@' + p + 'I1')),
            (p + 'I1', ('nd', lambda M, L, size=size: [(
                {ISS(L['t'] % 2): M[ISS(L['t'] % 2)] + size}, {},
                f'add iss[{L["t"] % 2}] += {size}')], '@' + p + 'I2')),
            (p + 'I2', ('load', lambda L: GEN, 't2', '@' + p + 'I3')),
            (p + 'I3', ('br', lambda L: L['t2'] == L['t'], '@' + p + 'S', '@' + p + 'I4')),
            (p + 'I4', ('nd', lambda M, L, size=size: [(
                {ISS(L['t'] % 2): M[ISS(L['t'] % 2)] - size},
                {'t': DEL, 't2': DEL}, f'sub iss[{L["t"] % 2}] -= {size}')], '@' + p + 'I5')),
            (p + 'I5', ('add', lambda L: WAKE, lambda L: 1, '_', '@' + p + 'I6')),
            (p + 'I6', ('fwake', lambda L: WAKE, '@' + p + 'I0')),
            # the send and the return, folded into the re-load that found no change
            (p + 'S', ('gpost', lambda M, L, k=k: (
                {TAG(k): L['t'], SENT(k): 1, RET(k): 1}, {'t': DEL, 't2': DEL}), nxt)),
        ]
    B.append(('END', ('done',)))
    return assemble(B)


def putter(puts):
    """puts: [(id, node)], each one locked fetch_or of its node's bit, then return."""
    B = []
    for n, (k, node) in enumerate(puts):
        nxt = f'@p{puts[n + 1][0]}' if n + 1 < len(puts) else '@END'
        B.append((f'p{k}', ('nd', lambda M, L, k=k, node=node: [(
            {DIRTY(node): 1, BITSET(k): 1, RET(k): 1}, {},
            f'put {k}: fetch_or dirty[{node}]')], nxt)))
    B.append(('END', ('done',)))
    return assemble(B)


def lander(ids):
    def pick(M, L):
        alts = []
        for k in ids:
            if M[SENT(k)] and M[LAND(k)] < M[SIZE(k)]:
                s = M[TAG(k)] % 2
                alts.append(({LAND(k): M[LAND(k)] + 1, LAN(s): M[LAN(s)] + 1}, {},
                             f'land a byte of get {k}, lan[{s}] += 1'))
        return alts
    return assemble([
        ('L0', ('nd', pick, '@L1')),
        ('L1', ('add', lambda L: WAKE, lambda L: 1, '_', '@L2')),
        ('L2', ('fwake', lambda L: WAKE, '@L0')),
    ])


def quiet(kind, nodes, gets, puts, variant=''):
    """kind 'full' or 'pe'; nodes: the nodes it flushes (every node for a full quiet)."""
    covered = [k for k, node in puts if node in nodes]

    def call(M, L):
        return {}, {'pre': tuple(k for k, _ in gets if M[RET(k)]) +
                           tuple(k for k in covered if M[RET(k)])}

    def complete(M, L):
        bad_gets = [k for k, _ in gets if k in L['pre'] and M[LAND(k)] != M[SIZE(k)]]
        bad_puts = [k for k in covered if k in L['pre'] and not M[FLUSHED(k)]]
        if bad_gets or bad_puts:
            return (f'{kind} quiet returned with prior get(s) {bad_gets} incomplete, '
                    f'prior put(s) {bad_puts} unflushed')
        return None

    def flush(node):
        def step(M, L):
            upd = {DIRTY(node): 0}
            for k, n in puts:
                if n == node and M[BITSET(k)]:
                    upd[FLUSHED(k)] = 1
            return [(upd, {}, f'flush node {node}: clear dirty[{node}]')]
        return step

    piggy_word = GEN if variant == 'any_bump' else FULL
    writes_full = kind == 'full' or variant == 'pe_full'
    B = [('Q0', ('gpre', call, '@Q1')),
         ('Q1', ('load', lambda L: GEN, 'e', '@K0')),
         ('K0', ('cas', lambda L: QLOCK, lambda L: 0, lambda L: 1, 'ok', '@K1')),
         ('K1', ('br', lambda L: L['ok'], '@P0', '@K2')),
         ('K2', ('fwait', lambda L: QLOCK, lambda L: 1, '@K0')),
         ('P0', ('load', lambda L: piggy_word, 'f', '@P1')),
         ('P1', ('br', lambda L: (L['f'] >= L['e'] if variant == 'at_least' else L['f'] > L['e']),
                 '@UNLOCK', '@G0')),
         ('G0', ('load', lambda L: GEN, 'g', '@G1')),
         ('G1', ('store', lambda L: GEN, lambda L: L['g'] + 1, True, '@G2'))]
    if writes_full:
        B += [('G2', ('store', lambda L: FULL, lambda L: L['g'] + 1, True, '@G3'))]
    else:
        B += [('G2', ('set', '_', lambda L: 0, '@G3'))]
    B += [('G3', ('set', 's', lambda L: L['g'] % 2, '@G4')),
          ('G4', ('set', 'g', lambda L: DEL, '@G5')),
          ('G5', ('set', 'e', lambda L: DEL, '@G6')),
          ('G6', ('set', 'f', lambda L: DEL, '@G7')),
          ('G7', ('set', 'ok', lambda L: DEL, '@W0')),
          ('W0', ('load', lambda L: WAKE, 'w', '@W1')),
          ('W1', ('load', lambda L: LAN(L['s']), 'x', '@W2')),
          ('W2', ('load', lambda L: ISS(L['s']), 'y', '@W3')),
          ('W3', ('br', lambda L: L['x'] == L['y'], '@X', '@W4')),
          ('W4', ('set', 'x', lambda L: DEL, '@W5')),
          ('W5', ('set', 'y', lambda L: DEL, '@W6')),
          ('W6', ('fwait', lambda L: WAKE, lambda L: L['w'], '@W0')),
          ('X', ('set', 'x', lambda L: DEL, '@X1')),
          ('X1', ('set', 'y', lambda L: DEL, '@X2')),
          ('X2', ('set', 'w', lambda L: DEL, '@X3')),
          ('X3', ('set', 's', lambda L: DEL, '@F0'))]
    for i, node in enumerate(nodes):
        B.append((f'F{i}', ('nd', flush(node), f'@F{i + 1}')))
    B += [(f'F{len(nodes)}', ('set', '_', lambda L: 0, '@UNLOCK')),
          ('UNLOCK', ('store', lambda L: QLOCK, lambda L: 0, True, '@U1')),
          ('U1', ('fwake', lambda L: QLOCK, '@RET')),
          ('RET', ('check', complete, '@END')),
          ('END', ('done',))]
    return assemble(B)


def run(name, gets_by_thread, puts_by_thread, quiets, nodes, variant='', expect_ok=None):
    gets = [g for thread in gets_by_thread for g in thread]
    puts = [p for thread in puts_by_thread for p in thread]
    mem = {GEN: 0, FULL: 0, WAKE: 0, QLOCK: 0}
    for k, size in gets:
        mem[SIZE(k)] = size
    threads = [getter(t) for t in gets_by_thread]
    threads += [putter(t) for t in puts_by_thread]
    lander_ix = len(threads)
    threads.append(lander([k for k, _ in gets]))
    first_quiet = len(threads)
    for kind, listed in quiets:
        threads.append(quiet(kind, nodes if kind == 'full' else listed, gets, puts, variant))
    # identical quiets are symmetric
    sym = []
    for q in set(quiets):
        ix = [first_quiet + i for i, other in enumerate(quiets) if other == q]
        if len(ix) > 1:
            sym.append(ix)
    res = explore(name, threads, mem, idle_ok=lambda i, M, L: i == lander_ix, symmetric=sym)
    return report(res, expect_ok=expect_ok)


FULL_Q = ('full', ())
PE0 = ('pe', (0,))
PE1 = ('pe', (1,))

CASES = [
    # name, gets by thread (id, size), puts by thread (id, node), quiets, variant, expected to pass
    ('design: a get, a put to node 1, a quiet and a pe_quiet of node 0',
     [[(0, 1)]], [[(1, 1)]], [FULL_Q, PE0], '', True),
    ('design: a get, puts to nodes 0 and 1, a quiet and a pe_quiet of node 0',
     [[(0, 1)]], [[(1, 0), (2, 1)]], [FULL_Q, PE0], '', True),
    ('design: puts to nodes 0 and 1, pe_quiets of node 0 and of node 1',
     [], [[(1, 0)], [(2, 1)]], [PE0, PE1], '', True),
    ('design: a get of 2 bytes, a put to node 1, 2 quiets and a pe_quiet of node 0',
     [[(0, 2)]], [[(1, 1)]], [FULL_Q, FULL_Q, PE0], '', True),
    ('design: 2 gets, a put to node 1, a quiet and a pe_quiet of node 0',
     [[(0, 1)], [(3, 1)]], [[(1, 1)]], [FULL_Q, PE0], '', True),
    ('full_first: a get, a put to node 1, 2 quiets',
     [[(0, 1)]], [[(1, 1)]], [FULL_Q, FULL_Q], 'any_bump', True),
    ('any_bump: a put to node 1, a quiet and a pe_quiet of node 0',
     [], [[(1, 1)]], [FULL_Q, PE0], 'any_bump', False),
    ('any_bump: puts to nodes 0 and 1, pe_quiets of node 0 and of node 1',
     [], [[(1, 0)], [(2, 1)]], [PE0, PE1], 'any_bump', False),
    ('pe_full: a put to node 1, a quiet and a pe_quiet of node 0',
     [], [[(1, 1)]], [FULL_Q, PE0], 'pe_full', False),
    ('at_least: a get, a put to node 1, a quiet',
     [[(0, 1)]], [[(1, 1)]], [FULL_Q], 'at_least', False),
]

if __name__ == '__main__':
    want = select(sys.argv)
    good = True
    print('== does every quiet complete what was issued before it was called, with pe_quiets?',
          flush=True)
    for name, gets, puts, quiets, variant, expect in CASES:
        if want(name):
            good &= run(name, gets, puts, quiets, (0, 1), variant, expect)
    print()
    print('ALL AS EXPECTED' if good else 'SOME RESULT UNEXPECTED OR INCOMPLETE')
    sys.exit(0 if good else 1)
