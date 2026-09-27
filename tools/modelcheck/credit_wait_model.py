#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Waiting for a reply credit, first come first served (design §3.11, §3.8;
phase 1 step 3 slice 2).

A connection's reply credits are shared by its node's PEs: a PE takes one
before a get or a fetching AMO toward that peer, and gives it back when the
reply lands (§3.8). A credit is **indivisible**, so §3.11 asks for a wake of
one waiter rather than a herd, and for a handoff: "wake order is not acquire
order", and a thread already running can take a credit in the window between
a release and the woken waiter's check. §3.11 also warns that a hand-built
FIFO does not help unless acquisition is mediated by the queue.

The design here makes the queue the only way in once anyone waits:

  state   one word: `available` free credits, `next` the next ticket to hand
          out, `served` the tickets already served. `available` is non-zero
          only while no ticket is outstanding (next == served).
  wake    the futex word; a waiter sleeps with the bitset of its ticket.

Take:  if available > 0, take one (CAS). Otherwise take ticket t = next (CAS
       next + 1), and wait: w = load(wake); if served > t, the credit is
       yours; else futex_wait_bitset(wake, w, bit(t)).
Give:  if a ticket is outstanding, serve it (CAS served + 1): the credit
       passes to ticket `served` without ever being free, so nothing running
       can take it. Then add(wake, 1) and futex_wake_bitset(all, bit(served)).
       Otherwise return it to `available` (CAS), with no wake and no syscall.

The bitset makes the wake almost one: only waiters whose ticket shares the
served ticket's bit wake, one in `BITS` classes (32 in the library; 2 in some
cases here, so that classes collide). No waiter count is needed - the queue
is the count - and nothing costs memory per PE.

Designs
  design           as above.
Breakages
  pool_handoff     a give always returns the credit to `available` and wakes
                   one registered waiter, which then takes from the pool - the
                   handoff bit without the queue. A thread arriving while the
                   woken one is not yet running takes it: BARGE.
  wake_any         tickets, but a plain futex_wake of one: the kernel may wake
                   a later ticket's waiter, which finds its ticket unserved
                   and sleeps again, and the served waiter sleeps on. LOST
                   WAKE.
  bump_first       the giver bumps the word before it serves the ticket: a
                   waiter woken by the bump rechecks, finds its ticket not yet
                   served, and sleeps on the new value. LOST WAKE.
  eager_available  a give with a ticket outstanding returns the credit to
                   `available` as well as serving the ticket: two holders of
                   one credit. OVERSUBSCRIBED.

Properties
  OVERSUBSCRIBED  never more credits held than there are.
  BARGE           no thread takes a free credit while a ticket is outstanding.
  liveness        every PE finishes its rounds (the trapped-state check).

