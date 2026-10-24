#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Pipelined pieces through a prefix scan's tree (spec §9.10.10; design §5.8,
"Prefix scans"; the author's decision of 2026-09-27, "pipeline it").

scan_model.py checks the scan as first built: every member at its team rank's
place in a pre-order k-ary tree over the ranks, and a scan larger than a slot
in pieces, each piece all the way up and down before the next. This model
checks it with D pieces in flight, pipeline_model.py's rules carried over to
the scan's tree:
  * A member keeps D up-slots per child position and D down-slots, and piece g
    goes in slot g mod D, g numbering the team's pieces over its life (every
    member of a team moves the same pieces, so every member numbers alike).
  * One counter per child position and one for downs, cumulative over the
    team's life, compared as at least.
  * A member sends piece q up only once it has consumed piece q - D's down:
    its parent read piece q - D's total before sending that down, so the slot
    is free.
  * A member copies a down out before it does anything that lets its parent
    write that slot again - the next piece it sends up.
  * Whichever is ready: its children's totals for piece `up`, while the window
    allows, or piece `down`'s down. The root's down is its own, zero, but it
    takes a piece down only once it has taken it up - and so does every
    member: a piece's down to child j carries the totals of children
    0 .. j - 1, read when the piece went up.
  * A member keeps its own source and its children's totals for the pieces in
    flight, D of each, until each piece has gone down.

Checked
  SLOT     no slot is written while it holds a value not yet read.
  VALUE    a value read from a slot is the piece waited for: its (scan, piece).
  RESULT   every member's exclusive result for every piece is the exact sum of
           the sources of the ranks before it: sources are distinct powers of
           two per (scan, piece, rank), so a sum is a set.
  and, by the engine: no stuck member, and every state can still finish.

Teams: A is the members in world order, B the same members reversed, so the
two trees differ; each has its own slots and counters, as built (scan_model.py,
per_team). A case's order is the scans every member makes, "AB" one on A then
one on B.

