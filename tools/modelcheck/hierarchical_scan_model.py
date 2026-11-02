#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""A hierarchical prefix scan: a scan within each node, a scan of the nodes'
totals among the leads, each member's result from its node's prefix (the
large-cluster playbook's C9, `docs/design/hierarchical-scan-proposal.md`;
modelled 2026-11-02, not built).

As built (design §5.8, "Prefix scans", as built; internode-generic.c,
osoe_internode_generic_scan; scan_model.py), a scan on a team spanning nodes
runs over a pre-order tree of the team's ranks, every member a place in it,
fan-out 4, and takes no epoch: about 2 x ceil(log_4 M) hops for M members,
many of them network hops whatever the layout. The proposal, where every
node's members are one run of the team's ranks (team.c's
osoe_team_node_runs, a block distribution): the members arrive as a
reduction's do; the lead reads every member's source and sums them, the
node's total, and sends it up a pre-order tree over the runs in team-rank
order (collect's leads' scan, osoe_internode_scan_node_totals, over the runs
instead of the node set's positions); the down brings the totals of the runs
before this one, `before`; the lead writes every member's dest, the run's
prefix in rank order from `before` - as the one-node scan writes them today -
and releases. The exact sum is the spec's; a floating sum rounds in another
order than today's tree, which §5.8 already allows.

Threads: N nodes of L members each, member 0 of a node its lead; the leads'
tree at fan-out 2, so that three nodes give a root with two children and four
an interior node with a child of its own. A source is a distinct bit per
(epoch, rank), so a sum is a set and a result is checked exactly.

Properties
  SOURCE  the lead reads a member's source only after that member arrived at
          this epoch: it holds this epoch's value (invariant 17, push never
          pull; in place, the member's dest of the last epoch is its source).
  SLOT    a slot among the leads is written only once its last value was read.
  VALUE   a value a lead takes from a slot is this scan's.
  RESULT  every member reads, after the release, the exact prefix the spec
          defines for its team rank, inclusive or exclusive.
  and no trapped state (the engine's AG EF done), with STUCK at dead ends.

Designs
  hierarchical   the proposal, above.
Breakages, each named for the property it must fail
  lead_before_arrived   the lead reads the sources after its own arrival alone.
                        -> SOURCE
  release_before_prefix the lead releases its members before writing their
                        dests. -> RESULT
  member_before_release a member reads its dest without waiting for the
                        release. -> RESULT
  no_left_sums          child j's down lacks the totals of children 0 .. j - 1.
                        -> RESULT (scan_model.py's breakage of the same name,
                        over the leads' tree)
  down_before_up        the root sends child j its down as soon as it holds
                        the totals before it, without waiting for child j's
                        own total: a root a whole scan ahead writes a down
                        slot its child has not read. -> SLOT (the rule as
                        built: a parent writes a child's down only after the
                        child's total arrived, so the child is in this scan)
  position_order        the leads' tree over the node set's positions, and
                        `before` taken as the base in rank order, under a
                        layout whose runs meet the positions out of order.
                        -> RESULT (team.c's node_runs_order: the tree must be
                        over the runs in rank order, or the base corrected)

Layouts: `order` lists the nodes in team-rank order - run j is node order[j],
holding ranks j L .. j L + L - 1: in order, reversed (a negative stride), or a
permutation (a block map numbered out of order). The tree is over the run
indices; only `position_order` puts it over the node ids.

The waits are epoch.c's wait_eq with a sweep and the leads' counter waits,
whose futex protocols other models cover, so they are `await` here. Under
x86-TSO the sources' stores, the dests' and the slots' are plain (buffered),
each drained by the locked add that follows it - the arrival's, a counter's,
the release's - as the library's are (§3.16).

Usage: hierarchical_scan_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

SRC = lambda n, m: ('src', n, m)
DST = lambda n, m: ('dest', n, m)
ARRIVED = lambda n: ('arrived', n)
RELEASED = lambda n: ('released', n)
UP = lambda n, j: ('up', n, j)          # at node n's lead: child position j's subtree total
UPC = lambda n, j: ('upc', n, j)        # cumulative: totals put into position j
DOWN = lambda n: ('down', n)            # at node n's lead: the totals of the runs before its subtree
DOWNC = lambda n: ('downc', n)
TAG = lambda key: ('$tag',) + key       # ghost: the epoch of the value in the slot
UNREAD = lambda key: ('$unread',) + key  # ghost: the slot holds a value not yet read


def children(a, b, k):
    """The children of the place covering [a, b): (first, end) of each."""
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


def places(count, k):
    """index -> (parent index or None, position among its parent's children, [child index])"""
    info = {}

    def walk(a, b, parent, position):
        kids = children(a, b, k)
        info[a] = (parent, position, [c for c, _ in kids])
        for j, (c, e) in enumerate(kids):
            walk(c, e, a, j)
    walk(0, count, None, None)
    return info


def source(e, rank, total):
    return 1 << ((e - 1) * total + rank)


def expected(e, rank, total, inclusive):
    end = rank + 1 if inclusive else rank
    return sum(source(e, r, total) for r in range(end))


def dest_of(inplace, n, m):
    return SRC(n, m) if inplace else DST(n, m)


def lead(design, n, order, members, epochs, inplace, inclusive, k):
    """Node n's lead: member 0 of its run."""
    nodes = len(order)
    total = nodes * members
    run = order.index(n)
    # The tree: over the runs in rank order, or (position_order) over the node ids.
    tree_index = n if design == 'position_order' else run
    parent_index, position, kid_indexes = places(nodes, k)[tree_index]
    node_of_index = (lambda q: q) if design == 'position_order' else (lambda q: order[q])
    parent_node = None if parent_index is None else node_of_index(parent_index)
    kids = [node_of_index(q) for q in kid_indexes]
    rank0 = run * members
    B = []
    for e in range(1, epochs + 1):
        pre = f'n{n}e{e}_'
        nxt = f'@n{n}e{e + 1}_W' if e < epochs else '@END'

        def step(label, ins):
            B.append((pre + label, ins))

        def goto(label, target):
            B.append((pre + label, ('br', lambda L: True, '@' + pre + target, '@' + pre + target)))

        # My own source: the application's plain store, drained by the arrival.
        step('W', ('store', lambda L: SRC(n, 0), lambda L, e=e: source(e, rank0, total), False,
                   '@' + pre + 'A'))
        step('A', ('add', lambda L: ARRIVED(n), lambda L: 1, '_', '@' + pre + 'WA'))
        if design == 'lead_before_arrived':
            goto('WA', 'P0')
        else:
            step('WA', ('await', lambda M, L, e=e: M[ARRIVED(n)] == e * members, '@' + pre + 'P0'))
        # Pass 1: every member's source, the node's total `t` (a set of bits).
        step('P0', ('set', 't', lambda L: 0, '@' + pre + 'P1_0'))
        for m in range(members):
            after = f'P1_{m + 1}' if m + 1 < members else 'U0'
            step(f'P1_{m}', ('load', lambda L, m=m: SRC(n, m), 'v', '@' + pre + f'P2_{m}'))
            step(f'P2_{m}', ('check',
                             lambda M, L, e=e, m=m: None if L['v'] == source(e, rank0 + m, total)
                             else f'SOURCE: the lead of node {n} read member {m}\'s source at '
                                  f'epoch {e} and found {L["v"]:#x}, not {source(e, rank0 + m, total):#x}',
                             '@' + pre + f'P3_{m}'))
            step(f'P3_{m}', ('set', 't', lambda L: L['t'] | L['v'], '@' + pre + after))

        # Up: each child's subtree total, in position order (c0, c1, ...).
        def take_up(j, after):
            step(f'U{j}', ('await', lambda M, L, j=j, e=e: M[UPC(n, j)] >= e, '@' + pre + f'V{j}'))
            step(f'V{j}', ('check',
                           lambda M, L, j=j, e=e: None if M[TAG(UP(n, j))] == e
                           else f'VALUE: the lead of node {n} took up-slot {j} at epoch {e} holding '
                                f'epoch {M[TAG(UP(n, j))]}\'s total',
                           '@' + pre + f'L{j}'))
            step(f'L{j}', ('load', lambda L, j=j: UP(n, j), f'c{j}', '@' + pre + f'T{j}'))
            step(f'T{j}', ('ghost', lambda M, L, j=j: ({UNREAD(UP(n, j)): 0}, {}),
                           '@' + pre + after))

        # Down to child j: before + own total + the totals of children 0 .. j - 1.
        def send_down(j, after):
            cn = kids[j]

            def value(L, j=j):
                v = L['b'] | L['t']
                if design != 'no_left_sums':
                    for i in range(j):
                        v |= L[f'c{i}']
                return v
            step(f'D{j}', ('check',
                           lambda M, L, cn=cn, e=e: None if M.get(UNREAD(DOWN(cn)), 0) == 0
                           else f'SLOT: the lead of node {n} wrote node {cn}\'s down slot at epoch '
                                f'{e} while it held epoch {M[TAG(DOWN(cn))]}\'s, unread',
                           '@' + pre + f'E{j}'))
            step(f'E{j}', ('gpre', lambda M, L, cn=cn, e=e: ({TAG(DOWN(cn)): e, UNREAD(DOWN(cn)): 1},
                                                             {}),
                           '@' + pre + f'F{j}'))
            step(f'F{j}', ('store', lambda L, cn=cn: DOWN(cn), value, False, '@' + pre + f'G{j}'))
            step(f'G{j}', ('add', lambda L, cn=cn: DOWNC(cn), lambda L: 1, '_', '@' + pre + after))

        root = parent_node is None
        if root and design == 'down_before_up':
            # The breakage: child j's down as soon as the totals before it are
            # in, then child j's own total.
            step('U0', ('set', 'b', lambda L: 0, '@' + pre + ('D0' if kids else 'Q0')))
            for j in range(len(kids)):
                send_down(j, f'U{j}x')
                take_up_after = f'D{j + 1}' if j + 1 < len(kids) else 'Q0'
                # take_up labels U{j}, V{j}, L{j}, T{j}: rename its entry.
                step(f'U{j}x', ('await', lambda M, L, j=j, e=e: M[UPC(n, j)] >= e,
                                '@' + pre + f'V{j}'))
                step(f'V{j}', ('check',
                               lambda M, L, j=j, e=e: None if M[TAG(UP(n, j))] == e
                               else f'VALUE: the lead of node {n} took up-slot {j} at epoch {e} '
                                    f'holding epoch {M[TAG(UP(n, j))]}\'s total',
                               '@' + pre + f'L{j}'))
                step(f'L{j}', ('load', lambda L, j=j: UP(n, j), f'c{j}', '@' + pre + f'T{j}'))
                step(f'T{j}', ('ghost', lambda M, L, j=j: ({UNREAD(UP(n, j)): 0}, {}),
                               '@' + pre + take_up_after))
        else:
            for j in range(len(kids)):
                take_up(j, f'U{j + 1}' if j + 1 < len(kids) else 'S')
            if not kids:
                goto('U0', 'S')
            if root:
                step('S', ('set', 'b', lambda L: 0, '@' + pre + ('D0' if kids else 'Q0')))
            else:
                # My subtree's total up, then my `before` down.
                def subtree(L):
                    v = L['t']
                    for i in range(len(kids)):
                        v |= L[f'c{i}']
                    return v
                step('S', ('check',
                           lambda M, L, e=e: None if M.get(UNREAD(UP(parent_node, position)), 0) == 0
                           else f'SLOT: the lead of node {n} wrote its parent\'s up-slot {position} at '
                                f'epoch {e} while it held epoch '
                                f'{M[TAG(UP(parent_node, position))]}\'s total, unread',
                           '@' + pre + 'S1'))
                step('S1', ('gpre', lambda M, L, e=e: ({TAG(UP(parent_node, position)): e,
                                                        UNREAD(UP(parent_node, position)): 1}, {}),
                            '@' + pre + 'S2'))
                step('S2', ('store', lambda L: UP(parent_node, position), subtree, False,
                            '@' + pre + 'S3'))
                step('S3', ('add', lambda L: UPC(parent_node, position), lambda L: 1, '_',
                            '@' + pre + 'S4'))
                step('S4', ('await', lambda M, L, e=e: M[DOWNC(n)] >= e, '@' + pre + 'S5'))
                step('S5', ('check',
                            lambda M, L, e=e: None if M[TAG(DOWN(n))] == e
                            else f'VALUE: the lead of node {n} took its down at epoch {e} holding '
                                 f'epoch {M[TAG(DOWN(n))]}\'s',
                            '@' + pre + 'S6'))
                step('S6', ('load', lambda L: DOWN(n), 'b', '@' + pre + 'S7'))
                step('S7', ('ghost', lambda M, L: ({UNREAD(DOWN(n)): 0}, {}),
                            '@' + pre + ('D0' if kids else 'Q0')))
            for j in range(len(kids)):
                send_down(j, f'D{j + 1}' if j + 1 < len(kids) else 'Q0')

        # Pass 2: every member's dest in rank order from `before` (position_order
        # takes `before` as it comes); then the release. The breakage releases
        # first.
        if design == 'release_before_prefix':
            step('Q0', ('add', lambda L: RELEASED(n), lambda L: 1, '_', '@' + pre + 'Q1'))
            step('Q1', ('set', 'r', lambda L: L['b'], '@' + pre + 'X0_0'))
        else:
            step('Q0', ('set', 'r', lambda L: L['b'], '@' + pre + 'X0_0'))
        for m in range(members):
            after = f'X0_{m + 1}' if m + 1 < members else 'R'
            # Read the member's source for this chunk (the one-node scan reads
            # it before writing its dest, so the two may be one array).
            step(f'X0_{m}', ('load', lambda L, m=m: SRC(n, m), 'v', '@' + pre + f'X1_{m}'))
            step(f'X1_{m}', ('check',
                             lambda M, L, e=e, m=m: None if L['v'] == source(e, rank0 + m, total)
                             else f'SOURCE: the lead of node {n} read member {m}\'s source at '
                                  f'epoch {e} and found {L["v"]:#x}, not {source(e, rank0 + m, total):#x}',
                             '@' + pre + f'X2_{m}'))
            if inclusive:
                step(f'X2_{m}', ('set', 'r', lambda L: L['r'] | L['v'], '@' + pre + f'X3_{m}'))
                step(f'X3_{m}', ('store', lambda L, m=m: dest_of(inplace, n, m), lambda L: L['r'],
                                 False, '@' + pre + after))
            else:
                step(f'X2_{m}', ('store', lambda L, m=m: dest_of(inplace, n, m), lambda L: L['r'],
                                 False, '@' + pre + f'X3_{m}'))
                step(f'X3_{m}', ('set', 'r', lambda L: L['r'] | L['v'], '@' + pre + after))
        if design == 'release_before_prefix':
            goto('R', 'Y0')
        else:
            step('R', ('add', lambda L: RELEASED(n), lambda L: 1, '_', '@' + pre + 'Y0'))
        # My own result, as the application reads it after the call.
        step('Y0', ('load', lambda L: dest_of(inplace, n, 0), 'v', '@' + pre + 'Y1'))
        step('Y1', ('check',
                    lambda M, L, e=e: None if L['v'] == expected(e, rank0, total, inclusive)
                    else f'RESULT: the lead of node {n}, rank {rank0}, read {L["v"]:#x} at epoch {e}, '
                         f'not {expected(e, rank0, total, inclusive):#x}',
                    '@' + pre + 'Z'))
        step('Z', ('set', 'v', lambda L: DEL, '@' + pre + 'Z1'))
        step('Z1', ('set', 'r', lambda L: DEL, '@' + pre + 'Z2'))
        step('Z2', ('set', 'b', lambda L: DEL, '@' + pre + 'Z3'))
        step('Z3', ('set', 't', lambda L: DEL, '@' + pre + 'Z4'))
        for i in range(len(kids)):
            step(f'Z{4 + i}', ('set', f'c{i}', lambda L: DEL, '@' + pre + f'Z{5 + i}'))
        step(f'Z{4 + len(kids)}', ('set', '_', lambda L: DEL, nxt))
    B.append(('END', ('done',)))
    return assemble(B)


def member(design, n, m, order, members, epochs, inplace, inclusive):
    total = len(order) * members
    rank = order.index(n) * members + m
    B = []
    for e in range(1, epochs + 1):
        pre = f'n{n}m{m}e{e}_'
        nxt = f'@n{n}m{m}e{e + 1}_W' if e < epochs else '@END'
        B.append((pre + 'W', ('store', lambda L: SRC(n, m), lambda L, e=e: source(e, rank, total),
                              False, '@' + pre + 'A')))
        B.append((pre + 'A', ('add', lambda L: ARRIVED(n), lambda L: 1, '_', '@' + pre + 'WR')))
        if design == 'member_before_release':
            B.append((pre + 'WR', ('br', lambda L: True, '@' + pre + 'R', '@' + pre + 'R')))
        else:
            B.append((pre + 'WR', ('await', lambda M, L, e=e: M[RELEASED(n)] == e, '@' + pre + 'R')))
        B.append((pre + 'R', ('load', lambda L: dest_of(inplace, n, m), 'v', '@' + pre + 'C')))
        B.append((pre + 'C', ('check',
                              lambda M, L, e=e: None if L['v'] == expected(e, rank, total, inclusive)
                              else f'RESULT: member {m} of node {n}, rank {rank}, read {L["v"]:#x} '
                                   f'at epoch {e}, not {expected(e, rank, total, inclusive):#x}',
                              '@' + pre + 'Z')))
        B.append((pre + 'Z', ('set', 'v', lambda L: DEL, nxt)))
    B.append(('END', ('done',)))
    return assemble(B)


LAYOUTS = {
    'order': lambda nodes: list(range(nodes)),
    'reversed': lambda nodes: list(range(nodes - 1, -1, -1)),
    'permuted': lambda nodes: [1, 2, 0] if nodes == 3 else [2, 0, 3, 1] if nodes == 4 else [1, 0],
}
K = 2


def case(design, nodes, members, epochs, inplace, inclusive, layout, tso):
    order = LAYOUTS[layout](nodes)
    threads = []
    for n in range(nodes):
        threads.append(lead(design, n, order, members, epochs, inplace, inclusive, K))
        for m in range(1, members):
            threads.append(member(design, n, m, order, members, epochs, inplace, inclusive))
    mem = {}
    for n in range(nodes):
        mem[ARRIVED(n)] = mem[RELEASED(n)] = mem[DOWN(n)] = mem[DOWNC(n)] = 0
        for j in range(K):
            mem[UP(n, j)] = mem[UPC(n, j)] = 0
        for m in range(members):
            mem[SRC(n, m)] = 0
            if not inplace:
                mem[DST(n, m)] = 0
    name = (f'hierarchical_scan/{design} nodes={nodes} members={members} epochs={epochs} '
            f'{"inplace" if inplace else "separate"} {"inclusive" if inclusive else "exclusive"} '
            f'{layout}')
    return name, threads, mem


# (design, nodes, members, epochs, inplace, inclusive, layout, expect): True, or
# the property the case must fail.
CASES = [
    ('hierarchical', 2, 2, 2, False, False, 'order', True),
    ('hierarchical', 2, 2, 2, True, True, 'order', True),
    ('hierarchical', 2, 3, 2, True, False, 'reversed', True),
    ('hierarchical', 3, 2, 2, False, True, 'order', True),
    ('hierarchical', 3, 2, 1, True, False, 'permuted', True),
    ('hierarchical', 3, 2, 1, True, True, 'reversed', True),
    ('hierarchical', 4, 2, 1, False, False, 'order', True),
    ('hierarchical', 4, 1, 2, True, True, 'permuted', True),
    ('lead_before_arrived', 2, 2, 1, False, False, 'order', 'SOURCE'),
    ('lead_before_arrived', 2, 2, 1, True, True, 'order', 'SOURCE'),
    ('release_before_prefix', 2, 2, 1, False, False, 'order', 'RESULT'),
    ('member_before_release', 2, 2, 1, False, True, 'order', 'RESULT'),
    ('no_left_sums', 3, 1, 1, False, False, 'order', 'RESULT'),
    ('down_before_up', 2, 1, 2, False, False, 'order', 'SLOT'),
    ('position_order', 3, 2, 1, False, False, 'permuted', 'RESULT'),
    ('position_order', 2, 2, 1, False, True, 'reversed', 'RESULT'),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, nodes, members, epochs, inplace, inclusive, layout, expect in CASES:
        for tso in (False, True):
            name, threads, mem0 = case(design, nodes, members, epochs, inplace, inclusive, layout, tso)
            if not keep(name):
                continue
            res = explore(name, threads, mem0, tso=tso, max_seconds=900,
                          liveness_max_states=2_000_000)
            ok &= report(res, expect=expect, tso=tso)
    sys.exit(0 if ok else 1)