Usage: credit_wait_model.py [substring ...]
"""
import sys

from engine import DEL, assemble, explore, report, select

STATE = ('state',)
WAKE = ('wake',)
HELD = ('$held',)
BARGED = ('$barged',)
OVER = ('$over',)
K = 64                   # one field's range in the packed state word


def pack(available, nxt, served):
    return available + K * nxt + K * K * served


def unpack(v):
    return v % K, (v // K) % K, v // (K * K)


def pe(design, rounds, credits, bits):
    """A PE that takes a credit, holds it, and gives it back, `rounds` times."""
    B = [('R0', ('set', 'r', lambda L: 0, '@TOP')),
         ('TOP', ('br', lambda L, n=rounds: L['r'] >= n, '@END', '@TAKE')),
         ('TAKE', ('load', lambda L: STATE, 's', '@TAKE?'))]

    def fast(L):
        a, n, sv = unpack(L['s'])
        return a > 0
    B += [('TAKE?', ('br', fast, '@FAST', '@TICKET')),
          ('FAST', ('cas', lambda L: STATE, lambda L: L['s'], lambda L: L['s'] - 1, 'ok',
                    '@FAST?')),
          ('FAST?', ('br', lambda L: L['ok'], '@GOTF', '@TAKE'))]

    # Taking a free credit: BARGE if a ticket is outstanding at that moment.
    def got_free(M, L):
        a, n, sv = unpack(M[STATE])
        upd = {HELD: M[HELD] + 1}
        if M[HELD] + 1 > credits:
            upd[OVER] = 1
        if design != 'pool_handoff' and n != sv:
            upd[BARGED] = 1
        if design == 'pool_handoff' and M[('$waiting',)] > 0:
            upd[BARGED] = 1
        return upd, {}
    B += [('GOTF', ('gpost', got_free, '@HOLD'))]

    if design == 'pool_handoff':
        # Register as a waiter, take from the pool when woken.
        B += [('TICKET', ('ghost', lambda M, L: ({('$waiting',): M[('$waiting',)] + 1}, {}),
                          '@REG')),
              ('REG', ('add', lambda L: ('waiters',), lambda L: 1, '_', '@WAIT')),
              ('WAIT', ('load', lambda L: WAKE, 'w', '@WCHK')),
              ('WCHK', ('load', lambda L: STATE, 's', '@WCHK?')),
              ('WCHK?', ('br', fast, '@WTAKE', '@WSLEEP')),
              ('WTAKE', ('cas', lambda L: STATE, lambda L: L['s'], lambda L: L['s'] - 1, 'ok',
                         '@WTAKE?')),
              ('WTAKE?', ('br', lambda L: L['ok'], '@WGOT', '@WAIT')),
              ('WSLEEP', ('fwait', lambda L: WAKE, lambda L: L['w'], '@WAIT')),
              ('WGOT', ('add', lambda L: ('waiters',), lambda L: -1, '_', '@WGOT2')),
              ('WGOT2', ('ghost', lambda M, L: ({('$waiting',): M[('$waiting',)] - 1,
                                                 HELD: M[HELD] + 1,
                                                 OVER: 1 if M[HELD] + 1 > credits else M[OVER]},
                                                {}), '@HOLD'))]
    else:
        def with_ticket(L):
            a, n, sv = unpack(L['s'])
            return pack(a, n + 1, sv)
        B += [('TICKET', ('cas', lambda L: STATE, lambda L: L['s'], with_ticket, 'ok',
                          '@TICKET?')),
              ('TICKET?', ('br', lambda L: L['ok'], '@MINE', '@TAKE')),
              ('MINE', ('set', 't', lambda L: unpack(L['s'])[1], '@WAIT')),
              ('WAIT', ('load', lambda L: WAKE, 'w', '@WCHK')),
              ('WCHK', ('load', lambda L: STATE, 's', '@WCHK?')),
              ('WCHK?', ('br', lambda L: unpack(L['s'])[2] > L['t'], '@WGOT', '@WSLEEP'))]
        if design == 'wake_any':
            B += [('WSLEEP', ('fwait', lambda L: WAKE, lambda L: L['w'], '@WAIT'))]
        else:
            B += [('WSLEEP', ('fwaitm', lambda L: WAKE, lambda L: L['w'],
                              lambda L, b=bits: 1 << (L['t'] % b), '@WAIT'))]

        def got_served(M, L):
            upd = {HELD: M[HELD] + 1}
            if M[HELD] + 1 > credits:
                upd[OVER] = 1
            return upd, {'t': DEL}
        B += [('WGOT', ('ghost', got_served, '@HOLD'))]

    # Hold the credit, then give it back.
    B += [('HOLD', ('ghost', lambda M, L: ({HELD: M[HELD] - 1}, {}), '@GIVE')),
          ('GIVE', ('load', lambda L: STATE, 's', '@GIVE?'))]
    if design == 'pool_handoff':
        B += [('GIVE?', ('br', lambda L: True, '@POOL', '@POOL')),
              ('POOL', ('cas', lambda L: STATE, lambda L: L['s'], lambda L: L['s'] + 1, 'ok',
                        '@POOL?')),
              ('POOL?', ('br', lambda L: L['ok'], '@COUNT', '@GIVE')),
              ('COUNT', ('load', lambda L: ('waiters',), 'n', '@ANY')),
              ('ANY', ('br', lambda L: L['n'] > 0, '@PBUMP', '@NEXT')),
              ('PBUMP', ('add', lambda L: WAKE, lambda L: 1, '_', '@PWAKE')),
              ('PWAKE', ('fwaken', lambda L: WAKE, lambda L: 1, lambda L: 0xffffffff, '@NEXT'))]
    else:
        def queued(L):
            a, n, sv = unpack(L['s'])
            return n != sv

        def served_one(L):
            a, n, sv = unpack(L['s'])
            return pack(a + (1 if design == 'eager_available' else 0), n, sv + 1)
        B += [('GIVE?', ('br', queued, '@HAND', '@POOL')),
              ('POOL', ('cas', lambda L: STATE, lambda L: L['s'], lambda L: L['s'] + 1, 'ok',
                        '@POOL?')),
              ('POOL?', ('br', lambda L: L['ok'], '@NEXT', '@GIVE'))]
        if design == 'bump_first':
            B += [('HAND', ('add', lambda L: WAKE, lambda L: 1, '_', '@HCAS')),
                  ('HCAS', ('cas', lambda L: STATE, lambda L: L['s'], served_one, 'ok', '@HCAS?')),
                  ('HCAS?', ('br', lambda L: L['ok'], '@HWAKE', '@GIVE'))]
        else:
            B += [('HAND', ('cas', lambda L: STATE, lambda L: L['s'], served_one, 'ok', '@HAND?')),
                  ('HAND?', ('br', lambda L: L['ok'], '@HBUMP', '@GIVE')),
                  ('HBUMP', ('add', lambda L: WAKE, lambda L: 1, '_', '@HWAKE'))]
        if design == 'wake_any':
            B += [('HWAKE', ('fwaken', lambda L: WAKE, lambda L: 1, lambda L: 0xffffffff,
                             '@NEXT'))]
        else:
            B += [('HWAKE', ('fwaken', lambda L: WAKE, lambda L: 1 << 30,
                             lambda L, b=bits: 1 << (unpack(L['s'])[2] % b), '@NEXT'))]
    B += [('NEXT', ('set', 'r', lambda L: L['r'] + 1, '@CLEAN')),
          ('CLEAN', ('set', 's', lambda L: DEL, '@TOP')),
          ('END', ('done',))]
    return assemble(B)


def check_all(M, L):
    if M[OVER]:
        return 'OVERSUBSCRIBED: more credits held than there are'
    if M[BARGED]:
        return 'BARGE: a free credit was taken while a waiter was queued for one'
    return None


def case(design, rounds, credits, bits):
    threads = [pe(design, r, credits, bits) for r in rounds]
    # The violation flags are sticky, so an observer that waits for either a
    # flag or every PE's end, and checks once, sees every violation - without a
    # check after every step, which would multiply the states.
    threads.append(assemble([('OBS', ('await', lambda M, L, n=len(rounds):
                                      M[OVER] or M[BARGED] or M[('$done',)] == n, '@CHK')),
                             ('CHK', ('check', check_all, '@END')),
                             ('END', ('done',))]))
    mem = {STATE: pack(credits, 0, 0), WAKE: 0, HELD: 0, BARGED: 0, OVER: 0,
           ('waiters',): 0, ('$waiting',): 0, ('$done',): 0}
    shape = ','.join(str(r) for r in rounds)
    return f'credit_wait/{design} rounds={shape} credits={credits} bits={bits}', threads, mem


def with_done(threads, count):
    """Mark each PE's completion in ghost memory, for the observer."""
    out = []
    for prog in threads[:count]:
        prog = list(prog)
        end = len(prog) - 1
        assert prog[end] == ('done',)
        prog[end] = ('ghost', lambda M, L: ({('$done',): M[('$done',)] + 1}, {}), len(prog))
        prog.append(('done',))
        out.append(tuple(prog))
    return out + threads[count:]


# (design, rounds per PE, credits, bitset classes, tso, expect_ok)
CASES = [
    ('design', [1, 1], 1, 32, False, True),
    ('design', [1, 1, 1], 1, 32, False, True),
    ('design', [2, 1, 1], 1, 32, True, True),
    ('design', [1, 1, 1], 1, 2, False, True),
    ('design', [2, 1, 1], 1, 2, False, True),
    ('design', [1, 1, 1], 2, 32, True, True),

    ('pool_handoff', [1, 1, 1], 1, 32, False, False),
    ('wake_any', [1, 1, 1], 1, 32, False, False),
    ('bump_first', [1, 1], 1, 32, False, False),
    ('eager_available', [1, 1, 1], 1, 32, False, False),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, rounds, credits, bits, tso, expect in CASES:
        name, threads, mem0 = case(design, rounds, credits, bits)
        threads = with_done(threads, len(rounds))
        if not keep(name):
            continue
        res = explore(name, threads, mem0, tso=tso, max_seconds=900,
                      liveness_max_states=4_000_000)
        ok &= report(res, expect_ok=expect, tso=tso)
    sys.exit(0 if ok else 1)
