#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""A team's node block: claimed from the pool, held, given back (step D; proposal §3, D3-D5).

A team's state on a node is a block of the node's pool, found by the team's
*name* on that node - (local id of its local lead, number) - through the map,
which holds block index + 1 under the name, 0 for none. The split's local lead
claims a free block from the pool's bitmap by compare-and-swap, writes it - a
raised generation, the name, the count of the team's local members as its
holders, the counters, the record - and publishes the map entry with release,
all before the split's sync. Members find the block through the map after the
sync and keep it. At destroy, after the sync over the team, each member leaves:
the team's local lead first clears the map entry under its name - the name is
its own, and nothing resolves it once any member here has left the team: no
message for the team arrives at the node after that - and then every member
brings holders down by one, and the one that brings it to zero retracts the
record, raises the generation and returns the block, in that order. So the
lead's name is free as soon as the lead has left, whoever holds the block
still, and the lead takes the number again at once (D4, as built: the first
design had the last holder clear the entry, and a lead whose members were slow
to leave found its own name taken).

The progress engine keys its state by block index. An arrival handoff carries
the generation beside the epoch, and a message from another node carries the
name, which resolves through the map; the engine learns a block's team afresh -
record and generation - whenever a handoff or a resolved block shows a
generation other than the one it learnt (D4, D5). A message for a team can be
in flight only while the team lives: the destroy's sync completes only after
every message for the team has arrived (proposal §3, "A late message"), which
the model takes as given.

Checked
  REWRITE  no block is written for a team while a member of another still holds
           it.
  OWN      every member's view of the block's counters is its own team's,
           straggler reads after the destroy's sync included.
  ENGINE   the engine never applies an arrival or a message of one generation
           to state learnt for another.
  NAMED    a message for a live team resolves, through the map, to that team's
           block.
  FREE     the lead finds its name unmapped once it has left its team, so that
           its number is in its free mask at its next split.
  FOUND    the member finds A's block mapped under A's name once the split's
           sync has returned.
  and, by the engine: no stuck thread, and from every reachable state the run can
  still finish (AG EF done).

Threads (one node; a pool of two blocks)
  lead     local id 0: creates team A under name 0 with the member, posts an
           arrival, destroys A and leaves; then creates team B under the same
           name, alone on this node, and posts an arrival.
  member   local id 1: A's other member - finds A's block after the split's sync,
           reads the counters, reads them once more after the destroy's sync (a
           straggler), and leaves last or first.
  other    local id 2: creates team C under name 1, alone on this node, whenever
           a block is free, and posts an arrival.
  remote   another node's lead: one message for A while it lives, then one for C
           and one for B once each lives.
  engine   this node's progress thread: takes posted arrivals and messages in any
           order.

Designs
  design           as above.
Breakages
  no_holders       no holders count: the lead gives the block back as it leaves,
                   whoever still holds it (REWRITE, OWN).
  no_relearn       the engine keeps the team it first learnt under a block index
                   (ENGINE).
  map_early        every member clears the map entry as it leaves, not the lead
                   alone: the lead takes the name again, and the slow member then
                   clears the new team's entry (NAMED).
  last_clears      the first design: the last holder clears the entry, with the
                   rest of the give-back, and the lead none - a lead whose member
                   is slow to leave finds its own name mapped (FREE).
  free_first       the last holder returns the block before raising its
                   generation: another team claims and writes it in between, and
                   the raise lands on that team's block (NAMED, OWN).

Every step is a locked instruction or a single-writer store whose order the
model states; runs are SC. The map entry's release and the members' acquire are
the record's argument in node.h, which the sync orders; the model keeps the
order and leaves the fences to the sanitizers.

Usage: team_block_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

BLOCKS = 2
FREE = ('free',)                      # bit b set: block b is free
MAP = lambda n: ('map', n)            # name -> block index + 1, 0 for none
GEN = lambda b: ('gen', b)            # the block's generation
HOLDERS = lambda b: ('holders', b)
RECORD = lambda b: ('record', b)      # 1 while a team's record is published
COUNTERS = lambda b: ('counters', b)  # whose counters: the generation that wrote them
HANDOFF = lambda b: ('handoff', b)    # generation * 16 + posted epoch
SYNC = lambda k: ('$sync', k)         # an abstract sync over A's two members
HOLDER = lambda b, pe: ('$holder', b, pe)
LIVE = lambda n: ('$live', n)         # the team live under name n: 0, or A=1, B=2, C=3
TEAM_GEN = lambda t: ('$team_gen', t)
TEAM_BLOCK = lambda t: ('$team_block', t)
MESSAGE = ('$message',)               # a message in flight: name * 8 + team, or 0
A_TAKEN = ('$a_taken',)               # A's arrival and A's message, taken by the engine
DONE = ('$done',)

