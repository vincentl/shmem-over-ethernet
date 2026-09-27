#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Waking the lead only when it sleeps (design §3.10, §3.12; phase 1 step 2 slice 3, C2).

Every remote operation used to end with a locked add on the lead's wake word and
a futex_wake **syscall**, whether or not the lead was asleep - L-way contention
on one node-wide line and a syscall per put (second review, D12). The
replacement has two words:

  dirty           the connection area's dirty-connection bitmap: a writer that
                  commits into connection s sets bit s (fetch_or);
  lead_sleeping   1 while the lead is about to sleep or asleep, 0 otherwise.

Writer:  commit; fetch_or(dirty, bit s); if load(lead_sleeping) == 1 and
         cas(lead_sleeping, 1, 0) succeeds: add(wake, 1); futex_wake(wake).
Lead:    w = load(wake); bits = exchange(dirty, 0); handle bits; if anything,
         go round. Otherwise store(lead_sleeping, 1); if load(dirty) != 0,
         store(lead_sleeping, 0) and go round; else futex_wait(wake, w), and on
         waking store(lead_sleeping, 0).

**This is a two-sided protocol (§3.16)**: the writer stores its bit and then
loads the flag; the lead stores the flag and then loads the bits. C11 forbids
both missing each other only among seq_cst operations, so the flag's store and
both loads are seq_cst and the bit is set by a locked RMW. The wake word is
still bumped before the futex_wake, and the lead waits on the value of it
loaded *before* its pass: a wake that lands between the lead's check and its
futex_wait changes the word and the wait returns at once (§3.10's rule).

Designs
  design           as above, and the wake is **claimed**: a writer that reads the
                   flag as 1 takes it to 0 with a CAS and wakes only if its CAS
                   won. Without the claim every writer during the lead's wake-up
                   latency sees 1 and makes the syscall - measured, 93% of the
                   puts of a burst still woke the lead.
  every_writer     every writer that reads 1 wakes: correct, and the syscall per
                   put the design exists to remove. Kept as the baseline.
Breakages, each losing a wake
  lead_release     the lead's store of lead_sleeping is a release store: under
                   x86-TSO it sits in the store buffer while the lead loads the
                   bits, and a writer that set its bit meanwhile reads 0.
  writer_plain     the writer marks its connection with a plain release store
                   (a per-connection byte, say) instead of a locked RMW: under
                   TSO the lead's check misses the buffered mark and the writer
                   misses the flag.
  no_recheck       the lead sleeps without loading the bits after raising the
                   flag. Loses a wake under SC.
  no_bump          the writer calls futex_wake without changing the wake word.
                   A wake that lands before the lead's futex_wait is lost, and
                   the wait sleeps on an unchanged value. Loses a wake under SC.

The ring (phase 1 step 3, design §3.10)
  ring             the lead no longer sleeps in futex_wait: it waits in one
                   io_uring submission that carries its sockets' completions
                   and **at most one armed IORING_OP_FUTEX_WAIT** on the wake
                   word. Arming compares and registers at once - the value
                   differs, and the wait completes at once with -EAGAIN - and a
                   futex_wake completes a registered wait whatever its value.
                   The lead arms only when none is armed, so a wait armed in an
                   earlier idle period, on an older value, is the one it sleeps
                   on after a socket completion woke it: §3.10's claim is that
                   this stays correct, because every bump of the word is
                   followed by a futex_wake, which completes it. Socket
                   completions are modelled as spurious wakes of the lead, up
                   to two, which is what leaves a wait armed across periods.
  ring_every_idle  arms a new wait on every idle pass while one is still armed
                   - the earlier draft that armed 1,881 and overflowed the
                   completion queue (review D3). Fails AT MOST ONE ARMED, the
                   assumption the CQ bound rests on (§3.3).
  ring_no_bump     the claimed wake calls futex_wake without bumping the word.
                   A wake that lands before the arm finds nothing registered,
                   and the arm then compares an unchanged value: still a lost
                   wake, with the ring as without it.

The property is liveness: every commit is handled by the lead, which exits when
it has handled them all. A lost wake shows as LOST WAKE (the lead asleep with a
bit set and nothing left to wake it). The sweep is not modelled - in the library
it bounds a lost wake to one idle interval, which on the data path is exactly the
latency spike this protocol must not cause.

Usage: lead_wake_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

DIRTY = ('dirty',)
SLEEPING = ('lead_sleeping',)
WAKE = ('wake',)
# The ring's armed futex wait, a kernel object (the ring designs only): ARMED is
# 1 while one is registered, CQE while its completion waits to be reaped, and
# SOCKET the socket completions still to come.
ARMED = ('$armed',)
CQE = ('$futex_cqe',)
SOCKET = ('$socket_completions',)
RING = ('ring', 'ring_every_idle', 'ring_no_bump')


def complete_armed(M, L):
    """A futex_wake completes the registered wait, if any, whatever its value."""
    return ({ARMED: 0, CQE: 1}, {}) if M[ARMED] else ({}, {})


def data(s):
    return ('data', s)


def writer(design, connection, ops):
    """An application thread committing `ops` operations into `connection`."""
    bit = 1 << connection
    B = []
    for k in range(ops):
        p = f'w{k}_'
        nxt = f'@w{k + 1}_C' if k + 1 < ops else '@WEND'
        # The commit itself: the bytes the lead will seal (a release store,
        # §3.16 single-writer publication in the model's terms).
        B += [(p + 'C', ('add', lambda L, s=connection: data(s), lambda L: 1, '_', '@' + p + 'M'))]
        if design == 'writer_plain':
            B += [(p + 'M', ('store', lambda L, s=connection: ('mark', s), lambda L: 1, False,
                             '@' + p + 'L'))]
        else:
            B += [(p + 'M', ('or', lambda L: DIRTY, lambda L, b=bit: b, '_', '@' + p + 'L'))]
        B += [(p + 'L', ('load', lambda L: SLEEPING, 'z', '@' + p + 'B')),
              (p + 'B', ('br', lambda L: L['z'] == 1, '@' + p + 'Q', '@' + p + 'X'))]
        if design == 'every_writer':
            B += [(p + 'Q', ('set', 'z', lambda L: 1, '@' + p + 'K'))]
        else:
            # Claim the wake: only the writer that takes the flag from 1 to 0
            # wakes; the rest see 0 and skip until the lead raises it again.
            B += [(p + 'Q', ('cas', lambda L: SLEEPING, lambda L: 1, lambda L: 0, 'z',
                             '@' + p + 'C2')),
                  (p + 'C2', ('br', lambda L: L['z'], '@' + p + 'K', '@' + p + 'X'))]
        if design in ('no_bump', 'ring_no_bump'):
            B += [(p + 'K', ('fwake', lambda L: WAKE, '@' + p + 'KG'))]
        else:
            B += [(p + 'K', ('add', lambda L: WAKE, lambda L: 1, '_', '@' + p + 'F')),
                  (p + 'F', ('fwake', lambda L: WAKE, '@' + p + 'KG'))]
        # The same futex_wake completes the ring's armed wait (the ring designs).
        if design in RING:
            B += [(p + 'KG', ('gpost', complete_armed, '@' + p + 'X'))]
        else:
            B += [(p + 'KG', ('set', 'z', lambda L: L['z'], '@' + p + 'X'))]
        B += [(p + 'X', ('set', 'z', lambda L: DEL, nxt))]
    B += [('WEND', ('done',))]
    return assemble(B)


def lead(design, connections, total):
    """The lead's agent. Exits once it has handled `total` commits."""
    B = [('L0', ('set', 'h', lambda L: 0, '@TOP')),
         ('TOP', ('br', lambda L, t=total: L['h'] >= t, '@LEND', '@W')),
         ('W', ('load', lambda L: WAKE, 'w', '@X'))]
    if design == 'writer_plain':
        # One mark per connection, each cleared with an exchange-like pair of
        # steps: load it, and if set, clear it (a locked op, as exchange is).
        def take(j):
            return [(f'X{j}', ('load', lambda L, s=j: ('mark', s), f'm{j}', f'@X{j}c')),
                    (f'X{j}c', ('br', lambda L, s=j: L[f'm{s}'] == 1, f'@X{j}z', f'@X{j + 1}')),
                    (f'X{j}z', ('cas', lambda L, s=j: ('mark', s), lambda L: 1, lambda L: 0, '_',
                                f'@X{j + 1}'))]
        B += [('X', ('set', 'bits', lambda L: 0, '@X0'))]
        for j in range(connections):
            B += take(j)
        B += [(f'X{connections}', ('set', 'bits', lambda L, n=connections:
                                   sum(1 << j for j in range(n) if L[f'm{j}'] == 1), '@H'))]
    else:
        # exchange(dirty, 0), as a locked and-not of every bit.
        B += [('X', ('andn', lambda L: DIRTY, lambda L, n=connections: (1 << n) - 1, 'bits', '@H'))]
    # Handle: for every set bit, take everything committed into that connection.
    B += [('H', ('br', lambda L: L['bits'] != 0, '@H0', '@IDLE'))]
    for j in range(connections):
        B += [(f'H{j}', ('br', lambda L, s=j: (L['bits'] >> s) & 1, f'@H{j}l', f'@H{j + 1}')),
              (f'H{j}l', ('load', lambda L, s=j: data(s), f'd{j}', f'@H{j}g')),
              (f'H{j}g', ('set', 'h', lambda L, s=j, n=connections:
                          sum(L[f'd{i}'] or 0 for i in range(n)), f'@H{j + 1}'))]
    B += [(f'H{connections}', ('set', 'bits', lambda L: DEL, '@TOP'))]
    # Nothing to do: raise the flag, check again, sleep.
    serial = design != 'lead_release'
    B += [('IDLE', ('store', lambda L: SLEEPING, lambda L: 1, serial, '@RE'))]
    if design == 'no_recheck':
        B += [('RE', ('set', 'again', lambda L: 0, '@DECIDE'))]
    elif design == 'writer_plain':
        B += [('RE', ('set', 'again', lambda L: 0, '@RE0'))]
        for j in range(connections):
            B += [(f'RE{j}', ('load', lambda L, s=j: ('mark', s), f'r{j}', f'@RE{j}a')),
                  (f'RE{j}a', ('set', 'again', lambda L, s=j: L['again'] | (L[f'r{s}'] == 1),
                               f'@RE{j + 1}'))]
        B += [(f'RE{connections}', ('set', '_', lambda L: 0, '@DECIDE'))]
    else:
        B += [('RE', ('load', lambda L: DIRTY, 'again', '@DECIDE'))]
    B += [('DECIDE', ('br', lambda L: L['again'] != 0, '@AWAKE', '@SLEEP'))]
    if design not in RING:
        B += [('SLEEP', ('fwait', lambda L: WAKE, lambda L: L['w'], '@AWAKE'))]
    else:
        # §3.10's loop: arm only if nothing is armed (every_idle: always), then
        # wait for a completion - the futex's, or a socket's.
        def arm(M, L):
            if M[ARMED] or M[CQE]:
                return [({'$two_armed': 1}, {'armed': 1}, 'arm a second futex wait')]
            if M[WAKE] != L['w']:
                return [({CQE: 1}, {'armed': 1}, 'arm: the value moved, -EAGAIN at once')]
            return [({ARMED: 1}, {'armed': 1}, f'arm FUTEX_WAIT(wake, {L["w"]})')]

        def wait(M, L):
            out = []
            if M[CQE]:
                out.append(({CQE: 0}, {'armed': 0}, 'reap the futex completion'))
            if M[SOCKET]:
                out.append(({SOCKET: M[SOCKET] - 1}, {}, 'a socket completion'))
            return out                      # blocked while neither: asleep

        def one_armed(M, L):
            return ('AT MOST ONE ARMED: a second IORING_OP_FUTEX_WAIT was armed while '
                    'one was outstanding' if M['$two_armed'] else None)

        B += [('SLEEP', ('br', lambda L: L['armed'] == 1 and design != 'ring_every_idle',
                         '@RWAIT', '@ARM')),
              ('ARM', ('nd', arm, '@ONE')),
              ('ONE', ('check', one_armed, '@RWAIT')),
              ('RWAIT', ('nd', wait, '@AWAKE'))]
    B += [('AWAKE', ('store', lambda L: SLEEPING, lambda L: 0, serial, '@TOP')),
          ('LEND', ('done',))]
    return assemble(B)


def case(design, layout):
    """`layout` lists, per writer, (connection, operations)."""
    connections = max(c for c, _ in layout) + 1
    total = sum(n for _, n in layout)
    threads = [writer(design, c, n) for c, n in layout] + [lead(design, connections, total)]
    mem = {DIRTY: 0, SLEEPING: 0, WAKE: 0, ARMED: 0, CQE: 0, SOCKET: 2, '$two_armed': 0}
    for s in range(connections):
        mem[data(s)] = 0
        mem[('mark', s)] = 0
    shape = ' '.join(f'c{c}x{n}' for c, n in layout)
    return f'lead_wake/{design} writers={shape}', threads, mem


# (design, [(connection, ops) per writer], tso, expect_ok)
CASES = [
    ('every_writer', [(0, 1), (1, 1)], True, True),
    ('every_writer', [(0, 2), (1, 2)], True, True),
    ('design', [(0, 1), (1, 1)], False, True),
    ('design', [(0, 1), (1, 1)], True, True),
    ('design', [(0, 2), (0, 1)], True, True),
    ('design', [(0, 2), (1, 2)], True, True),
    ('design', [(0, 1), (1, 1), (0, 1)], True, True),

    ('lead_release', [(0, 1), (1, 1)], False, True),     # SC cannot show it
    ('lead_release', [(0, 1), (1, 1)], True, False),
    ('writer_plain', [(0, 1), (1, 1)], False, True),     # nor this one
    ('writer_plain', [(0, 1), (1, 1)], True, False),
    ('no_recheck', [(0, 1), (1, 1)], False, False),
    ('no_bump', [(0, 1), (1, 1)], False, False),

    ('ring', [(0, 1), (1, 1)], False, True),
    ('ring', [(0, 1), (1, 1)], True, True),
    ('ring', [(0, 2), (1, 2)], True, True),
    ('ring', [(0, 1), (1, 1), (0, 1)], True, True),
    ('ring_every_idle', [(0, 1), (1, 1)], False, False),
    ('ring_no_bump', [(0, 1), (1, 1)], False, False),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, layout, tso, expect in CASES:
        name, threads, mem0 = case(design, layout)
        if not keep(name):
            continue
        res = explore(name, threads, mem0, tso=tso, max_seconds=900,
                      liveness_max_states=4_000_000)
        ok &= report(res, expect_ok=expect, tso=tso)
    sys.exit(0 if ok else 1)
