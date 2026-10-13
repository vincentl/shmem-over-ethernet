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

Owners (v0.2 step 4, Part 1; docs/design/progress-helpers-proposal.md §3.2,
§3.3): with H helpers the data connections belong to owners 1 to H - connection
c to owner 1 + c mod H, at bit c div H of that owner's bitmap - and with none, to
owner 0, at bit c, which is the model as it was. Every owner has its own
bitmap, flag and wake word, and runs the same loop on them alone; a writer
addresses its connection's owner by the same arithmetic, with no table. No word
is shared between owners, so each owner is the protocol above on its own
connections, and the map is what is checked:
  wrong_owner      the writer computes the owner as c mod H, without the 1: its
                   bit lands in another owner's bitmap, or in none, and the
                   owner of its connection sleeps on with the commit unhandled.
  mark_wake_apart  the writer marks its connection's owner's bitmap and loads,
                   claims and wakes another owner's flag and word - the next
                   round - as a writer whose mark and wake each computed the
                   owner, and one of them wrongly, would: its owner sleeps on
                   with the bit set. The library names the owner once, for both
                   (osoe_connection_area_owner; the adversarial read of Part 1,
                   N2, whose variant this is).

The property is liveness: every commit is handled by its owner, which exits
when it has handled them all. A lost wake shows as LOST WAKE (the lead asleep with a
bit set and nothing left to wake it). The sweep is not modelled - in the library
it bounds a lost wake to one idle interval, which on the data path is exactly the
latency spike this protocol must not cause.

Usage: lead_wake_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

# Each owner's words. Owner 0 alone, with no helpers, is the model as it was.
def DIRTY(o): return ('dirty', o)
def SLEEPING(o): return ('lead_sleeping', o)
def WAKE(o): return ('wake', o)
# The ring's armed futex wait, a kernel object (the ring designs only): ARMED is
# 1 while one is registered, CQE while its completion waits to be reaped, and
# SOCKET the socket completions still to come.
def ARMED(o): return ('$armed', o)
def CQE(o): return ('$futex_cqe', o)
def SOCKET(o): return ('$socket_completions', o)
def TWO_ARMED(o): return ('$two_armed', o)


RING = ('ring', 'ring_every_idle', 'ring_no_bump')