A, B, C = 1, 2, 3
TEAM = {A: 'A', B: 'B', C: 'C'}


def lowest(w):
    for b in range(BLOCKS):
        if w & (1 << b):
            return b
    return None


def claim(p, then):
    """Take a free block by compare-and-swap over the bitmap; wait while none is
    free (a dry pool fails the split in the library; here it only waits)."""
    return [(p + 'c', ('await', lambda M, L: M[FREE] != 0, '@' + p + 'l')),
            (p + 'l', ('load', lambda L: FREE, 'w', '@' + p + 'z')),
            (p + 'z', ('br', lambda L: L['w'] == 0, '@' + p + 'c', '@' + p + 'p')),
            (p + 'p', ('set', 'b', lambda L: lowest(L['w']), '@' + p + 'x')),
            (p + 'x', ('cas', lambda L: FREE, lambda L: L['w'], lambda L: L['w'] & ~(1 << L['b']),
                       'ok', '@' + p + 'k')),
            (p + 'k', ('br', lambda L: L['ok'], then, '@' + p + 'l'))]


def write_block(p, name, team, members, then):
    """The split's local lead writes its block: no holder of another team may
    remain (REWRITE); then the generation, the counters, the holders, the record,
    and last the map entry - with release, before the sync."""
    def check(M, L):
        held = [pe for pe in range(3) if M[HOLDER(L['b'], pe)]]
        return None if not held else \
            f'REWRITE: block {L["b"]} written for team {TEAM[team]} while PE {held[0]} holds it'
    return [(p + 'h', ('check', check, '@' + p + 'g')),
            (p + 'g', ('load', lambda L: GEN(L['b']), 'g', '@' + p + 'G')),
            (p + 'G', ('store', lambda L: GEN(L['b']), lambda L: L['g'] + 1, True, '@' + p + 'C')),
            (p + 'C', ('store', lambda L: COUNTERS(L['b']), lambda L: L['g'] + 1, True,
                       '@' + p + 'H')),
            (p + 'H', ('store', lambda L: HOLDERS(L['b']), lambda L, k=len(members): k, True,
                       '@' + p + 'Hg')),
            (p + 'Hg', ('gpost', lambda M, L, ms=members: ({HOLDER(L['b'], pe): 1 for pe in ms}, {}),
                        '@' + p + 'R')),
            (p + 'R', ('store', lambda L: RECORD(L['b']), lambda L: 1, True, '@' + p + 'M')),
            (p + 'M', ('store', lambda L: MAP(name), lambda L: L['b'] + 1, True, '@' + p + 'Mg')),
            (p + 'Mg', ('gpost', lambda M, L, n=name, t=team:
                        ({LIVE(n): t, TEAM_GEN(t): L['g'] + 1, TEAM_BLOCK(t): L['b']},
                         {'g': L['g'] + 1}), then))]


def post(p, then):
    """The member that arrives last posts the team's arrival: the generation it
    holds beside epoch 1, a single-writer store."""
    return [(p + 'a', ('store', lambda L: HANDOFF(L['b']), lambda L: L['g'] * 16 + 1, True, then))]


def read_counters(p, team, then):
    return [(p + 'r', ('load', lambda L: COUNTERS(L['b']), 'c', '@' + p + 'rc')),
            (p + 'rc', ('check', lambda M, L, t=team: None if L['c'] == L['g'] else
                        f'OWN: a member of team {TEAM[t]} read block {L["b"]}\'s counters of '
                        f'generation {L["c"]}, its own being {L["g"]}', then))]


