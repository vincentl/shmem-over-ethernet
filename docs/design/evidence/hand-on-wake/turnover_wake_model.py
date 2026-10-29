# SPDX-License-Identifier: LGPL-3.0-or-later
"""P2's hand-on wake for a reopened connection buffer, modelled first
(docs/prompts/v0.2-open-items.md P2; design §3.11's wake count).

Writers reserve room in the open buffer (a CAS on ROOM, each needing `least`);
one that finds too little registers on the turnover word (base/wait-word.h),
asks for a seal (ASK) and sleeps. The agent, whenever the open buffer holds a
reservation, seals it and opens the next (ROOM := CAP, GEN + 1),
then wakes. As built it wakes every registered writer (wake_all). The hand-on
design (hand_on) wakes one; a writer that reserves and leaves room wakes the
next, as a two-sided step (its CAS, then its load of the count); and a woken
writer that finds too little passes the wake on - once a generation, then it
reloads the word before it sleeps, since its own bump changed it.

Properties: liveness (every writer leaves; LOST WAKE, STUCK and TRAPPED are the
engine's), and PASSES: no generation sees more passes than writers.
Breakages
  pass_always   a woken writer passes every time it finds too little: two
                writers too large for the room pass the wake back and forth.
"""
import sys

from engine import DEL, assemble, explore, report, select

ROOM, GEN, WORD, WAITERS, ASK, DONE, PASSES = (('room',), ('gen',), ('word',), ('waiters',),
                                               ('ask',), ('done',), ('passes',))
EVERY = 0xffffffff


def wake(design, nxt):
    """The agent's or a hand-on's wake: every waiter, or one."""
    if design == 'wake_all':
        return ('fwake', lambda L: WORD, nxt)
    return ('fwaken', lambda L: WORD, lambda L: 1, lambda L: EVERY, nxt)


def writer(design, least, writers):
    B = [('F0', ('load', lambda L: ROOM, 'r', '@F1')),
         ('F1', ('br', lambda L, t=least: L['r'] >= t, '@FC', '@REG')),
         ('FC', ('cas', lambda L: ROOM, lambda L: L['r'], lambda L, t=least: L['r'] - t, 'ok',
                 '@FC2')),
         ('FC2', ('br', lambda L: L['ok'], '@GOT', '@F0')),
         ('REG', ('add', lambda L: WAITERS, lambda L: 1, '_', '@W0')),
         ('W0', ('set', 'woke', lambda L: 0, '@P0')),
         ('P0', ('set', 'passed', lambda L: -1, '@LOOP')),
         ('LOOP', ('load', lambda L: WORD, 'w', '@LR')),
         ('LR', ('load', lambda L: ROOM, 'r', '@LB')),
         ('LB', ('br', lambda L, t=least: L['r'] >= t, '@LC', '@ASK')),
         ('LC', ('cas', lambda L: ROOM, lambda L: L['r'], lambda L, t=least: L['r'] - t, 'ok',
                 '@LC2')),
         ('LC2', ('br', lambda L: L['ok'], '@LEAVE', '@LOOP')),
         ('ASK', ('store', lambda L: ASK, lambda L: 1, True, '@PQ'))]
    if design == 'wake_all':
        B += [('PQ', ('br', lambda L: False, '@SLEEP', '@SLEEP'))]
    else:
        once = design != 'pass_always'
        B += [('PQ', ('br', lambda L: L['woke'] == 1 and L['r'] > 0, '@PG', '@SLEEP')),
              ('PG', ('load', lambda L: GEN, 'g', '@PG2')),
              ('PG2', ('br', lambda L, o=once: not o or L['g'] != L['passed'], '@PN', '@SLEEP')),
              ('PN', ('load', lambda L: WAITERS, 'n', '@PB')),
              ('PB', ('br', lambda L: L['n'] > 1, '@PS', '@SLEEP')),
              ('PS', ('set', 'passed', lambda L: L['g'], '@PC')),
              ('PC', ('ghost', lambda M, L: ({PASSES: M[PASSES] + 1}, {}), '@PCK')),
              ('PCK', ('check', lambda M, L, n=writers: M[PASSES] > n * (M[GEN] + 1) and
                       'PASSES: more passes than writers in a generation', '@PW')),
              ('PW', ('add', lambda L: WORD, lambda L: 1, '_', '@PWK')),
              ('PWK', wake(design, '@PAFTER' if design == 'stale_word' else '@LOOP')),
              # stale_word: sleeps on the word it loaded before its own bump.
              ('PAFTER', ('fwait', lambda L: WORD, lambda L: L['w'], '@WOKE'))]
    B += [('SLEEP', ('fwait', lambda L: WORD, lambda L: L['w'], '@WOKE')),
          ('WOKE', ('set', 'woke', lambda L: 1, '@LOOP')),
          ('LEAVE', ('add', lambda L: WAITERS, lambda L: -1, '_', '@GOT'))]
    if design == 'wake_all':
        B += [('GOT', ('br', lambda L: False, '@FIN', '@FIN'))]
    else:
        # The hand-on: room left after this reservation, and anyone registered.
        B += [('GOT', ('br', lambda L, t=least: L['r'] - t > 0, '@HN', '@FIN')),
              ('HN', ('load', lambda L: WAITERS, 'n', '@HB')),
              ('HB', ('br', lambda L: L['n'] != 0, '@HW', '@FIN')),
              ('HW', ('add', lambda L: WORD, lambda L: 1, '_', '@HWK')),
              ('HWK', wake(design, '@FIN'))]
    B += [('FIN', ('add', lambda L: DONE, lambda L: 1, '_', '@CLEAN')),
          ('CLEAN', ('set', 'r', lambda L: DEL, '@CLEAN2')),
          ('CLEAN2', ('set', 'w', lambda L: DEL, '@END')),
          ('END', ('done',))]
    return assemble(B)


