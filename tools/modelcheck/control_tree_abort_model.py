#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""A global exit's spread over a control tree, and the carrier's wait on it
(docs/design/control-tree-proposal.md; the large-cluster playbook's C17;
design §3.15 "One carrier for the job", P1).

As built, a node's lead holds a control wire to every other node, and an
abort crosses them as an abortive goodbye on each (abort_model.py,
abort_exit_model.py, carrier_model.py): the lead sends one on every wire that
has not carried the peer's own goodbye naming a caller ranked at or below its
own, at most one a wire and the wire's last segment; a node told declares the
same abort and relays, so a goodbye reaches every node by one hop or two. The
job's carrier is the lowest-ranked caller: a carrier's node judges itself
outranked by a goodbye it reads itself, naming a lower caller, and a carrier's
node not outranked waits for every other node's end of stream, so that the
carrier leaves last (§3.15, "The carrier leaves last"). Both lean on the mesh:
the goodbyes of two carriers' nodes reach each other directly, and the
carrier's node sees every node end.

The control tree keeps a lead's control wires to its parent and children in
the world team's tree alone, so a node hears of an abort through its
neighbours and sees only their ends. The proposal's rules, which this model
checks:

  * A node's word is the lowest caller it knows, with that caller's status;
    an abort *notice* carries the two. A node that learns a lower caller than
    its word's, from a neighbour or its own PE, takes it and tells every
    neighbour whose last notice to it named no caller as low - so a notice may
    follow another on one wire, where today's goodbye is the wire's last
    segment, and a node tells a neighbour back when that neighbour's notice
    named a higher caller (today's peer_outranked_here).
  * The *direction* of a node is the neighbour its word's notice came from,
    or itself where its own PE is the word's caller. A node ends - its
    process, which ends every stream it holds - once it has told every
    neighbour and every neighbour but the one in its direction has ended; the
    word's own node, whose direction is itself, waits for every neighbour. So
    the ends gather toward the lowest caller's node, which leaves last, and a
    node's exit status is its word's: the lowest caller's PE leaves with the
    status, every other with 0.

Each node here is its lead: its PEs' ends and the lead's bounded waits for
them are the interleaving. The streams are per directed edge, in order, as
TCP's are; an end of stream follows what was queued.

Checked
  LAST    when the lowest caller's node has ended, every node has ended.
  STATUS  a node whose PE called ends knowing the lowest caller: its PE
          leaves with the status exactly when it is that caller.
  STUCK   no terminal state has a node not ended (every wait here is
          unbounded; the C bounds them, OSOE_ABORT_LEAD_WAIT_SECONDS, and a
          job that runs them out is one these rules let wait for ever).
  LIVE    from every state, a state where every node has ended is reachable.

Cases: three nodes in a chain or a star, four in a tree of two levels; one
caller or two, on leaves, interior nodes and the root, the lower rank on
either; and a death - a node that ends every stream without a notice, which
its neighbours take for a fatal (a fatal relays no further here: the C's
fatal ends its process at once, and the neighbours' own ends tell the rest;
design §3.15, "receive_wire"). LAST and STATUS are not asked with a death: a
dead node's end looks to the carrier's node like a node done, and the nodes
beyond it, cut off, end by their own fatals after - the proposal accepts it,
the job being a failure already.

Designs
  design           the rules above.
Breakages
  one_notice       at most one notice a wire, as a goodbye is today: a lower
                   caller learnt after the wire's notice is never relayed on
                   it, and two carriers' nodes wait for each other (STUCK).
  first_wins       a node's word is its first notice's caller, as the abort
                   word's CAS has it today, so no lower caller is relayed
                   (STUCK; the carrier's node of the lower never hears the
                   higher's node end, which waits for it).
  no_tell_back     nothing to a neighbour whose own notice has arrived,
                   whatever it named - the rule before P1 (STUCK).
  no_far_wait      a node that is not the word's own waits for nobody, as a
                   non-carrier node does today: its end cuts the tree, and
                   the carrier's node, seeing it, leaves before the nodes
                   beyond (LAST).
  wait_all         every node waits for every neighbour's end, no direction
                   (STUCK: two neighbours wait for each other).
  as_built         one_notice, first_wins and no_far_wait together: today's
                   rules on a tree (LAST and STATUS: two PEs leave with a
                   status, and the carrier's node not last).

Usage: control_tree_abort_model.py [substring ...]
"""
import sys
from collections import deque

import engine

# Shapes: each node's neighbours.
SHAPES = {
    'chain3': {0: (1,), 1: (0, 2), 2: (1,)},
    'star3': {0: (1, 2), 1: (0,), 2: (0,)},
    'tree4': {0: (1, 2), 1: (0, 3), 2: (0,), 3: (1,)},
}

# (name, shape, callers {node: rank}, dead node or None, expect)
CASES = [
    ('leaf', 'chain3', {2: 7}, None, True),
    ('root', 'chain3', {0: 7}, None, True),
    ('middle', 'chain3', {1: 7}, None, True),
    ('ends_low_leaf', 'chain3', {0: 7, 2: 3}, None, True),
    ('ends_low_root', 'chain3', {0: 3, 2: 7}, None, True),
    ('middle_and_leaf', 'chain3', {1: 7, 2: 3}, None, True),
    ('star_leaves', 'star3', {1: 7, 2: 3}, None, True),
    ('star_root_leaf', 'star3', {0: 7, 1: 3}, None, True),
    ('tree_far_leaves', 'tree4', {2: 7, 3: 3}, None, True),
    ('tree_far_leaves_low_other', 'tree4', {2: 3, 3: 7}, None, True),
    ('tree_root_deep', 'tree4', {0: 7, 3: 3}, None, True),
    ('tree_three', 'tree4', {0: 9, 2: 7, 3: 3}, None, True),
    # Nodes 0 and 1 tell each other at once, 5 and 7, and the lowest is beyond 1.
    ('tree_crossing', 'tree4', {1: 7, 2: 5, 3: 3}, None, True),
    ('death_far', 'chain3', {0: 7}, 2, True),
    ('death_middle', 'chain3', {0: 7}, 1, True),
    ('death_beside', 'tree4', {3: 7}, 2, True),
]

BREAKAGES = [
    ('one_notice', 'tree_crossing', 'STUCK'),
    ('first_wins', 'ends_low_leaf', 'STUCK'),
    ('no_tell_back', 'ends_low_leaf', 'STUCK'),
    ('no_far_wait', 'tree_root_deep', 'LAST'),
    ('wait_all', 'leaf', 'STUCK'),
    ('as_built', 'ends_low_leaf', 'LAST'),
]

SELF = -1
NONE = 99          # a rank no caller has: "no word"


def run(design, shape, callers, dead):
    neigh = SHAPES[shape]
    nodes = sorted(neigh)
    one_notice = design in ('one_notice', 'as_built')
    first_wins = design in ('first_wins', 'as_built')
    no_far_wait = design in ('no_far_wait', 'as_built')
    no_tell_back = design == 'no_tell_back'
    wait_all = design == 'wait_all'
    lowest = min(callers.values())
    lowest_node = min(n for n, r in callers.items() if r == lowest)

    # node state: (phase, word, direction, sent, heard, ended, first)
    #   phase 0 running, 1 aborting, 2 ended, 3 dead
    #   word: the lowest rank known (NONE if none); direction: SELF or a neighbour
    #   sent[y]: the lowest rank told to neighbour y (NONE if none)
    #   heard[y]: the lowest rank neighbour y told this node (NONE if none)
    #   ended[y]: neighbour y's stream has ended
    #   first: the first rank the node took - what first_wins relays
    def fresh(n):
        k = len(neigh[n])
        return (0, NONE, SELF, (NONE,) * k, (NONE,) * k, (False,) * k, NONE)

    start_nodes = tuple(fresh(n) for n in nodes)
    # channels: per node, per neighbour index: items queued toward that neighbour
    start_chans = tuple(tuple(() for _ in neigh[n]) for n in nodes)
    start = (start_nodes, start_chans)

    def index(x, y):
        return neigh[x].index(y)

    def succ(S):
        ns, cs = S
        out = []

        def emit(x, nx, sends=(), label=''):
            """sends: [(y, item)] queued from x toward y."""
            nn = list(ns)
            nn[x] = nx
            nc = [list(c) for c in cs]
            for y, item in sends:
                nc[x][index(x, y)] = nc[x][index(x, y)] + (item,)
            out.append(((tuple(nn), tuple(tuple(c) for c in nc)), label))

        for x in nodes:
            phase, word, direction, sent, heard, ended, first = ns[x]
            if phase >= 2:
                continue
            # A declaration: this node's own PE calls shmem_global_exit.
            if phase == 0 and x in callers and dead != x:
                emit(x, (1, callers[x], SELF, sent, heard, ended, callers[x]),
                     label=f'{x} declares rank {callers[x]}')
            # A death: every stream ends without a word.
            if phase == 0 and dead == x:
                emit(x, (3, word, direction, sent, heard, ended, first),
                     sends=[(y, 'EOF') for y in neigh[x]], label=f'{x} dies')
            # Read the head of a neighbour's stream.
            for j, y in enumerate(neigh[x]):
                q = cs[y][index(y, x)]
                if not q:
                    continue
                item, rest = q[0], q[1:]
                nn = list(ns)
                nc = [list(c) for c in cs]
                nc[y][index(y, x)] = rest
                if item == 'EOF':
                    ne = list(ended)
                    ne[j] = True
                    np = phase
                    if phase == 0:
                        # A death seen: a fatal, status 1, no caller to rank.
                        np = 1
                    nn[x] = (np, word, direction, sent, heard, tuple(ne), first)
                    out.append(((tuple(nn), tuple(tuple(c) for c in nc)), f'{x} reads the end of {y}'))
                else:
                    rank = item[1]
                    nh = list(heard)
                    nh[j] = min(nh[j], rank)
                    nw, nd, nf = word, direction, first
                    if rank < word:
                        nw, nd = rank, j
                    if first == NONE:
                        nf = rank
                    np = 1 if phase == 0 else phase
                    nn[x] = (np, nw, nd, sent, tuple(nh), ended, nf)
                    out.append(((tuple(nn), tuple(tuple(c) for c in nc)), f'{x} reads notice {rank} from {y}'))
            if phase != 1:
                continue
            # A fatal - a word with no caller - ends at once, telling nobody,
            # as the C's fatal does (fatal.c: _exit on the thread that found it).
            if word == NONE:
                emit(x, (2, word, direction, sent, heard, ended, first),
                     sends=[(y, 'EOF') for y in neigh[x]], label=f'{x} ends on a fatal')
                continue
            tell = first if first_wins else word       # what this node's notices name
            # Tell a neighbour.
            for j, y in enumerate(neigh[x]):
                if ended[j] or sent[j] <= tell:
                    continue
                if one_notice and sent[j] != NONE:
                    continue
                if no_tell_back and heard[j] != NONE:
                    continue
                if heard[j] <= tell:
                    continue            # nobody to tell: the neighbour knows as low a caller
                nsent = list(sent)
                nsent[j] = tell
                emit(x, (1, word, direction, tuple(nsent), heard, ended, first),
                     sends=[(y, ('N', tell))], label=f'{x} tells {y} of rank {tell}')
            # End: told everyone, and every neighbour but the direction's has ended.
            told = all(ended[j] or sent[j] <= tell or heard[j] <= tell
                       or (one_notice and sent[j] != NONE)
                       for j in range(len(neigh[x])))
            if not told:
                continue
            if wait_all:
                waits = range(len(neigh[x]))
            elif no_far_wait and direction != SELF:
                waits = ()
            else:
                waits = [j for j in range(len(neigh[x])) if j != direction]
            if all(ended[j] for j in waits):
                emit(x, (2, word, direction, sent, heard, ended, first),
                     sends=[(y, 'EOF') for y in neigh[x]], label=f'{x} ends')
        return out

    seen = {start: None}
    edges = {}
    queue = deque([start])
    while queue:
        S = queue.popleft()
        ss = succ(S)
        edges[S] = [T for T, _ in ss]
        for T, label in ss:
            if T not in seen:
                seen[T] = (S, label)
                queue.append(T)

    def trace(S):
        tr = []
        while seen.get(S):
            S, label = seen[S]
            tr.append(label)
        return list(reversed(tr))

    def finished(n):
        return n[0] >= 2

    # A caller whose node's word was set before its call - by a neighbour's
    # notice - lost its declaration and leaves with 0, as today (§3.15: the
    # first declaration on a node fixes the status there): the carriers are
    # the callers whose declarations won, which a run decides.
    def winners(ns):
        return {n: callers[n] for n in callers if ns[n][6] == callers[n] and n != dead}

    for S in edges:
        ns = S[0]
        if dead is None:
            won = winners(ns)
            if won:
                low = min(won.values())
                low_node = min(n for n, r in won.items() if r == low)
                if ns[low_node][0] == 2:
                    for n in nodes:
                        if not finished(ns[n]):
                            return (False, f'LAST: node {low_node}, the lowest caller\'s, ended before node {n}',
                                    len(seen), trace(S))
            if not edges[S] and all(finished(ns[n]) for n in nodes):
                for n, r in won.items():
                    carries = ns[n][1] == r          # its PE leaves with the status
                    if carries != (r == low):
                        return (False, f'STATUS: node {n}\'s caller, rank {r}, leaves '
                                + ('with the status' if carries else 'with 0')
                                + f' where the lowest caller is rank {low}', len(seen), trace(S))
        if not edges[S] and not all(finished(ns[n]) for n in nodes):
            waiting = [n for n in nodes if not finished(ns[n])]
            return False, f'STUCK: nothing can happen and nodes {waiting} have not ended', len(seen), trace(S)
    goal = {S for S in edges if all(finished(n) for n in S[0])}
    rev = {}
    for S, ts in edges.items():
        for T in ts:
            rev.setdefault(T, []).append(S)
    good = set(goal)
    work = deque(goal)
    while work:
        T = work.popleft()
        for S in rev.get(T, ()):
            if S not in good:
                good.add(S)
                work.append(S)
    bad = [S for S in edges if S not in good]
    if bad:
        return False, 'LIVE: a state from which not every node can end', len(seen), trace(bad[0])
    return True, None, len(seen), []


def main():
    want = engine.select(sys.argv)
    by_name = {c[0]: c for c in CASES}
    runs = [('design', c[0], c[4]) for c in CASES] + BREAKAGES
    ok = True
    for design, case, expect in runs:
        name = f'control_tree_abort/{design} case={case}'
        if not want(name):
            continue
        _, shape, callers, dead, _ = by_name[case]
        held, failure, states, tr = run(design, shape, callers, dead)
        wrong = engine.judge(held, engine.named(failure), expect)
        mark = '' if wrong is None else '   <-- UNEXPECTED' + wrong
        print(f'{name}: {"holds" if held else "FAILS: " + failure} ({states} states){mark}', flush=True)
        if tr and wrong is not None:
            print('   trace: ' + ' ; '.join(tr))
        ok &= wrong is None
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