def owner_of(c, helpers):
    """The owner of data connection c, and its bit there (proposal §3.2)."""
    return (0, c) if helpers == 0 else (1 + c % helpers, c // helpers)


def complete_armed(o):
    """A futex_wake completes owner o's registered wait, if any, whatever its value."""
    return lambda M, L: ({ARMED(o): 0, CQE(o): 1}, {}) if M[ARMED(o)] else ({}, {})


def data(s):
    return ('data', s)


def writer(design, connection, ops, helpers):
    """An application thread committing `ops` operations into `connection`, and
    waking its connection's owner."""
    o, index = owner_of(connection, helpers)
    if design == 'wrong_owner':
        o = connection % helpers                # the map without its 1
    bit = 1 << index
    DIRTY_, SLEEPING_, WAKE_ = DIRTY(o), SLEEPING(o), WAKE(o)
    if design == 'mark_wake_apart':
        w = 1 + (o % helpers)                   # the next owner round, for the wake
        SLEEPING_, WAKE_ = SLEEPING(w), WAKE(w)
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
            B += [(p + 'M', ('or', lambda L: DIRTY_, lambda L, b=bit: b, '_', '@' + p + 'L'))]
        B += [(p + 'L', ('load', lambda L: SLEEPING_, 'z', '@' + p + 'B')),
              (p + 'B', ('br', lambda L: L['z'] == 1, '@' + p + 'Q', '@' + p + 'X'))]
        if design == 'every_writer':
            B += [(p + 'Q', ('set', 'z', lambda L: 1, '@' + p + 'K'))]
        else:
            # Claim the wake: only the writer that takes the flag from 1 to 0
            # wakes; the rest see 0 and skip until the lead raises it again.
            B += [(p + 'Q', ('cas', lambda L: SLEEPING_, lambda L: 1, lambda L: 0, 'z',
                             '@' + p + 'C2')),
                  (p + 'C2', ('br', lambda L: L['z'], '@' + p + 'K', '@' + p + 'X'))]
        if design in ('no_bump', 'ring_no_bump'):
            B += [(p + 'K', ('fwake', lambda L: WAKE_, '@' + p + 'KG'))]
        else:
            B += [(p + 'K', ('add', lambda L: WAKE_, lambda L: 1, '_', '@' + p + 'F')),
                  (p + 'F', ('fwake', lambda L: WAKE_, '@' + p + 'KG'))]
        # The same futex_wake completes the ring's armed wait (the ring designs).
        if design in RING:
            B += [(p + 'KG', ('gpost', complete_armed(o), '@' + p + 'X'))]
        else:
            B += [(p + 'KG', ('set', 'z', lambda L: L['z'], '@' + p + 'X'))]
        B += [(p + 'X', ('set', 'z', lambda L: DEL, nxt))]
    B += [('WEND', ('done',))]
    return assemble(B)


def lead(design, o, owned, total):
    """Owner o's thread - the lead's agent when o is 0 - over the data connections
    `owned`, bit j of its bitmap being owned[j]. Exits once it has handled the
    `total` commits into them."""
    connections = len(owned)
    DIRTY_, SLEEPING_, WAKE_ = DIRTY(o), SLEEPING(o), WAKE(o)
    ARMED_, CQE_, SOCKET_, TWO_ = ARMED(o), CQE(o), SOCKET(o), TWO_ARMED(o)
    B = [('L0', ('set', 'h', lambda L: 0, '@TOP')),
         ('TOP', ('br', lambda L, t=total: L['h'] >= t, '@LEND', '@W')),
         ('W', ('load', lambda L: WAKE_, 'w', '@X'))]
    if design == 'writer_plain':
        # One mark per connection, each cleared with an exchange-like pair of
        # steps: load it, and if set, clear it (a locked op, as exchange is).
        def take(j):
            c = owned[j]
            return [(f'X{j}', ('load', lambda L, s=c: ('mark', s), f'm{j}', f'@X{j}c')),
                    (f'X{j}c', ('br', lambda L, s=j: L[f'm{s}'] == 1, f'@X{j}z', f'@X{j + 1}')),
                    (f'X{j}z', ('cas', lambda L, s=c: ('mark', s), lambda L: 1, lambda L: 0, '_',
                                f'@X{j + 1}'))]
        B += [('X', ('set', 'bits', lambda L: 0, '@X0'))]
        for j in range(connections):
            B += take(j)
        B += [(f'X{connections}', ('set', 'bits', lambda L, n=connections:
                                   sum(1 << j for j in range(n) if L[f'm{j}'] == 1), '@H'))]
    else:
        # exchange(dirty, 0), as a locked and-not of every bit.
        B += [('X', ('andn', lambda L: DIRTY_, lambda L, n=connections: (1 << n) - 1, 'bits', '@H'))]
    # Handle: for every set bit, take everything committed into that connection.
    B += [('H', ('br', lambda L: L['bits'] != 0, '@H0', '@IDLE'))]
    for j in range(connections):
        B += [(f'H{j}', ('br', lambda L, s=j: (L['bits'] >> s) & 1, f'@H{j}l', f'@H{j + 1}')),
              (f'H{j}l', ('load', lambda L, s=owned[j]: data(s), f'd{j}', f'@H{j}g')),
              (f'H{j}g', ('set', 'h', lambda L, s=j, n=connections:
                          sum(L[f'd{i}'] or 0 for i in range(n)), f'@H{j + 1}'))]
    B += [(f'H{connections}', ('set', 'bits', lambda L: DEL, '@TOP'))]
    # Nothing to do: raise the flag, check again, sleep.
    serial = design != 'lead_release'
    B += [('IDLE', ('store', lambda L: SLEEPING_, lambda L: 1, serial, '@RE'))]
    if design == 'no_recheck':
        B += [('RE', ('set', 'again', lambda L: 0, '@DECIDE'))]
    elif design == 'writer_plain':
        B += [('RE', ('set', 'again', lambda L: 0, '@RE0'))]
        for j in range(connections):
            B += [(f'RE{j}', ('load', lambda L, s=owned[j]: ('mark', s), f'r{j}', f'@RE{j}a')),
                  (f'RE{j}a', ('set', 'again', lambda L, s=j: L['again'] | (L[f'r{s}'] == 1),
                               f'@RE{j + 1}'))]
        B += [(f'RE{connections}', ('set', '_', lambda L: 0, '@DECIDE'))]
    else:
        B += [('RE', ('load', lambda L: DIRTY_, 'again', '@DECIDE'))]
    B += [('DECIDE', ('br', lambda L: L['again'] != 0, '@AWAKE', '@SLEEP'))]
    if design not in RING:
        B += [('SLEEP', ('fwait', lambda L: WAKE_, lambda L: L['w'], '@AWAKE'))]
    else:
        # §3.10's loop: arm only if nothing is armed (every_idle: always), then
        # wait for a completion - the futex's, or a socket's.
        def arm(M, L):
            if M[ARMED_] or M[CQE_]:
                return [({TWO_: 1}, {'armed': 1}, 'arm a second futex wait')]
            if M[WAKE_] != L['w']:
                return [({CQE_: 1}, {'armed': 1}, 'arm: the value moved, -EAGAIN at once')]
            return [({ARMED_: 1}, {'armed': 1}, f'arm FUTEX_WAIT(wake {o}, {L["w"]})')]

        def wait(M, L):
            out = []
            if M[CQE_]:
                out.append(({CQE_: 0}, {'armed': 0}, 'reap the futex completion'))
            if M[SOCKET_]:
                out.append(({SOCKET_: M[SOCKET_] - 1}, {}, 'a socket completion'))
            return out                      # blocked while neither: asleep

        def one_armed(M, L):
            return ('AT MOST ONE ARMED: a second IORING_OP_FUTEX_WAIT was armed while '
                    'one was outstanding' if M[TWO_] else None)

        B += [('SLEEP', ('br', lambda L: L['armed'] == 1 and design != 'ring_every_idle',
                         '@RWAIT', '@ARM')),
              ('ARM', ('nd', arm, '@ONE')),
              ('ONE', ('check', one_armed, '@RWAIT')),
              ('RWAIT', ('nd', wait, '@AWAKE'))]
    B += [('AWAKE', ('store', lambda L: SLEEPING_, lambda L: 0, serial, '@TOP')),
          ('LEND', ('done',))]
    return assemble(B)


def case(design, layout, helpers=0):
    """`layout` lists, per writer, (connection, operations); `helpers` is H, the
    data connections then owned by owners 1 to H (proposal §3.2)."""
    connections = max(c for c, _ in layout) + 1
    owners = [0] if helpers == 0 else list(range(1, helpers + 1))
    threads = [writer(design, c, n, helpers) for c, n in layout]
    mem = {}
    for o in owners:
        owned = [c for c in range(connections) if owner_of(c, helpers)[0] == o]
        if not owned:
            continue
        total = sum(n for c, n in layout if c in owned)
        threads.append(lead(design, o, owned, total))
        mem.update({DIRTY(o): 0, SLEEPING(o): 0, WAKE(o): 0, ARMED(o): 0, CQE(o): 0,
                    SOCKET(o): 2, TWO_ARMED(o): 0})
    for s in range(connections):
        mem[data(s)] = 0
        mem[('mark', s)] = 0
    shape = ' '.join(f'c{c}x{n}' for c, n in layout)
    return (f'lead_wake/{design} writers={shape}' + (f' helpers={helpers}' if helpers else ''),
            threads, mem)


# (design, [(connection, ops) per writer], tso, expect[, helpers]); expect is True, or the
# property the case must fail
CASES = [
    ('every_writer', [(0, 1), (1, 1)], True, True),
    ('every_writer', [(0, 2), (1, 2)], True, True),
    ('design', [(0, 1), (1, 1)], False, True),
    ('design', [(0, 1), (1, 1)], True, True),
    ('design', [(0, 2), (0, 1)], True, True),
    ('design', [(0, 2), (1, 2)], True, True),
    ('design', [(0, 1), (1, 1), (0, 1)], True, True),

    ('lead_release', [(0, 1), (1, 1)], False, True),     # SC cannot show it
    ('lead_release', [(0, 1), (1, 1)], True, 'LOST WAKE'),
    ('writer_plain', [(0, 1), (1, 1)], False, True),     # nor this one
    ('writer_plain', [(0, 1), (1, 1)], True, 'LOST WAKE'),
    ('no_recheck', [(0, 1), (1, 1)], False, 'LOST WAKE'),
    ('no_bump', [(0, 1), (1, 1)], False, 'LOST WAKE'),

    ('ring', [(0, 1), (1, 1)], False, True),
    ('ring', [(0, 1), (1, 1)], True, True),
    ('ring', [(0, 2), (1, 2)], True, True),
    ('ring', [(0, 1), (1, 1), (0, 1)], True, True),
    ('ring_every_idle', [(0, 1), (1, 1)], False, 'AT MOST ONE ARMED'),
    ('ring_no_bump', [(0, 1), (1, 1)], False, 'STUCK'),

    # Owners (Part 1): one helper owns every data connection; two split them,
    # connections 0 and 2 to owner 1 and connection 1 to owner 2.
    ('design', [(0, 1), (1, 1)], True, True, 1),
    ('design', [(0, 1), (1, 1), (2, 1)], True, True, 2),
    ('design', [(0, 2), (1, 2)], True, True, 2),
    ('ring', [(0, 1), (1, 1)], True, True, 2),
    ('ring', [(0, 1), (1, 1), (2, 1)], True, True, 2),
    ('lead_release', [(0, 1), (2, 1)], True, 'LOST WAKE', 2),
    ('no_recheck', [(0, 1), (2, 1)], False, 'LOST WAKE', 2),
    ('ring_no_bump', [(0, 1), (2, 1)], False, 'STUCK', 2),
    ('wrong_owner', [(0, 1), (1, 1)], False, 'LOST WAKE', 2),
    ('mark_wake_apart', [(0, 1), (1, 1)], False, 'LOST WAKE', 2),
    ('mark_wake_apart', [(0, 1), (1, 1)], True, 'LOST WAKE', 2),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, layout, tso, expect, *rest in CASES:
        name, threads, mem0 = case(design, layout, *rest)
        if not keep(name):
            continue
        res = explore(name, threads, mem0, tso=tso, max_seconds=900,
                      liveness_max_states=4_000_000)
        ok &= report(res, expect=expect, tso=tso)
    sys.exit(0 if ok else 1)