def agent(design, cap, writers, opens):
    B = [('A0', ('set', 'k', lambda L: 0, '@TOP')),
         ('TOP', ('br', lambda L, o=opens: L['k'] >= o, '@AEND', '@AW')),
         # Something to send - a reservation since the last open - or everyone gone. A
         # writer's ASK after an open has already met it: the open's bump returns its
         # sleep, and its next look sees the room (the open stores it first).
         ('AW', ('await', lambda M, L, c=cap, n=writers: M[ROOM] < c or M[DONE] == n, '@AD')),
         ('AD', ('load', lambda L: DONE, 'd', '@AD2')),
         ('AD2', ('br', lambda L, n=writers: L['d'] == n, '@AEND', '@CLR')),
         ('CLR', ('store', lambda L: ASK, lambda L: 0, True, '@GEN')),
         ('GEN', ('add', lambda L: GEN, lambda L: 1, '_', '@OPEN')),
         ('OPEN', ('store', lambda L: ROOM, lambda L, c=cap: c, True, '@CNT')),
         ('CNT', ('load', lambda L: WAITERS, 'n', '@ANY')),
         ('ANY', ('br', lambda L: L['n'] != 0, '@BUMP', '@NEXT')),
         ('BUMP', ('add', lambda L: WORD, lambda L: 1, '_', '@WAKE')),
         ('WAKE', wake(design, '@NEXT')),
         ('NEXT', ('set', 'k', lambda L: L['k'] + 1, '@TOP')),
         ('AEND', ('done',))]
    return assemble(B)


def case(design, leasts, cap):
    opens = len(leasts)
    threads = [writer(design, t, len(leasts)) for t in leasts] + [agent(design, cap, len(leasts), opens)]
    mem = {ROOM: cap, GEN: 0, WORD: 0, WAITERS: 0, ASK: 0, DONE: 0, PASSES: 0}
    return f'turnover/{design} cap={cap} leasts={",".join(map(str, leasts))}', threads, mem


CASES = [
    ('wake_all', [1, 1, 2], 2, True),
    ('hand_on', [1, 1], 2, True),
    ('hand_on', [1, 1, 2], 2, True),
    ('hand_on', [2, 2, 1], 2, True),
    ('hand_on', [1, 1, 1], 2, True),
    ('pass_always', [2, 2, 1], 2, 'PASSES'),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, leasts, cap, expect in CASES:
        name, threads, mem0 = case(design, leasts, cap)
        if not keep(name):
            continue
        res = explore(name, threads, mem0, tso=False, max_seconds=600,
                      liveness_max_states=12_000_000)
        ok &= report(res, expect=expect, tso=False)
    sys.exit(0 if ok else 1)