**Lanes** (the team redesign's step E, E6; docs/design/team-resources-proposal.md
§4): a large scan's slots move from the team number to the member's *lane*,
one a thread, shared by the teams it scans (the counters stay the team's), and
a member entering a scan *grants* each of its tree children: it adds one to the
child's grant counter for the team, and the child sends its first piece up only
once that counter shows this scan's grant, counted as everything else is. A
down needs no grant: the parent writes a child's down-slot only after the
child's piece went up, inside this scan. lane_model.py has the lane's index
travelling with the grant, and the child's lane word read with its first piece;
here each member has one thread and one lane. A **small** scan (E6, E3; a
lowercase letter in a case's order) is one piece through the team number's own
small slots, a slot a position, with no grant and no lane: safe because a member
reaches its team's next scan only once it holds this one's down, which its
parent sends only after reading its children's totals.

Designs
  design              as above.
Breakages
  window              a member sends piece q once it holds piece q - D - 1's
                      down: one piece too many in flight (SLOT).
  count_before_copy   a member counts a down consumed when it arrives, and
                      copies it out only after whatever that count lets
                      through - its next piece up, after which its parent may
                      write the slot again (SLOT or VALUE).
  root_early          the root takes a piece down before taking it up, so the
                      downs it sends lack their children's totals (RESULT).
  ring_short          a member keeps D - 1 pieces' totals, not D: a piece's
                      totals are overwritten before it goes down (RESULT).
  summed              one up counter per member, summed over positions: a fast
                      child's next piece stands in for a slow child's (VALUE).
  lanes               the slots a member's lane, with the grant: a design.
  lanes_no_grant      the slots a member's lane, and a child that puts without
                      the grant: into a parent still in another team's scan
                      (SLOT or VALUE).
  lanes_stale_grant   a child that takes any grant it ever had from the team
                      for this scan's (SLOT or VALUE).
  lanes_node_small    small scans through slots a member's teams share, not each
                      team number's own (SLOT or VALUE).

Memory orders (T29; round six's portability review, whose note was that the slots
were written in `nd` steps, which the engine never buffers). Every case runs
under SC and under x86-TSO. A slot's bytes are a **put** into another member's
internal region, which its agent (or the helper that owns the wire) applies as
plain stores: a model store with `serial=False`, which the checker buffers under
TSO and flushes in the order made. The ghosts that go with a slot (what it held,
which scan and piece wrote it) are noted atomically with the store, as the `nd`
steps did, so the SC runs are state for state the ones they were. The add each
put is fenced before (OSOE_TRANSPORT_FENCED, transport.h, invariant 5) is
osoe_rmw_apply's (rmw.h): a seq_cst fetch_add, locked, which drains the buffer;
and the reader loads the counter (osoe_transport_load, seq_cst) before the slot.
Waits are abstract (the region wait's wake is wait_word_model.py's). The puts,
all osoe_internode_generic_scan's (internode-generic.c):
  UP, UPC       a piece's subtree total up, into the parent's lane slot (or the team
                number's small slot) for the member's position, then
                scan_counter_of_number's fenced add
  DOWN, DOWNC   the sum before a child's subtree down, into its slot, then the
                child's fenced add
  GRANT         a member entering a large scan: the grant's words put into each
                child, then scan_grant_counter's fenced add (the words are not
                modelled: one lane a member)
Not modelled: a lane word before a child's first piece, and the reader's clears.

Usage: scan_pipeline_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

UP = lambda p, t, j, s: ('up', p, t, j, s)   # at PE p, team t: child position j's slot s
UPC = lambda p, t, j: ('upc', p, t, j)       # its counter, cumulative
DOWN = lambda p, t, s: ('down', p, t, s)     # at PE p, team t: down-slot s
DOWNC = lambda p, t: ('downc', p, t)
GRANT = lambda p, t: ('grant', p, t)         # at PE p, team t: its tree parents' grants, cumulative
LANE = 'lane'                                # the lanes designs' key, in place of the team's
SHARED_SMALL = 'small'                       # lanes_node_small's key for every team's small slots
UNREAD = lambda key: ('$unread',) + key      # ghost: the slot holds a value not yet read
TAG = lambda key: ('$tag',) + key            # ghost: (scan, piece) of the value in the slot
OVERWRITE = ('$overwrite',)
FIN = ('$fin',)

TEAMS = {'A': lambda m: list(range(m)), 'B': lambda m: list(reversed(range(m))),
         # Two teams of two that share PE 1, the root of both: PE 0 is P's child
         # and PE m - 1 Q's, each at position 0 - a child outside the scan its
         # parent is in, which is where the lanes' grant matters.
         'P': lambda m: [1, 0], 'Q': lambda m: [1, m - 1]}
TEAM_NUMBER = {'A': 0, 'B': 1, 'P': 2, 'Q': 3}


def children(a, b, k):
    """The children of the place covering [a, b): (first rank, end) of each."""
    rest = b - (a + 1)
    if rest <= 0:
        return []
    n = min(k, rest)
    out, start = [], a + 1
    for j in range(n):
        size = rest // n + (1 if j < rest % n else 0)
        out.append((start, start + size))
        start += size
    return out


def places(members, k):
    """rank -> (parent rank or None, position among its parent's children,
    [child ranks])"""
    info = {}

    def walk(a, b, parent, position):
        kids = children(a, b, k)
        info[a] = (parent, position, [c for c, _ in kids])
        for j, (c, e) in enumerate(kids):
            walk(c, e, a, j)
    walk(0, members, None, None)
    return info


def source(x, q, rank, members, pieces):
    return 1 << ((x * pieces + q) * members + rank)


def tag(x, q):
    return 1000 * (x + 1) + q + 1


def member(design, pe, members, k, depth, pieces, order):
    """PE `pe`'s program: the scans of `order`, one after another, each its
    loop over the pieces."""
    window = depth + (1 if design == 'window' else 0)
    ring = depth - 1 if design == 'ring_short' else depth
    B = []
    moved = {}                  # team -> pieces its scans moved before this one
    granted = {}                # team -> grants this PE had for its scans before this one
    # A lowercase letter is a small scan on the team: one piece.
    mine = [(x, entry.upper(), entry.islower()) for x, entry in enumerate(order)
            if pe in TEAMS[entry.upper()](members)]
    for at, (x, team, small) in enumerate(mine):
        pes = TEAMS[team](members)
        count = 1 if small else pieces
        base = moved.get(team, 0)
        moved[team] = base + count
        grants = granted.get(team, 0)
        if not small and places(len(pes), k)[pes.index(pe)][0] is not None:
            granted[team] = grants + 1
        after = f'@x{mine[at + 1][0]}_E' if at + 1 < len(mine) else '@DONE'
        # Each scan's program in a call of its own, so that every function and
        # lambda below closes over this scan's names, not the last scan's.
        B += one_scan(design, pe, members, k, depth, count, window, ring, x, team, base, after,
                      grants, small)
    B += [('DONE', ('gpost', lambda M, L: ({FIN: M[FIN] + 1}, {}), '@END')),
          ('END', ('done',))]
    if not mine:
        B = [('START', ('br', lambda L: True, '@DONE', '@DONE'))] + B
    return assemble(B)


def one_scan(design, pe, members, k, depth, pieces, window, ring, x, team, base, after, grants,
             small):
    """Scan x of PE `pe`'s program: its loop over the pieces, entered at
    x<x>_E and leaving to `after`."""
    B = []
    pes = TEAMS[team](members)
    t = TEAM_NUMBER[team]
    rank = pes.index(pe)
    parent, position, kids = places(len(pes), k)[rank]
    ppe = None if parent is None else pes[parent]
    kpes = [pes[c] for c in kids]
    p = f'x{x}_'
    lanes = design.startswith('lanes')
    # Whose slots: a large scan's the lane's, or the team number's before the
    # lanes; a small one's the team number's own small slots, a slot a
    # position - or, under lanes_node_small, one set every team shares.
    if small:
        s_t = (SHARED_SMALL if design == 'lanes_node_small' else ('small', t))
    else:
        s_t = LANE if lanes else t

    def slot(q, base=base):
        return 0 if small else (base + q) % depth

    def local(name, q):
        return f'{name}{q % ring}'

    def arrived(M, q, base=base, kids=kids, t=t):
        if design == 'summed':
            return sum(M[UPC(pe, t, j)] for j in range(k)) >= len(kids) * (base + q + 1)
        return all(M[UPC(pe, t, j)] >= base + q + 1 for j in range(len(kids)))

    # Entry: nothing in flight, the ring's locals cleared.
    entry = [('set', 'up', lambda L: 0), ('set', 'down', lambda L: 0),
             ('set', 'pend', lambda L: 0), ('set', 'put', lambda L: DEL)]
    for q in range(ring):
        entry.append(('set', f'own{q}', lambda L: 0))
        for j in range(len(kids)):
            entry.append(('set', f'c{j}_{q}', lambda L: 0))
    for i, (op, var, fn) in enumerate(entry):
        here = p + ('E' if i == 0 else f'E{i}')
        nxt = '@' + p + (f'E{i + 1}' if i + 1 < len(entry) else 'G0')
        B.append((here, (op, var, fn, nxt)))
    # Entering: under the lanes designs, a grant to each tree child.
    G = []
    for j, kpe in enumerate(kpes if lanes and not small else []):
        G.append((p + f'G{j}', ('add', lambda L, kpe=kpe, t=t: GRANT(kpe, t), lambda L: 1, '_',
                                '@' + p + (f'G{j + 1}' if j + 1 < len(kpes) else 'LOOP'))))
    if not G:
        G.append((p + 'G0', ('br', lambda L: True, '@' + p + 'LOOP', '@' + p + 'LOOP')))
    B += G
    B.append((p + 'LOOP', ('br', lambda L: L['down'] >= pieces and not L['pend'],
                           after, '@' + p + 'CH')))

    def holds_grant(M, root=parent is None, t=t):
        if root or not lanes or small or design == 'lanes_no_grant':
            return True
        if design == 'lanes_stale_grant':
            return M[GRANT(pe, t)] >= 1
        return M[GRANT(pe, t)] >= grants + 1

    def choose(M, L, base=base, root=parent is None, t=t):
        alts = []
        if L['up'] < pieces and L['up'] < L['down'] + window and arrived(M, L['up']) and \
                holds_grant(M):
            alts.append(({}, {'go': 1}, f'PE {pe}: piece {L["up"]} up'))
        if L['pend']:
            alts.append(({}, {'go': 3}, f'PE {pe}: copy out the down it counted'))
        elif L['down'] < pieces and (L['down'] < L['up'] or (root and design == 'root_early')) \
                and (root or M[DOWNC(pe, t)] >= base + L['down'] + 1):
            alts.append(({}, {'go': 2}, f'PE {pe}: piece {L["down"]} down'))
        return alts

    B += [(p + 'CH', ('nd', choose, '@' + p + 'GO')),
          (p + 'GO', ('br', lambda L: L['go'] == 1, '@' + p + 'U', '@' + p + 'GO2')),
          (p + 'GO2', ('br', lambda L: L['go'] == 2, '@' + p + 'D', '@' + p + 'L'))]

    # Up: each child's total for piece `up`, kept, then the subtree's up.
    up = []
    for j in range(len(kids)):
        up.append((f'U{j}', ('check', lambda M, L, j=j, t=s_t:
                             None if M[TAG(UP(pe, t, j, slot(L['up'])))] == tag(x, L['up']) else
                             f'VALUE: PE {pe} read up-slot {j} with tag '
                             f'{M[TAG(UP(pe, t, j, slot(L["up"])))]}, waiting for piece '
                             f'{L["up"]} of scan {x} ({tag(x, L["up"])})')))
        up.append((f'UT{j}', ('ghost', lambda M, L, j=j, t=s_t:
                              ({UNREAD(UP(pe, t, j, slot(L['up']))): 0},
                               {local(f'c{j}_', L['up']): M[UP(pe, t, j, slot(L['up']))]}))))
    up.append(('UO', ('ghost', lambda M, L, rank=rank:
                      ({}, {local('own', L['up']): source(x, L['up'], rank, members, pieces)}))))
    if ppe is not None:
        # The put into the parent's up-slot: a store of the bytes - a plain store the
        # parent's agent makes, buffered under TSO - and, atomic with it, the slot's
        # ghosts (it was written; whether it still held an unread value). The add
        # after it, fenced, drains the bytes (osoe_internode_generic_scan, internode-
        # generic.c: osoe_transport_put, then osoe_transport_rmw OSOE_TRANSPORT_FENCED).
        def up_key(L, t=s_t, ppe=ppe, position=position):
            return UP(ppe, t, position, slot(L['up']))

        def note_up(M, L, n=len(kids)):
            key = up_key(L)
            if M[UNREAD(key)]:
                # An overwrite, flagged: the old bytes stored again, so nothing moves.
                return {OVERWRITE: 1}, {'put': M[key]}
            total = L[local('own', L['up'])] + sum(L[local(f'c{j}_', L['up'])] for j in range(n))
            return {UNREAD(key): 1, TAG(key): tag(x, L['up'])}, {'put': total}
        up += [('UPN', ('gpre', note_up)),
               ('UP', ('store', up_key, lambda L: L['put'], False)),
               ('UPD', ('set', 'put', lambda L: DEL)),
               ('UPO', ('check', lambda M, L: None if not M[OVERWRITE] else
                        f'SLOT: PE {pe} overwrote an unread up-slot')),
               ('UPA', ('add', lambda L, t=t, ppe=ppe, position=position: UPC(ppe, t, position),
                        lambda L: 1, '_'))]
    up.append(('UN', ('set', 'up', lambda L: L['up'] + 1)))

    # Down: the sum before this subtree, checked, then one to each child.
    def down_steps(prefix, var):
        D = []
        if ppe is not None:
            D += [(f'{prefix}V', ('check', lambda M, L, t=s_t:
                                  None if M[TAG(DOWN(pe, t, slot(L[var])))] == tag(x, L[var]) else
                                  f'VALUE: PE {pe} read its down-slot with tag '
                                  f'{M[TAG(DOWN(pe, t, slot(L[var])))]}, waiting for piece '
                                  f'{L[var]} of scan {x} ({tag(x, L[var])})')),
                  (f'{prefix}T', ('ghost', lambda M, L, t=s_t:
                                  ({UNREAD(DOWN(pe, t, slot(L[var]))): 0},
                                   {'before': M[DOWN(pe, t, slot(L[var]))]})))]
        else:
            D.append((f'{prefix}T', ('set', 'before', lambda L: 0)))
        D.append((f'{prefix}X', ('check', lambda M, L, rank=rank:
                                 None if L['before'] == sum(source(x, L[var], r, members, pieces)
                                                            for r in range(rank)) else
                                 f'RESULT: PE {pe}\'s exclusive result for piece {L[var]} of '
                                 f'scan {x} is {L["before"]:b}')))
        for j, kpe in enumerate(kpes):
            # Likewise a put into a child's down-slot: the bytes stored, buffered
            # under TSO, the ghosts atomic with the store, the fenced add after.
            def down_key(L, kpe=kpe, t=s_t):
                return DOWN(kpe, t, slot(L[var]))

            def note_down(M, L, j=j, down_key=down_key):
                key = down_key(L)
                if M[UNREAD(key)]:
                    return {OVERWRITE: 1}, {'put': M[key]}
                value = L['before'] + L[local('own', L[var])] + \
                    sum(L[local(f'c{i}_', L[var])] for i in range(j))
                return {UNREAD(key): 1, TAG(key): tag(x, L[var])}, {'put': value}
            D += [(f'{prefix}QN{j}', ('gpre', note_down)),
                  (f'{prefix}Q{j}', ('store', down_key, lambda L: L['put'], False)),
                  (f'{prefix}QD{j}', ('set', 'put', lambda L: DEL)),
                  (f'{prefix}QO{j}', ('check', lambda M, L: None if not M[OVERWRITE] else
                                      f'SLOT: PE {pe} overwrote an unread down-slot')),
                  (f'{prefix}QA{j}', ('add', lambda L, kpe=kpe, t=t: DOWNC(kpe, t),
                                      lambda L: 1, '_'))]
        return D

    if design == 'count_before_copy':
        down = [('DC', ('set', 'pend', lambda L: 1)),
                ('DN', ('set', 'down', lambda L: L['down'] + 1))]
        late = [('LF', ('set', 'fw', lambda L: L['down'] - 1))] + down_steps('L', 'fw') + \
               [('LP', ('set', 'pend', lambda L: 0))]
    else:
        down = down_steps('D', 'down') + [('DN', ('set', 'down', lambda L: L['down'] + 1))]
        late = [('LX', ('br', lambda L: True, '@' + p + 'LOOP', '@' + p + 'LOOP'))]

    for seq, start in ((up, 'U'), (down, 'D'), (late, 'L')):
        for i, (lab, ins) in enumerate(seq):
            here = p + (start if i == 0 else lab) if i == 0 else p + lab
            nxt = '@' + p + seq[i + 1][0] if i + 1 < len(seq) else '@' + p + 'LOOP'
            if ins[0] == 'br':
                B.append((here, ins))
            else:
                B.append((here, ins + (nxt,)))
    return B


def case(design, members, k, depth, pieces, order):
    threads = [member(design, pe, members, k, depth, pieces, order) for pe in range(members)]
    mem = {FIN: 0, OVERWRITE: 0}
    for p in range(members):
        for t in TEAM_NUMBER.values():
            mem[GRANT(p, t)] = 0
        for t in list(TEAM_NUMBER.values()) + [LANE, SHARED_SMALL] + \
                [('small', n) for n in TEAM_NUMBER.values()]:
            mem[DOWNC(p, t)] = 0
            for s in range(depth):
                mem[DOWN(p, t, s)] = mem[UNREAD(DOWN(p, t, s))] = mem[TAG(DOWN(p, t, s))] = 0
            for j in range(k):
                mem[UPC(p, t, j)] = 0
                for s in range(depth):
                    key = UP(p, t, j, s)
                    mem[key] = mem[UNREAD(key)] = mem[TAG(key)] = 0
    name = (f'scan-pipeline/{design} members={members} k={k} depth={depth} pieces={pieces} '
            f'order={order}')
    return name, threads, mem, members


# (design, members, fan-out, depth, pieces, order, expect): True, or the property it must fail.
# Each runs under SC and then under x86-TSO, with the same verdict - but `summed`, which
# fails VALUE and RESULT under both orders and is named by the one the search meets
# first: VALUE under SC, RESULT under TSO, whose flush steps move the search order
# (T29, 2026-10-24).
CASES = [
    ('design', 3, 2, 2, 3, 'A', True),           # a root and two leaves
    ('design', 4, 1, 2, 3, 'A', True),           # a chain: members both parent and child
    ('design', 4, 1, 1, 3, 'A', True),           # depth 1 is the scan as first built
    ('design', 4, 2, 2, 3, 'AA', True),          # the ring carried from one scan to the next
    ('design', 4, 2, 2, 2, 'AB', True),          # two teams, two trees, two sets of slots
    ('design', 4, 1, 3, 4, 'A', True),           # deeper than the pieces' halfway
    ('design', 5, 2, 2, 3, 'A', True),           # a member with children below the root
    ('design', 3, 1, 2, 3, 'ABA', True),         # back to A: its slots where it left them
    ('window', 3, 1, 2, 4, 'A', 'SLOT'),
    ('count_before_copy', 3, 1, 2, 4, 'A', 'SLOT'),
    ('root_early', 3, 2, 2, 3, 'A', 'RESULT'),
    ('ring_short', 3, 1, 2, 4, 'A', 'RESULT'),
    ('summed', 3, 2, 2, 3, 'A', 'VALUE'),
    # Step E: the slots a member's lane, shared by its teams, and the grant.
    ('lanes', 4, 2, 2, 2, 'AB', True),
    ('lanes', 3, 1, 2, 3, 'ABA', True),
    ('lanes', 4, 2, 2, 3, 'AA', True),
    ('lanes', 3, 2, 2, 2, 'ABAB', True),
    ('lanes', 3, 1, 2, 2, 'PQ', True),
    ('lanes', 3, 1, 2, 2, 'PQP', True),
    # With every member in both teams, a scan's last piece down proves every
    # piece of it went up, so a member that has finished one scan writes into
    # no slot of it: without the grant A then B is still safe, as consecutive
    # scans on one team are...
    ('lanes_no_grant', 3, 1, 2, 2, 'AB', True),
    # ...and the grant matters where a child is outside the scan its parent is
    # in: PE 2, in Q alone, puts into PE 1's lane while PE 1 is in P's scan.
    ('lanes_no_grant', 3, 1, 2, 2, 'PQ', 'SLOT'),
    ('lanes_stale_grant', 3, 1, 2, 2, 'QPQ', 'SLOT'),
    # Small scans through each team number's own slots, between large ones
    # through the lanes: on one team, on two whose parent is shared, with a
    # child outside the scan its parent is in.
    ('lanes', 3, 2, 2, 2, 'aAa', True),
    ('lanes', 3, 1, 2, 2, 'pQqP', True),
    ('lanes', 3, 1, 2, 2, 'pqpq', True),
    ('lanes_node_small', 3, 1, 2, 2, 'pq', 'SLOT'),
]


def main():
    want = select(sys.argv)
    ok = True
    for design, members, k, depth, pieces, order, expect in CASES:
        name, threads, mem, n = case(design, members, k, depth, pieces, order)
        if not want(name):
            continue
        for tso in (False, True):
            res = explore(name, threads, mem, tso=tso, max_seconds=900,
                          liveness_max_states=3_000_000, done=lambda M, n=n: M[FIN] == n)
            met = 'RESULT' if tso and design == 'summed' else expect
            ok &= report(res, expect=met, tso=tso)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
