#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""A split that finds its node's pool empty: dry, or a block on its way back (step D).

A team's local lead claims a block for it from the node's pool before the
split's reduction (team_block_model.py has the block's life). A destroy gives
the block back only once the last of the team's members on the node has left
it, after the destroy's sync, so a split just after a destroy can find the
pool empty for a block a moment from free. The split must not fail for that
block when the program ordered the destroy before it - through the lead's own
program order, the lead having been in the team, or through the split's
reduction, the team's members on the node all in its parent - and must not run
its rounds forever for a block that is not coming back. The design:

  - a pool found empty in the split's first round makes the round busy: the
    claim may have come before a parent PE's destroy, which gives its blocks
    back before that PE arrives in the round's reduction, and so before the
    next round's claim;
  - from the second round on it makes the round busy while a block is on its
    way back, and fails the split otherwise. A block keeps its last team's
    member count after that team has left it, and a claim zeroes it: a
    nonzero count with fewer holders says the team is leaving it, or has left
    and the block is not yet in the bitmap, or is in it again; a zero count
    says claimed, or never named, and then the bitmap's bit alone says free.
    The namer writes the holders before the count and the reader reads the
    count first, so that a block being named reads as claimed or as held; one
    read between its claim and the claim's zeroing reads as on its way back,
    which costs a busy round and nothing more (the model does not count
    rounds).

The adversarial read of 2026-10-03 found the first design's test - fewer
holders than members, the members never zeroed - true for ever of a block
given back and claimed again (`first`): a split_2d whose lead holds one claimed
block and finds no second ran busy rounds for ever. The fix it suggested, the
test made only of a held block (an odd generation), misses a block between its
last holder's generation raise and its bit (`odd_only`).