def leave(design, p, pe, name, then):
    """Forget the team: the lead (local id 0) clears the map entry under its name
    first; then holders down by one, and the last holder retracts the record,
    raises the generation and returns the block."""
    give_back = [(p + 'R', ('store', lambda L: RECORD(L['b']), lambda L: 0, True, '@' + p + 'G')),
                 (p + 'G', ('add', lambda L: GEN(L['b']), lambda L: 1, '_', '@' + p + 'F')),
                 (p + 'F', ('or', lambda L: FREE, lambda L: 1 << L['b'], '_', then))]
    unheld = lambda M, L: ({HOLDER(L['b'], pe): 0}, {})
    if design == 'last_clears':
        # The first design: the map entry is the last holder's to clear, with the
        # rest of the give-back.
        give_back = [(p + 'R', ('store', lambda L: RECORD(L['b']), lambda L: 0, True,
                                '@' + p + 'G')),
                     (p + 'G', ('add', lambda L: GEN(L['b']), lambda L: 1, '_', '@' + p + 'M')),
                     (p + 'M', ('store', lambda L: MAP(name), lambda L: 0, True, '@' + p + 'F')),
                     (p + 'F', ('or', lambda L: FREE, lambda L: 1 << L['b'], '_', then))]
        return [(p + 'q', ('ghost', unheld, '@' + p + 'd')),
                (p + 'd', ('add', lambda L: HOLDERS(L['b']), lambda L: -1, 'h', '@' + p + 'z')),
                (p + 'z', ('br', lambda L: L['h'] == 0, '@' + p + 'R', then))] + give_back
    # The lead's first step: its name's entry cleared, before its decrement - so
    # before the last holder's, and before the bit.
    clear = [(p + 'e', ('store', lambda L: MAP(name), lambda L: 0, True, '@' + p + 'd'))] \
        if pe == 0 or design == 'map_early' else []
    first = '@' + p + 'e' if clear else '@' + p + 'd'
    if design == 'free_first':
        # The block back in the pool before its generation is raised: a claimer
        # can write it in between, and the raise lands on the new team's.
        give_back = [(p + 'F', ('or', lambda L: FREE, lambda L: 1 << L['b'], '_', '@' + p + 'R')),
                     (p + 'R', ('store', lambda L: RECORD(L['b']), lambda L: 0, True, '@' + p + 'G')),
                     (p + 'G', ('add', lambda L: GEN(L['b']), lambda L: 1, '_', then))]
        return [(p + 'q', ('ghost', unheld, first))] + clear + [
                (p + 'd', ('add', lambda L: HOLDERS(L['b']), lambda L: -1, 'h', '@' + p + 'z')),
                (p + 'z', ('br', lambda L: L['h'] == 0, '@' + p + 'F', then))] + give_back
    if design == 'no_holders':
        # No count: the lead gives the block back as it leaves.
        if pe == 0:
            return [(p + 'q', ('ghost', unheld, '@' + p + 'e'))] + \
                [(p + 'e', ('store', lambda L: MAP(name), lambda L: 0, True, '@' + p + 'R'))] + \
                give_back
        return [(p + 'q', ('ghost', unheld, then))]
    return [(p + 'q', ('ghost', unheld, first))] + clear + [
            (p + 'd', ('add', lambda L: HOLDERS(L['b']), lambda L: -1, 'h', '@' + p + 'z')),
            (p + 'z', ('br', lambda L: L['h'] == 0, '@' + p + 'R', then))] + give_back


def sync(p, k, then, destroy=False):
    """An abstract sync over A's two members. The destroy's completes only once
    the engine has taken A's arrival and A's message: its release is the last of
    what the team's tree carries (proposal §3, "A late message")."""
    cond = (lambda M, L: M[SYNC(k)] == 2 and M[A_TAKEN] == 2) if destroy else \
           (lambda M, L: M[SYNC(k)] == 2)
    return [(p + 's', ('ghost', lambda M, L: ({SYNC(k): M[SYNC(k)] + 1}, {}), '@' + p + 'w')),
            (p + 'w', ('await', cond, then))]


def finish(locals_):
    return [('E', ('gpost', lambda M, L: ({DONE: M[DONE] + 1}, {k: DEL for k in locals_}), '@END')),
            ('END', ('done',))]


