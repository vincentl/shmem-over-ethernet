#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""An application thread waiting on the lead, without a syscall per event
(design §3.11, §4.6; phase 1 step 3 slice 2).

Two of the waits the SHMEM layer blocks in are for something only the lead's
agent moves: quiet waits for the peer's acknowledged sequence to reach its
writes, and a writer that found no room waits for a connection buffer to turn
over. Step 2 polled both (`poll_lead`: wake the lead, sleep 20 us, look again),
which cost a put and quiet 4x the floor and a stream 97% of it (§8.4). §4.6's
shape is a companion 32-bit word per waitable value - the progress thread
moves the value, then bumps the word, then wakes; the waiter loads the word,
then compares the value, then sleeps on the word - and it has one cost the
stand-in never had to answer: an acknowledgement lands on every quiet's path,
and a futex_wake for each, waiter or none, is a syscall per event on the
agent's thread (second review, atomics F3). So the agent must know whether
anyone waits, and the wait becomes two-sided:

  level     the value waited on (the acknowledged sequence, the turnover);
            only the agent stores it, and it only grows.
  word      the futex word: bumped after the level moves, if anyone waits.
  waiters   how many threads are registered on the word.

Waiter (target t):  add(waiters, 1); loop: w = load(word); if load(level) >= t,
                    leave; futex_wait(word, w). Leaving: add(waiters, -1).
Agent (per step):   store(level, v); if load(waiters) != 0: add(word, 1);
                    futex_wake(word, all).

**This is a two-sided protocol (§3.16)**: the waiter stores (registers) and
then loads the level; the agent stores the level and then loads the count.
Only among seq_cst operations does C11 forbid both missing each other, so the
level's store and both loads are seq_cst and the registration is a locked RMW.
The word is bumped before the futex_wake and the waiter sleeps on the value it
loaded *before* its check: a wake that lands between the check and the
futex_wait changes the word, and the wait returns at once. Both classes that
use this wake **every** waiter (§3.11: the wake count tracks divisibility):
each quiet has its own target, and each writer its own reservation to retry.

Designs
  design         as above.
  always_wake    the agent bumps and wakes on every step, waiter or none:
                 correct, and the syscall per event the design exists to
                 remove. Kept as the baseline.
Breakages, each losing a wake
  count_first    the agent loads the count before it stores the level.
                 Loses a wake under SC.
  register_late  the waiter loads the word and checks the level before it
                 registers. Loses a wake under SC.
  release_level  the level's store is a release store and the agent's load of
                 the count is not serializing: under x86-TSO the store sits in
                 the agent's store buffer while it reads a count of 0.
  wake_one       futex_wake wakes one waiter, as for an indivisible resource:
                 a waiter whose target is not yet reached absorbs the wake,
                 and one whose target is sleeps on.
  no_bump        futex_wake without changing the word: a wake that lands
                 before the futex_wait finds no one, and the wait then sleeps
                 on an unchanged value.

The property is liveness: every waiter leaves, and the agent finishes its
steps. A lost wake shows as LOST WAKE (a waiter asleep with its target reached
and nothing left to wake it), and the trapped-state check runs on every case.
The sweep is not modelled: in the library it bounds a lost wake to one sweep
interval, which on quiet's path is exactly the latency this protocol removes.