The guarantee stops there, and the read of the rebuilt part showed where: a
team whose member on the node is outside the split's parent, past the destroy's
sync and not yet gone, holds its block with every holder, and the split may
fail for it (`remote`, which LEGIT allows). No test the node can make tells
that member from one still waiting in the destroy's sync for a PE inside the
split: a flag its members set before arriving in that sync (`departing`, the
read's suggestion) makes the split wait for the first, and run its rounds for
ever for the second (`split_first`), whose destroy waits for the split.

A team of one claims no block (P7, node layout 42): it touches nothing of the
node's, and a split_2d with an xrange of 1 made one a PE, four of which emptied
the default pool at 256 PEs a node. So a lead claims a block for each team of
more than one PE it leads, and none for its teams of one (`one`).

Checked
  LEGIT    a split fails dry only when every block it has not claimed was, when
           the lead looked, a live team's, or one held by a member outside the
           split's parent - none free, none of a team the guarantee covers.
  SPARE    a split whose teams of more than one PE need no more blocks than the
           pool has free does not fail dry (P7).
  and, by the engine: no stuck thread, and from every reachable state the run can
  still finish (AG EF done) - no busy rounds for ever.

Cases (one node; the reductions abstract)
  member    a pool of one block, held by team T of leads A and member M. A
            leaves T, then splits a parent of its own and leads the new team
            here; M leaves T at any time; C, another lead of another split the
            program does not order, claims a block whenever one is free and
            names it for a live team.
  parent    a pool of one block, held by T = {M1, M2}; both leave T and then
            arrive in the split's reduction, which A, not in T, leads a team of.
  remote    a pool of one block, held by T, whose one member here, M, is not
            in the split's parent; X, T's member elsewhere and a parent PE,
            arrives in the split's reduction once T's sync - M in it - has
            completed; M leaves whenever it runs again.
  split_first  T = {X, M} as in remote, but X splits first and destroys T after:
            M waits in T's sync for X, who is in the split.
  two       a pool of two: block 0 given back by an old team and free, its
            flag still set, block 1 a live team's; A leads two teams of one
            split, as split_2d's row and column can be, claims block 0 and
            finds no second.
  one       a pool of two: block 0 free, block 1 a live team's; A leads two
            teams of one split, a team of one and one of more than one - a
            split_2d's row of one PE and its column - and claims a block for
            the second alone.

Designs
  design            as above.
Breakages
  first             the first design: fewer holders than members, and the members
                    never zeroed - A's own claimed block reads as on its way
                    back for ever (TRAPPED, in two).
  odd_only          the test made only of a block with an odd generation: a block
                    read between its last holder's raise and its bit is neither
                    (LEGIT, in member).
  zero_at_leave     the last holder zeroes the members as it leaves, the claim not:
                    a block given back and not yet in the bitmap reads as claimed
                    (LEGIT, in member).
  first_round_final a pool found empty in the first round fails the split: A's
                    claim came before M1 and M2 left (LEGIT, in parent).
  departing         a flag each member sets before it arrives in the destroy's
                    sync makes its block on its way back: the split waits for a
                    team whose destroy waits for the split (TRAPPED, in
                    split_first).
  singleton_claims  a team of one claims a block as any team does, as before P7:
                    A takes the free block for its team of one and finds none
                    for the other (SPARE, in one).

Memory orders (T29; the step D review's portability Q4). Every case runs under
SC and under x86-TSO, the stores as the library makes them (§3.16): a store the
library makes `release` is buffered (`serial=False`); a locked instruction, a
syscall or a `seq_cst` store drains the buffer. The orders are node.c's and
team.c's:
  claim     osoe_node_team_claim: the swap is a seq_cst compare-and-swap; the
            members' zeroing after it a release store (buffered)
  name      osoe_node_team_name: the generation, the holders and the members,
            in that order, release stores (buffered; the namer's stores reach
            memory in the order made, so a reader that loads the members first
            and the holders after never sees this team's members beside the
            last team's holders)
  leave     osoe_node_team_leave: the holders' decrement, the generation's raise
            and the bit's or are seq_cst read-modify-writes
  returning osoe_node_team_returning: the members, acquire; the holders and the
            bit, seq_cst; loads all
  The flag of `departing` and the last holder's zeroing of `zero_at_leave` are
  breakages never built: stored as the library would store such words, with
  release, and buffered.
  A sync's or a reduction's arrival (team.c, osoe_epoch_bump_eq) is a seq_cst
  add, which drains the buffer: the sync arrivals below are adds, and under TSO
  a claimer's reduction and a namer's split sync are a locked or of nothing
  on a word nothing else reads (`fence`), where the library has them; the SC runs
  have no such step. The verdicts are the same under both.

Usage: pool_dry_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

FREE = ('free',)                          # bit b set: block b is free
GEN = lambda b: ('gen', b)
HOLDERS = lambda b: ('holders', b)
MEMBERS = lambda b: ('members', b)
DEPARTING = lambda b: ('departing', b)    # departing: set by its team's members before the destroy's sync
OWNER = lambda b: ('$owner', b)           # whose the block is: one of OWNERS
ARRIVED = ('arrived',)                    # parent, remote: arrivals the reduction waits for
T_SYNC = ('t_sync',)                      # split_first: arrivals in T's destroy sync
SPLIT_DONE = ('$split_done',)             # split_first: A's split has returned
DONE = ('$done',)

FENCE = ('fence',)                        # only the fences touch it, and it never changes

FREE_OWNER, T, A, C, L, T_OUTSIDE = 0, 1, 2, 3, 4, 5
OWNERS = {FREE_OWNER: 'nobody (free)', T: 'T, destroyed before the split', A: 'A', C: 'C',
          L: 'a live team', T_OUTSIDE: 'T, its member here outside the parent and not yet gone'}

# Each case: the blocks, their state - generation, holders, members,
# departing, free, owner - and the threads. Block 0 of `two` keeps the flag
# its old team set as it left, which a claim must make irrelevant.
CASES_STATE = {
    'member': [(1, 2, 2, 0, False, T)],
    'parent': [(1, 2, 2, 0, False, T)],
    'remote': [(1, 1, 1, 0, False, T_OUTSIDE)],
    'split_first': [(1, 1, 1, 0, False, L)],
    'two': [(2, 0, 2, 1, True, FREE_OWNER), (1, 1, 1, 0, False, L)],
    'one': [(0, 0, 0, 0, True, FREE_OWNER), (1, 1, 1, 0, False, L)],
}
# The arrivals the split's reduction waits for after its first round.
ARRIVALS = {'parent': 2, 'remote': 1, 'split_first': 1}


def fence(p, then, tso):
    """Under TSO, a locked or of nothing: the seq_cst read-modify-write the
    library makes at this point (a reduction's or a sync's arrival), which drains
    the store buffer on x86. Under SC nothing: a local step that is no step."""
    if tso:
        return [(p, ('or', lambda L: FENCE, lambda L: 0, '_', then))]
    return [(p, ('br', lambda L: True, then, then))]


def depart(design, b, p, then):
    """`departing` only: a member about to arrive in its team's destroy sync
    marks the block."""
    if design != 'departing':
        return [(p, ('br', lambda L: True, then, then))]
    # Never built; a single-writer flag, which the library would store with release.
    return [(p, ('store', lambda L: DEPARTING(b), lambda L: 1, False, then))]


def leave(design, b, p, then):
    """One member leaves block b: holders down by one, and the last holder
    raises the generation and sets the bit (osoe_node_team_leave, node.c: each a
    seq_cst read-modify-write)."""
    B = [(p, ('add', lambda L: HOLDERS(b), lambda L: -1, 'left', '@' + p + 'k')),
         (p + 'k', ('br', lambda L: L['left'] == 0, '@' + p + 'z', then))]
    if design == 'zero_at_leave':
        # Never built: the claim's zeroing (release) moved to the last holder's leave.
        B += [(p + 'z', ('store', lambda L: MEMBERS(b), lambda L: 0, False, '@' + p + 'g'))]
    else:
        B += [(p + 'z', ('br', lambda L: True, '@' + p + 'g', '@' + p + 'g'))]
    B += [(p + 'g', ('add', lambda L: GEN(b), lambda L: 1, '_', '@' + p + 'f')),
          (p + 'f', ('or', lambda L: FREE, lambda L: 1 << b, '_', '@' + p + 'o')),
          (p + 'o', ('gpost', lambda M, L: ({OWNER(b): FREE_OWNER}, {'left': DEL}), then))]
    return B


def returning(design, blocks, p, yes, no):
    """osoe_node_team_returning, block by block in the library's order. Each
    load also notes, in local `o<b>`, whose block b was at that instant, so
    that LEGIT judges what the lead saw rather than what a member did after."""
    def seen(nxt):
        return ('gpost', lambda M, L: ({}, {f"o{L['b']}": M[OWNER(L['b'])]}), nxt)
    B = [(p, ('set', 'b', lambda L: 0, '@' + p + 'L'))]
    nxt = '@' + p + 'N'
    B += [(p + 'L', ('br', lambda L: L['b'] >= blocks, no, '@' + p + 'S'))]
    if design == 'odd_only':
        B += [(p + 'S', ('load', lambda L: GEN(L['b']), 'g', '@' + p + 'Sg')),
              (p + 'Sg', seen('@' + p + 'G')),
              (p + 'G', ('br', lambda L: L['g'] % 2 == 1, '@' + p + 'M', nxt)),
              (p + 'M', ('load', lambda L: MEMBERS(L['b']), 'm', '@' + p + 'H')),
              (p + 'H', ('load', lambda L: HOLDERS(L['b']), 'h', '@' + p + 'Hs')),
              (p + 'Hs', seen('@' + p + 'C')),
              (p + 'C', ('br', lambda L: L['h'] < L['m'], yes, nxt))]
    elif design == 'first':
        B += [(p + 'S', ('load', lambda L: MEMBERS(L['b']), 'm', '@' + p + 'H')),
              (p + 'H', ('load', lambda L: HOLDERS(L['b']), 'h', '@' + p + 'Hs')),
              (p + 'Hs', seen('@' + p + 'C')),
              (p + 'C', ('br', lambda L: L['h'] < L['m'], yes, nxt))]
    else:
        B += [(p + 'S', ('load', lambda L: MEMBERS(L['b']), 'm', '@' + p + 'Z')),
              (p + 'Z', ('br', lambda L: L['m'] != 0, '@' + p + 'H', '@' + p + 'F')),
              (p + 'H', ('load', lambda L: HOLDERS(L['b']), 'h', '@' + p + 'Hs')),
              (p + 'Hs', seen('@' + p + 'C')),
              (p + 'C', ('br', lambda L: L['h'] < L['m'], yes,
                         '@' + p + 'P' if design == 'departing' else nxt)),
              (p + 'P', ('load', lambda L: DEPARTING(L['b']), 'd', '@' + p + 'Ps')),
              (p + 'Ps', seen('@' + p + 'Q')),
              (p + 'Q', ('br', lambda L: L['d'], yes, nxt)),
              (p + 'F', ('load', lambda L: FREE, 'f', '@' + p + 'Fs')),
              (p + 'Fs', seen('@' + p + 'B')),
              (p + 'B', ('br', lambda L: (L['f'] >> L['b']) & 1, yes, nxt))]
    B += [(p + 'N', ('set', 'b', lambda L: L['b'] + 1, '@' + p + 'L'))]
    return B


def claim(design, who, p, got, dry):
    """osoe_node_team_claim: the highest free bit by compare-and-swap (seq_cst), then
    the block's members zeroed (but in `first` and `zero_at_leave`). The pools here
    are one word, so a lost swap's move to the word a local id names (P9)
    stays in it, as a retry does."""
    B = [(p, ('load', lambda L: FREE, 'w', '@' + p + 'e')),
         (p + 'e', ('br', lambda L: L['w'] == 0, dry, '@' + p + 'p')),
         (p + 'p', ('set', 'c', lambda L: L['w'].bit_length() - 1, '@' + p + 'x')),
         (p + 'x', ('cas', lambda L: FREE, lambda L: L['w'], lambda L: L['w'] & ~(1 << L['c']),
                    'ok', '@' + p + 'o')),
         (p + 'o', ('gpost', lambda M, L: ({OWNER(L['c']): who} if L['ok'] else {}, {}),
                    '@' + p + 'k')),
         (p + 'k', ('br', lambda L: L['ok'], '@' + p + 'z', '@' + p))]
    if design in ('first', 'zero_at_leave'):
        B += [(p + 'z', ('br', lambda L: True, got, got))]
    else:
        # osoe_node_team_claim, node.c: the members' zeroing after the swap is a release
        # store, buffered under TSO.
        B += [(p + 'z', ('store', lambda L: MEMBERS(L['c']), lambda L: 0, False, got))]
    return B


def split(design, case, who, need, p, then, tso):
    """A split's rounds on a lead that needs `need` blocks: claims kept across
    rounds; a pool found empty makes the round busy in the first round and
    while a block is on its way back, and fails the split otherwise. A's
    failure is checked LEGIT; C's needs no check, nothing ordering it."""
    blocks = len(CASES_STATE[case])

    def legit(M, L):
        if who != A:
            return None
        if case == 'one':
            # A's teams of more than one PE need the one free block (P7).
            return 'SPARE: A failed dry where its one team of more than one PE needed one block, ' \
                   'and one was free'
        for b in range(blocks):
            seen = L[f'o{b}']
            if seen not in (A, C, L_OWNER, T_OUTSIDE):
                return f'LEGIT: A failed dry, having found block {b} {OWNERS[seen]}'
        return None

    B = [(p, ('set', 'round', lambda L: 0, '@' + p + 'R')),
         (p + 'R', ('br', lambda L: (L['have'] or 0) >= need, '@' + p + 'Y', '@' + p + 'c'))]
    B += claim(design, who, p + 'c', '@' + p + 'G', '@' + p + 'D')
    B += [(p + 'G', ('set', 'have', lambda L: (L['have'] or 0) + 1, '@' + p + 'R')),
          # Dry: busy in the first round, or while a block is on its way back.
          (p + 'D', ('br', lambda L: L['round'] == 0 and design != 'first_round_final',
                     '@' + p + 'B', '@' + p + 'q'))]
    B += returning(design, blocks, p + 'q', '@' + p + 'B', '@' + p + 'F')
    # Busy: the round's reduction - in parent, the one M1 and M2 arrive in
    # after leaving T - and the round again.
    if case in ARRIVALS:
        wait = lambda M, L, n=ARRIVALS[case]: M[ARRIVED] == n
    else:
        wait = lambda M, L: True
    # The lead's arrival in the round's reduction (osoe_team_split_and) is the
    # fence; the wait for the others' is the await.
    B += fence(p + 'B', '@' + p + 'Bw', tso)
    B += [(p + 'Bw', ('await', wait, '@' + p + 'b')),
          (p + 'b', ('gpost', lambda M, L: ({}, {'round': 1, 'w': DEL, 'c': DEL, 'ok': DEL,
                                                 'b': DEL, 'm': DEL, 'h': DEL, 'f': DEL,
                                                 'g': DEL, 'd': DEL, 'o0': DEL, 'o1': DEL}),
                      '@' + p + 'R')),
          (p + 'F', ('check', legit, '@' + p + 'X')),
          (p + 'X', ('gpost', lambda M, L: ({SPLIT_DONE: 1} if who == A else {}, {}), then))]
    if who == C:
        # C names the block it claimed for a live team: holders, the flag
        # cleared, then members. osoe_node_team_name (node.c) stores the
        # generation, the holders and the members with release, in that order:
        # buffered under TSO, and flushed in the order made. The flag is the
        # breakage `departing`'s, never built, stored as the library would.
        # The split's sync after it (osoe_team_sync) is the fence.
        B += [(p + 'Y', ('load', lambda L: GEN(L['c']), 'g', '@' + p + 'Yg')),
              (p + 'Yg', ('store', lambda L: GEN(L['c']), lambda L: L['g'] + 1, False, '@' + p + 'Yh')),
              (p + 'Yh', ('store', lambda L: HOLDERS(L['c']), lambda L: 1, False, '@' + p + 'Yd')),
              (p + 'Yd', ('store', lambda L: DEPARTING(L['c']), lambda L: 0, False, '@' + p + 'Ym')),
              (p + 'Ym', ('store', lambda L: MEMBERS(L['c']), lambda L: 1, False, '@' + p + 'Yf'))]
        B += fence(p + 'Yf', then, tso)
    else:
        B += fence(p + 'Y', then, tso)
    return B


L_OWNER = L


def finish(p):
    return [(p, ('ghost', lambda M, L: ({DONE: M[DONE] + 1},
                                       {k: DEL for k in ('round', 'have', 'w', 'c', 'ok', 'b', 'm',
                                                         'h', 'f', 'g', 'd', 'o0', 'o1',
                                                         'left')}), '@END')),
            ('END', ('done',))]


def case(design, which, tso=False):
    state = CASES_STATE[which]
    mem = {FREE: 0, DONE: 0, ARRIVED: 0, T_SYNC: 0, SPLIT_DONE: 0}
    for b, (gen, holders, members, departing, free, owner) in enumerate(state):
        mem[GEN(b)], mem[HOLDERS(b)], mem[MEMBERS(b)] = gen, holders, members
        mem[DEPARTING(b)] = departing
        mem[OWNER(b)] = owner
        if free:
            mem[FREE] |= 1 << b
    if which == 'member':
        lead = assemble(depart(design, 0, 'd', '@l') + leave(design, 0, 'l', '@s')
                        + split(design, which, A, 1, 's', '@E', tso) + finish('E'))
        member = assemble(depart(design, 0, 'd', '@l') + leave(design, 0, 'l', '@E')
                          + finish('E'))
        other = assemble(split(design, which, C, 1, 's', '@E', tso) + finish('E'))
        threads = [lead, member, other]
    elif which == 'parent':
        # A leaver's arrival in the split's reduction: a locked add (osoe_epoch_bump_eq).
        arrive = [('a', ('add', lambda L: ARRIVED, lambda L: 1, '_', '@E'))]
        leaver = assemble(depart(design, 0, 'd', '@l') + leave(design, 0, 'l', '@a') + arrive
                          + finish('E'))
        lead = assemble(split(design, which, A, 1, 's', '@E', tso) + finish('E'))
        threads = [lead, leaver, leaver]
    elif which == 'remote':
        # M, T's one member here, not in the parent: it marks the block and
        # arrives in T's sync, whose completion lets X - T's member and a
        # parent PE elsewhere - return from its destroy and arrive in the
        # split's reduction, which the add stands for; then M leaves,
        # whenever it runs again. Arrival in a sync is a locked add
        # (osoe_epoch_bump_eq), which drains M's buffer under TSO.
        sync = [('a', ('add', lambda L: ARRIVED, lambda L: 1, '_', '@l'))]
        member = assemble(depart(design, 0, 'd', '@a') + sync + leave(design, 0, 'l', '@E')
                          + finish('E'))
        lead = assemble(split(design, which, A, 1, 's', '@E', tso) + finish('E'))
        threads = [lead, member]
    elif which == 'split_first':
        # T = {X, M}, M here and not in the parent, X a parent PE elsewhere
        # that splits first and destroys T after: M arrives in T's sync and
        # waits for X there; X is in the split's reduction (ARRIVED, at once)
        # and arrives in T's sync once the split has returned; then M leaves.
        member = assemble(depart(design, 0, 'd', '@a') + [
            ('a', ('add', lambda L: T_SYNC, lambda L: 1, '_', '@w')),
            ('w', ('await', lambda M, L: M[T_SYNC] == 2, '@n')),
            ('n', ('gpost', lambda M, L: ({OWNER(0): T}, {}), '@l'))]
            + leave(design, 0, 'l', '@E') + finish('E'))
        other = assemble([
            ('x', ('add', lambda L: ARRIVED, lambda L: 1, '_', '@y')),
            ('y', ('await', lambda M, L: M[SPLIT_DONE] == 1, '@z')),
            ('z', ('add', lambda L: T_SYNC, lambda L: 1, '_', '@E'))] + finish('E'))
        lead = assemble(split(design, which, A, 1, 's', '@E', tso) + finish('E'))
        threads = [lead, member, other]
    elif which == 'one':
        # The blocks A claims: one for its team of more than one PE, and none for
        # its team of one (team.c, claims_block) - or, broken, one for each.
        need = 2 if design == 'singleton_claims' else 1
        threads = [assemble(split(design, which, A, need, 's', '@E', tso) + finish('E'))]
    else:
        threads = [assemble(split(design, which, A, 2, 's', '@E', tso) + finish('E'))]
    return f'pool_dry/{which}/{design}', threads, mem


# (case, design, expect): True, or the property it must fail. Each runs under SC and then
# under x86-TSO, with the same verdict.
CASES = [
    ('member', 'design', True),
    ('parent', 'design', True),
    ('remote', 'design', True),
    ('split_first', 'design', True),
    ('two', 'design', True),
    ('two', 'first', 'TRAPPED'),
    ('member', 'odd_only', 'LEGIT'),
    ('member', 'zero_at_leave', 'LEGIT'),
    ('parent', 'first_round_final', 'LEGIT'),
    ('split_first', 'departing', 'TRAPPED'),
    ('one', 'design', True),
    ('one', 'singleton_claims', 'SPARE'),
]


def main():
    want = select(sys.argv)
    ok = True
    for which, design, expect in CASES:
        if not want(case(design, which)[0]):
            continue
        for tso in (False, True):
            name, threads, mem = case(design, which, tso)
            res = explore(name, threads, mem, tso=tso, max_seconds=1800,
                          liveness_max_states=6_000_000,
                          done=lambda M, n=len(threads): M[DONE] == n,
                          symmetric=[[1, 2]] if which == 'parent' else ())
            ok &= report(res, expect=expect, tso=tso)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