def lead(design):
    B_ = []
    B_ += claim('a', '@ah')
    B_ += write_block('a', 0, A, (0, 1), '@as')
    B_ += sync('a', 1, '@aa')
    B_ += post('a', '@ar')
    B_ += read_counters('a', A, '@ds')
    B_ += sync('d', 2, '@dl', destroy=True)
    B_ += [('dl', ('ghost', lambda M, L: ({LIVE(0): 0}, {}), '@lq'))]
    B_ += leave(design, 'l', 0, 0, '@bm')
    # Team B under the same name: the free mask leaves the number out while a
    # block is still mapped under (lead, number), and the lead finds it free as
    # soon as it has left A, whoever holds A's block still (FREE).
    B_ += [('bm', ('check', lambda M, L: None if M[MAP(0)] == 0 else
                   'FREE: the lead left team A and found its name still mapped, its number not '
                   'free for team B', '@bc'))]
    B_ += claim('b', '@bh')
    B_ += write_block('b', 0, B, (0,), '@ba')
    B_ += post('b', '@br')
    B_ += read_counters('b', B, '@E')
    B_ += finish(('w', 'ok', 'c', 'h', 'b', 'g'))
    return assemble(B_)


def member(design):
    B_ = []
    B_ += sync('a', 1, '@f')
    B_ += [('f', ('load', lambda L: MAP(0), 'v', '@fz')),
           ('fz', ('check', lambda M, L: None if L['v'] else
                   'FOUND: a member of team A found no block', '@fb')),
           ('fb', ('set', 'b', lambda L: L['v'] - 1, '@fg')),
           ('fg', ('load', lambda L: GEN(L['b']), 'g', '@fr'))]
    B_ += read_counters('f', A, '@ds')
    B_ += sync('d', 2, '@sr', destroy=True)
    B_ += read_counters('s', A, '@lq')                  # the straggler's read
    B_ += leave(design, 'l', 1, 0, '@E')
    B_ += finish(('v', 'c', 'h', 'b', 'g'))
    return assemble(B_)


def other(design):
    B_ = []
    B_ += claim('c', '@ch')
    B_ += write_block('c', 1, C, (2,), '@ca')
    B_ += post('c', '@cr')
    B_ += read_counters('c', C, '@E')
    B_ += finish(('w', 'ok', 'c', 'b', 'g'))
    return assemble(B_)


def remote():
    """Another node: one message for A while it lives; then for C and for B, each
    once it lives. One message in flight at a time."""
    def send(name, team):
        return lambda M, L: ({MESSAGE: name * 8 + team}, {})
    return assemble([
        ('1', ('await', lambda M, L: M[LIVE(0)] == A and M[MESSAGE] == 0, '@1s')),
        ('1s', ('ghost', send(0, A), '@2')),
        ('2', ('await', lambda M, L: M[LIVE(1)] == C and M[MESSAGE] == 0, '@2s')),
        ('2s', ('ghost', send(1, C), '@3')),
        ('3', ('await', lambda M, L: M[LIVE(0)] == B and M[MESSAGE] == 0, '@3s')),
        ('3s', ('ghost', send(0, B), '@E')),
        ('E', ('ghost', lambda M, L: ({DONE: M[DONE] + 1}, {}), '@END')),
        ('END', ('done',))])


