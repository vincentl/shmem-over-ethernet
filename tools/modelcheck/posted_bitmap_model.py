#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Which team blocks have an arrival posted: a bitmap behind a summary (step D, D5).

The node lead's agent takes a node's arrivals out of each team block's handoff
word (arrival_model.py, which checks one word a team is enough). Until the team
redesign's step D it read every team's word every pass; the proposal's D5 puts
a bitmap of posted blocks behind a summary word - a bit for each group of the
bitmap's words - so that a pass with nothing posted costs one load, however
many teams the node holds.

A member that posts stores the epoch into its block's handoff word, a release
store; sets its block's bit in the bitmap and its group's bit in the summary,
each a locked or; and then bumps the lead's wake word and wakes it. The agent
loads the wake word first (arrival_model.py's rule), exchanges the summary with
zero, exchanges with zero each bitmap word whose group the summary named, and
reads the handoff word of each block whose bit was set, taking a value it has
not taken (the whole word compared with the one it took last); it loads the
summary before exchanging it, and exchanges it only when it is not zero, so that
a pass with nothing posted writes nothing. A pass that takes nothing sleeps on
the wake value it loaded first.

Checked
  ORDER    each team's arrivals reach the root once each, in epoch order.
  and, by the engine: no stuck thread - a post the agent never takes leaves its
  member waiting for a release that never comes - and from every reachable
  state the run can still finish (AG EF done).

Threads
  member b   one local member of team b, the one that posts, for EPOCHS
             epochs; three teams, blocks 0 and 1 in group 0 and block 2 in
             group 1, so that both the summary and a word with two bits are met.
  lead       the agent.
  root       the internode round trip, abstracted: it releases a team's epoch
             at any moment after its arrival is forwarded.

Designs
  design            as above.
Breakages
  summary_first     the member sets its group's summary bit before its block's
                    bit: the agent can take the summary, find the word empty,
                    and the bit set after it is never looked for.
  wake_first        the member bumps the wake word before setting the bits: the
                    agent loads the bumped value, finds nothing, and sleeps on it.
  bits_first        the member sets the bits before storing the epoch: the agent
                    takes the bits, reads the old word, and the new one is never
                    looked for.
  clear_after       the agent loads the summary and clears it after it has taken
                    the words, rather than exchanging it: a summary bit set in
                    between is cleared unread.
  kept_bits         the agent loads a bitmap word rather than exchanging it, so
                    a bit it took stays set, and keeps no record of the word it
                    took last (internode-am.c's `consumed`): a later pass that
                    the summary sends to that word takes the bit again and
                    forwards the same arrival a second time. ORDER alone fails
                    it - the root takes no epoch twice and the agent counts
                    only what moved, so nothing waits - and either half alone
                    holds: this design hands the agent no word it has taken (the
                    step D review, correctness G3), and the comparison with the
                    word last taken passes over a kept bit's.

Usage: posted_bitmap_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

TEAMS, EPOCHS = 3, 2
GROUP = (0, 0, 1)                         # block b's word of the bitmap
HANDOFF = lambda b: ('handoff', b)
POSTED = lambda g: ('posted', g)          # block b's bit: 1 << b
SUMMARY = ('summary',)                    # group g's bit: 1 << g
WAKE = ('wake',)
RELEASED = lambda b: ('released', b)
FORWARDED = lambda b: ('forwarded', b)
DONE = ('$done',)


def member(design, b):
    g = GROUP[b]
    B = []
    for k in range(EPOCHS):
        e, p = k + 1, f'm{k}_'
        nxt = f'@m{k + 1}_S' if k + 1 < EPOCHS else '@E'
        store = (p + 'S', ('store', lambda L: HANDOFF(b), lambda L, e=e: e, False, '@' + p + 'B'))
        bit = (p + 'B', ('or', lambda L: POSTED(g), lambda L: 1 << b, '_', '@' + p + 'G'))
        summary = (p + 'G', ('or', lambda L: SUMMARY, lambda L: 1 << g, '_', '@' + p + 'K'))
        wake = [(p + 'K', ('add', lambda L: WAKE, lambda L: 1, '_', '@' + p + 'F')),
                (p + 'F', ('fwake', lambda L: WAKE, '@' + p + 'W'))]
        wait = (p + 'W', ('await', lambda M, L, e=e: M[RELEASED(b)] == e, nxt))
        if design == 'summary_first':
            B += [store,
                  (p + 'B', ('or', lambda L: SUMMARY, lambda L: 1 << g, '_', '@' + p + 'G')),
                  (p + 'G', ('or', lambda L: POSTED(g), lambda L: 1 << b, '_', '@' + p + 'K'))]
            B += wake + [wait]
        elif design == 'wake_first':
            B += [store,
                  (p + 'B', ('add', lambda L: WAKE, lambda L: 1, '_', '@' + p + 'F')),
                  (p + 'F', ('fwake', lambda L: WAKE, '@' + p + 'G')),
                  (p + 'G', ('or', lambda L: POSTED(g), lambda L: 1 << b, '_', '@' + p + 'H')),
                  (p + 'H', ('or', lambda L: SUMMARY, lambda L: 1 << g, '_', '@' + p + 'W')),
                  wait]
        elif design == 'bits_first':
            B += [(p + 'S', ('or', lambda L: POSTED(g), lambda L: 1 << b, '_', '@' + p + 'B')),
                  (p + 'B', ('or', lambda L: SUMMARY, lambda L: 1 << g, '_', '@' + p + 'G')),
                  (p + 'G', ('store', lambda L: HANDOFF(b), lambda L, e=e: e, False,
                             '@' + p + 'K'))]
            B += wake + [wait]
        else:
            B += [store, bit, summary] + wake + [wait]
    B += [('E', ('ghost', lambda M, L: ({DONE: M[DONE] + 1}, {}), '@END')),
          ('END', ('done',))]
    return assemble(B)


def lead(design):
    """The agent: a pass, then a sleep if it took nothing, until every arrival
    of every team is forwarded."""
    def forward(M, L):
        """The root takes each team's next epoch once, and the agent counts
        what it forwarded: an arrival out of order moves neither, so that only
        ORDER, checked at 'FO', sees one."""
        b = L['b']
        if L['h'] != M[FORWARDED(b)] + 1:
            return {}, {}
        took = {'took': 1, 'n': (L['n'] or 0) + 1}
        if design != 'kept_bits':
            took[f'c{b}'] = L['h']
        return {FORWARDED(b): M[FORWARDED(b)] + 1}, took

    def order(M, L):
        b = L['b']
        if L['h'] == (L[f'c{b}'] or 0):
            return None                 # not new: nothing to forward
        return (None if L['h'] == M[FORWARDED(b)] + 1 else
                f"team {b}: epoch {L['h']} forwarded after {M[FORWARDED(b)]}: an arrival was "
                f"{'repeated' if L['h'] <= M[FORWARDED(b)] else 'skipped'}")

    B = [('L', ('br', lambda L: (L['n'] or 0) >= TEAMS * EPOCHS, '@LEND', '@W')),
         ('W', ('load', lambda L: WAKE, 'w', '@S0')),
         ('S0', ('set', 'took', lambda L: 0, '@S'))]
    if design == 'clear_after':
        B += [('S', ('load', lambda L: SUMMARY, 's', '@G0'))]
    else:
        # A load first, as the library does, and the exchange only when it is
        # not zero: a pass with nothing posted writes nothing.
        B += [('S', ('load', lambda L: SUMMARY, 's', '@SZ')),
              ('SZ', ('br', lambda L: L['s'] != 0, '@SX', '@D')),
              ('SX', ('andn', lambda L: SUMMARY, lambda L: (1 << 2) - 1, 's', '@G0'))]
    # Each group the summary named, in order; each block of it whose bit was set.
    if design == 'kept_bits':
        take = ('GX', ('load', lambda L: POSTED(L['g']), 'x', '@B0'))
    else:
        take = ('GX', ('andn', lambda L: POSTED(L['g']), lambda L: (1 << TEAMS) - 1, 'x',
                       '@B0'))
    B += [('G0', ('set', 'g', lambda L: 0, '@G')),
          ('G', ('br', lambda L: L['g'] >= 2, '@D', '@GS')),
          ('GS', ('br', lambda L: (L['s'] >> L['g']) & 1, '@GX', '@GN')),
          take,
          ('B0', ('set', 'b', lambda L: 0, '@BL')),
          ('BL', ('br', lambda L: L['b'] >= TEAMS, '@GN', '@BS')),
          ('BS', ('br', lambda L: (L['x'] >> L['b']) & 1 and GROUP[L['b']] == L['g'],
                  '@BH', '@BN')),
          ('BH', ('load', lambda L: HANDOFF(L['b']), 'h', '@FO')),
          ('FO', ('check', order, '@FW')),
          ('FW', ('br', lambda L: L['h'] != (L[f"c{L['b']}"] or 0), '@FF', '@BN')),
          ('FF', ('ghost', forward, '@BN')),
          ('BN', ('set', 'b', lambda L: L['b'] + 1, '@BL')),
          ('GN', ('set', 'g', lambda L: L['g'] + 1, '@G'))]
    if design == 'clear_after':
        B += [('D', ('andn', lambda L: SUMMARY, lambda L: L['s'], '_', '@D2')),
              ('D2', ('br', lambda L: L['took'], '@L', '@SLEEP'))]
    else:
        B += [('D', ('br', lambda L: L['took'], '@L', '@SLEEP'))]
    B += [('SLEEP', ('fwait', lambda L: WAKE, lambda L: L['w'], '@L')),
          ('LEND', ('ghost', lambda M, L: ({DONE: M[DONE] + 1},
                                           {k: DEL for k in ('w', 's', 'g', 'x', 'b', 'h',
                                                             'took')}), '@END')),
          ('END', ('done',))]
    return assemble(B)


def root():
    """Release any team's next epoch once its arrival is forwarded."""
    def ready(M, L):
        return [({RELEASED(b): M[RELEASED(b)] + 1}, {'r': (L['r'] or 0) + 1}, f'release {b}')
                for b in range(TEAMS) if M[FORWARDED(b)] > M[RELEASED(b)]]
    return assemble([
        ('R', ('br', lambda L: (L['r'] or 0) >= TEAMS * EPOCHS, '@E', '@RW')),
        ('RW', ('await', lambda M, L: any(M[FORWARDED(b)] > M[RELEASED(b)]
                                          for b in range(TEAMS)), '@RN')),
        ('RN', ('nd', ready, '@R')),
        ('E', ('ghost', lambda M, L: ({DONE: M[DONE] + 1}, {}), '@END')),
        ('END', ('done',))])


def case(design):
    threads = [member(design, b) for b in range(TEAMS)] + [lead(design), root()]
    mem = {SUMMARY: 0, WAKE: 0, DONE: 0}
    for b in range(TEAMS):
        mem[HANDOFF(b)] = mem[RELEASED(b)] = mem[FORWARDED(b)] = 0
    for g in range(2):
        mem[POSTED(g)] = 0
    return f'posted_bitmap/{design} teams={TEAMS} epochs={EPOCHS}', threads, mem


CASES = [
    ('design', True),
    ('summary_first', False),
    ('wake_first', False),
    ('bits_first', False),
    ('clear_after', False),
    ('kept_bits', False),
]


def main():
    want = select(sys.argv)
    ok = True
    for design, expect in CASES:
        name, threads, mem = case(design)
        if not want(name):
            continue
        res = explore(name, threads, mem, max_seconds=1800, liveness_max_states=6_000_000,
                      done=lambda M: M[DONE] == TEAMS + 2)
        ok &= report(res, expect_ok=expect)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