Usage: wait_word_model.py [substring ...]
"""
import sys

from engine import DEL, assemble, explore, report, select

LEVEL = ('level',)
WORD = ('word',)
WAITERS = ('waiters',)
BASELINES = ('always_wake',)


def waiter(design, target):
    """An application thread waiting for the level to reach `target`."""
    B = []
    if design == 'register_late':
        B += [('W', ('load', lambda L: WORD, 'w', '@C')),
              ('C', ('load', lambda L: LEVEL, 'v', '@R')),
              ('R', ('add', lambda L: WAITERS, lambda L: 1, '_', '@DEC')),
              ('DEC', ('br', lambda L, t=target: L['v'] >= t, '@LEAVE', '@SLEEP')),
              ('SLEEP', ('fwait', lambda L: WORD, lambda L: L['w'], '@W2')),
              # After the first sleep, the loop is the design's.
              ('W2', ('load', lambda L: WORD, 'w', '@C2')),
              ('C2', ('load', lambda L: LEVEL, 'v', '@DEC2')),
              ('DEC2', ('br', lambda L, t=target: L['v'] >= t, '@LEAVE', '@SLEEP'))]
    else:
        B += [('R', ('add', lambda L: WAITERS, lambda L: 1, '_', '@W')),
              ('W', ('load', lambda L: WORD, 'w', '@C')),
              ('C', ('load', lambda L: LEVEL, 'v', '@DEC')),
              ('DEC', ('br', lambda L, t=target: L['v'] >= t, '@LEAVE', '@SLEEP')),
              ('SLEEP', ('fwait', lambda L: WORD, lambda L: L['w'], '@W'))]
    B += [('LEAVE', ('add', lambda L: WAITERS, lambda L: -1, '_', '@CLEAN')),
          ('CLEAN', ('set', 'w', lambda L: DEL, '@END')),
          ('END', ('done',))]
    return assemble(B)


def agent(design, steps):
    """The lead's agent: moves the level from 0 to `steps`, one step at a time."""
    serial = design != 'release_level'
    B = [('A0', ('set', 'v', lambda L: 0, '@TOP')),
         ('TOP', ('br', lambda L, s=steps: L['v'] >= s, '@AEND', '@NEXT')),
         ('NEXT', ('set', 'v', lambda L: L['v'] + 1, '@STEP0'))]
    if design == 'count_first':
        B += [('STEP0', ('load', lambda L: WAITERS, 'n', '@STORE')),
              ('STORE', ('store', lambda L: LEVEL, lambda L: L['v'], True, '@ANY'))]
    else:
        B += [('STEP0', ('store', lambda L: LEVEL, lambda L: L['v'], serial, '@COUNT')),
              ('COUNT', ('load', lambda L: WAITERS, 'n', '@ANY'))]
    if design in BASELINES:
        B += [('ANY', ('br', lambda L: True, '@BUMP', '@TOP'))]
    else:
        B += [('ANY', ('br', lambda L: L['n'] != 0, '@BUMP', '@TOP'))]
    if design == 'no_bump':
        B += [('BUMP', ('fwake', lambda L: WORD, '@TOP'))]
    elif design == 'wake_one':
        B += [('BUMP', ('add', lambda L: WORD, lambda L: 1, '_', '@WAKE')),
              ('WAKE', ('fwaken', lambda L: WORD, lambda L: 1, lambda L: 0xffffffff, '@TOP'))]
    else:
        B += [('BUMP', ('add', lambda L: WORD, lambda L: 1, '_', '@WAKE')),
              ('WAKE', ('fwake', lambda L: WORD, '@TOP'))]
    B += [('AEND', ('done',))]
    return assemble(B)


def case(design, targets):
    steps = max(targets)
    threads = [waiter(design, t) for t in targets] + [agent(design, steps)]
    mem = {LEVEL: 0, WORD: 0, WAITERS: 0}
    shape = ','.join(str(t) for t in targets)
    return f'wait_word/{design} targets={shape}', threads, mem


# (design, waiters' targets, tso, expect): True, or the property it must fail
CASES = [
    ('always_wake', [1, 2], True, True),
    ('design', [1], False, True),
    ('design', [1, 2], False, True),
    ('design', [1, 2], True, True),
    ('design', [2, 2], True, True),
    ('design', [1, 2, 2], True, True),

    ('count_first', [1], False, 'LOST WAKE'),
    ('register_late', [1], False, 'LOST WAKE'),
    ('release_level', [1], False, True),     # SC cannot show it
    ('release_level', [1], True, 'LOST WAKE'),
    ('wake_one', [1, 2], False, 'LOST WAKE'),
    ('no_bump', [1], False, 'LOST WAKE'),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, targets, tso, expect in CASES:
        name, threads, mem0 = case(design, targets)
        if not keep(name):
            continue
        res = explore(name, threads, mem0, tso=tso, max_seconds=900,
                      liveness_max_states=4_000_000)
        ok &= report(res, expect=expect, tso=tso)
    sys.exit(0 if ok else 1)