def engine(design):
    """Takes posted arrivals and messages, in any order, until it has taken the
    three arrivals and the three messages. Its state for a block is the
    generation it learnt the block's team at."""
    def events(M, L):
        alts = []
        for b in range(BLOCKS):
            h = M[HANDOFF(b)]
            if h and h != (L[f'taken{b}'] or 0):
                alts.append(({}, {'kind': 1, 'b': b, 'eg': h // 16, 'h': h}, f'arrival on block {b}'))
        if M[MESSAGE]:
            alts.append(({}, {'kind': 2, 'm': M[MESSAGE]}, 'a message'))
        return alts

    def learn(M, L):
        # The record and the generation, afresh, when the event shows another
        # generation than the one learnt - or, broken, only the first time.
        b, g = L['b'], L['eg']
        known = L[f'learnt{b}']
        if known != g and (design != 'no_relearn' or not known):
            return {}, {f'learnt{b}': g}
        return {}, {}

    def apply(M, L):
        b, g = L['b'], L['eg']
        if L[f'learnt{b}'] != g:
            return (f'ENGINE: the engine applied '
                    f'{"an arrival" if L["kind"] == 1 else "a message"} of generation {g} to '
                    f'block {b}\'s state, learnt at generation {L[f"learnt{b}"]}')
        return None

    def own(M, L):
        name, team = divmod(L['m'], 8)
        if L['b'] != M[TEAM_BLOCK(team)] or L['eg'] != M[TEAM_GEN(team)]:
            return (f'NAMED: a message for team {TEAM[team]} under name {name} resolved to block '
                    f'{L["b"]} at generation {L["eg"]}, the team\'s being block '
                    f'{M[TEAM_BLOCK(team)]} at {M[TEAM_GEN(team)]}')
        return None

    def is_a(M, L):
        return L['b'] == M[TEAM_BLOCK(A)] and L['eg'] == M[TEAM_GEN(A)] and M[LIVE(0)] == A

    return assemble([
        ('L', ('br', lambda L: (L['n'] or 0) >= 6, '@E', '@P')),
        ('P', ('nd', events, '@K')),
        ('K', ('br', lambda L: L['kind'] == 1, '@HL', '@Q')),
        # An arrival: the handoff's own generation.
        ('HL', ('ghost', learn, '@HA')),
        ('HA', ('check', apply, '@HT')),
        ('HT', ('ghost', lambda M, L: ({A_TAKEN: M[A_TAKEN] + is_a(M, L)},
                                       {f'taken{L["b"]}': L['h'], 'n': (L['n'] or 0) + 1}), '@X')),
        # A message: its name through the map, then the block's generation.
        ('Q', ('load', lambda L: MAP(L['m'] // 8), 'v', '@Qz')),
        ('Qz', ('check', lambda M, L: None if L['v'] else
                f'NAMED: a message for team {TEAM[L["m"] % 8]} under name {L["m"] // 8} found no '
                f'block in the map', '@Qb')),
        ('Qb', ('set', 'b', lambda L: L['v'] - 1, '@Qg')),
        ('Qg', ('load', lambda L: GEN(L['b']), 'eg', '@Qo')),
        ('Qo', ('check', own, '@QL')),
        ('QL', ('ghost', learn, '@QA')),
        ('QA', ('check', apply, '@QT')),
        ('QT', ('ghost', lambda M, L: ({MESSAGE: 0, A_TAKEN: M[A_TAKEN] + (L['m'] % 8 == A)},
                                       {'n': (L['n'] or 0) + 1, 'v': DEL, 'm': DEL}), '@X')),
        ('X', ('set', 'kind', lambda L: DEL, '@X2')),
        ('X2', ('set', 'eg', lambda L: DEL, '@X3')),
        ('X3', ('set', 'h', lambda L: DEL, '@X4')),
        ('X4', ('set', 'b', lambda L: DEL, '@L')),
        ('E', ('ghost', lambda M, L: ({DONE: M[DONE] + 1}, {}), '@END')),
        ('END', ('done',))])


def case(design):
    threads = [lead(design), member(design), other(design), remote(), engine(design)]
    mem = {FREE: (1 << BLOCKS) - 1, MESSAGE: 0, A_TAKEN: 0, DONE: 0, SYNC(1): 0, SYNC(2): 0}
    for b in range(BLOCKS):
        mem[GEN(b)] = mem[HOLDERS(b)] = mem[RECORD(b)] = mem[COUNTERS(b)] = mem[HANDOFF(b)] = 0
        for pe in range(3):
            mem[HOLDER(b, pe)] = 0
    for n in (0, 1):
        mem[MAP(n)] = mem[LIVE(n)] = 0
    for t in (A, B, C):
        mem[TEAM_GEN(t)] = 0
        mem[TEAM_BLOCK(t)] = 0
    return f'team_block/{design} blocks={BLOCKS}', threads, mem


CASES = [
    ('design', True),
    ('no_holders', 'REWRITE'),
    ('no_relearn', 'ENGINE'),
    ('map_early', 'NAMED'),
    ('last_clears', 'FREE'),
    ('free_first', 'NAMED'),
]


def main():
    want = select(sys.argv)
    ok = True
    for design, expect in CASES:
        name, threads, mem = case(design)
        if not want(name):
            continue
        res = explore(name, threads, mem, max_seconds=1800,
                      done=lambda M: M[DONE] == 5)
        ok &= report(res, expect=expect)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
