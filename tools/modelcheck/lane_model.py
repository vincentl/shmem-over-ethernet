#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Lanes: the pipeline's slots a thread's own, lent to one collective at a time (step E).

docs/design/team-resources-proposal.md §4, E4, E5 and E9;
docs/prompts/done/phase1-step7-slice4-prompt.md, Part 1's first model. It is
pipeline_model.py with the slots moved:

  * a team number keeps its counters, and the 16 KiB slots are a **lane** of
    the thread that leads the collective on its node - each lead thread of a
    node has a lane of its own (E9), so two threads leading two teams'
    collectives at once on one node never meet in a slot;
  * a team's collectives may be led on a node by a different thread from one
    collective to the next (a program's threads take turns on a team: its
    collectives are not concurrent, spec §9.10, but any thread may call the
    next), so where a lane is cannot be computed - it travels: a parent
    entering a collective puts its lane's index into each child's grant line
    for the team and then adds one to the child's **grant** counter; a child
    sends its first piece up only once its grant counter shows this
    collective's grant, and puts into the lane that grant names; and a child
    writes its own lane's index beside its parent's arrival counter for its
    position before its first add, which the parent reads as it takes that
    child's first piece and keeps for the collective (E4, corrected: the
    proposal had the parent read it when it first sends that child a release,
    by which time a child of the team's next collective, in a tree turned to
    another root, may have written its own there - release_lane_late).

A **broadcast** needs no grant (E4): its pieces go up empty, so a child writes
nothing into its parent's lane, and its first arrival carries its own lane's
index all the same, for the releases that bring the data down. A **small**
collective (E3) is one piece through the team number's own small slot for the
position, and its release through the number's own small release slot: no
grant, no lane - safe for the reason one slot per position is safe at D = 1 in
pipeline_model.py, a child reaching the team's next collective only after
holding this one's release.

Everything else is the pipeline's: D slots a row, piece k in slot k mod D; an
arrival counter per child position and a release counter, per team; a node
sends piece k up only once it holds release k - D; a release is copied out and
forwarded before it is counted; whichever input is ready. Counters are
cumulative over the team's life and compared as at least.

Checked
  UP      the slot a parent reads holds the piece it waits for - this team's,
          this collective's.
  DOWN    the release slot a node reads holds the piece it waits for.
  LANE    every put lands in a lane bound, at that moment, to the collective
          that puts (a ghost binding, set as a thread enters and cleared as it
          returns).
  and, by the engine: no stuck thread, and from every state the run can finish.

Each thread runs the collectives it leads in the order given, and a team's
collectives on one node in that order whichever thread leads them (a ghost
turn per team and node: the program's own ordering of a team's calls across
its threads). A node outside a team does not wait for it: that is how a child
of one team comes to push while its parent is inside another's collective.

Designs
  design        as above.
Breakages
  no_grant      the first design's rule, the first model's breakage: a node's
                lead threads share one lane, found by arithmetic rather than
                told, and a child pushes without waiting for the grant - safe
                only while the slots are the team's own (UP, LANE); with every
                node in both teams and one order it is safe, as consecutive
                collectives on one team are.
  stale_grant   a child takes any grant it ever had from the team for this
                collective's (UP, LANE): the parent may be inside another
                team's, or another thread may lead the team now.
  lane_early    a child reads its parent's lane before its grant counter shows
                the grant: the lane of the team's last collective (LANE).
  release_lane_early
                a parent reads a child's lane as it enters, before the child's
                first arrival: the lane the child used last (LANE, DOWN).
  release_lane_late
                the proposal's rule: a parent reads a child's lane when it
                first sends that child a release. With no more pieces than the
                window the parent's last piece goes up before its first release
                comes down, the collective can end elsewhere, and a node that is
                the parent's child at that position in the team's next
                collective - a broadcast rooted elsewhere - writes its own lane
                there first (LANE).
  one_lane      a node's lead threads share one lane, the first design's slots
                a node's (UP, LANE): two threads leading two teams at once.
  node_small    a node's small slots shared by its teams, not a team number's
                own (UP): a child of one team puts its small piece while the
                parent reads another team's.
  node_small_down  only the small release slot shared by a node's teams (DOWN):
                holds with one thread a node, fails with two leading two teams
                at once - the one breakage that reaches the small path's DOWN
                check (round six's H3).

Runs are SC, as pipeline_model.py's: every add is fenced after its data.

Usage: lane_model.py [substring ...]
"""
import sys

from engine import DEL, assemble, explore, report, select

ARR = lambda t, n, c: ('arr', t, n, c)      # team t: node n's arrival counter, child position c
REL = lambda t, n: ('rel', t, n)            # team t: node n's release counter
GRANT = lambda t, n: ('grant', t, n)        # team t: the grants node n has had from its parents
GLANE = lambda t, n: ('glane', t, n)        # team t: the lane node n's last grant named (index + 1)
ALANE = lambda t, n, c: ('alane', t, n, c)  # team t: the lane node n's child at c named (index + 1)
SLOT = lambda n, h, c, j: ('slot', n, h, c, j)  # node n, lane h: child position c's row
SMALL = lambda t, n, c: ('small', t, n, c)  # team t: node n's small slot, child position c
SMALL_DOWN = lambda t, n: ('small_down', t, n)  # team t: node n's small release slot
RSLOT = lambda n, h, j: ('rslot', n, h, j)      # node n, lane h: the release row
BIND = lambda n, h: ('$bind', n, h)         # ghost: the collective lane h of node n is bound to
TURN = lambda t, n: ('$turn', t, n)         # ghost: team t's collectives node n has finished
FIN = ('$fin',)


def tree(members, root, fanout):
    """The k-ary tree over a team's node list `members`, rotated to index `root`:
    {node: (parent node or None, position, [child nodes])}."""
    count = len(members)
    out = {}
    for at, n in enumerate(members):
        i = (at - root) % count
        parent = None if i == 0 else members[((i - 1) // fanout + root) % count]
        position = 0 if i == 0 else (i - 1) % fanout
        kids = [members[(fanout * i + c + 1 + root) % count] for c in range(fanout)
                if fanout * i + c + 1 < count]
        out[n] = (parent, position, kids)
    return out


# The designs whose parent reads a child's lane as it takes the child's first
# piece; release_lane_early reads it at entry, release_lane_late at the first
# release, no_grant computes it.
READS_AT_ARRIVAL = ('design', 'stale_grant', 'lane_early', 'one_lane', 'node_small', 'node_small_down')


def lane_of(design, h):
    """The lane a node's thread h leads with: its own, or under one_lane and
    no_grant the node's one."""
    return 0 if design in ('one_lane', 'no_grant') else h


def thread(design, n, h, fanout, depth, collectives, pieces):
    """Node n's lead thread h, running the collectives it leads, in order."""
    window = depth
    lane = lane_of(design, h)
    B = []
    # Bases, by team, over every collective of the team node n is in - whichever
    # of the node's threads leads it - so that each sees the counters as the
    # team's earlier collectives on this node left them.
    base_arr, base_rel, base_grant, turns = {}, {}, {}, {}
    plan = []
    for x, (team, members, root, tagbase, leads, kind) in enumerate(collectives):
        if n not in members:
            continue
        t = tree(members, root, fanout)
        parent, position, kids = t[n]
        arr = base_arr.setdefault(team, [0] * fanout)
        rel = base_rel.setdefault(team, 0)
        grant = base_grant.setdefault(team, 0)
        turn = turns.setdefault(team, 0)
        count = 1 if kind == 's' else pieces
        if leads[n] == h:
            plan.append((x, team, parent, position, kids, [t[k][1] for k in kids],
                         list(arr), rel, grant, turn, tagbase, kind, count))
        for k in kids:
            arr[t[k][1]] += count
        if parent is not None:
            base_rel[team] = rel + count
            if kind == 'r':
                base_grant[team] = grant + 1
        turns[team] = turn + 1

    for at, (x, team, parent, position, kids, kid_pos, arr, rel, grant, turn,
             tagbase, kind, pieces) in enumerate(plan):
        small_team = '*' if design == 'node_small' else team
        small_down_team = '*' if design in ('node_small', 'node_small_down') else team
        p = f'n{n}h{h}c{x}_'
        after = f'@n{n}h{h}c{plan[at + 1][0]}_T' if at + 1 < len(plan) else '@DONE'
        me = x + 1

        # Entering: the team's earlier collectives on this node are over (the
        # program's order), the lane is this collective's, and every child is
        # told which lane - its index, then the grant's add.
        B += [(p + 'T', ('await', lambda M, L, team=team, turn=turn: M[TURN(team, n)] == turn,
                         '@' + p + 'B')),
              (p + 'B', ('ghost', lambda M, L, me=me: ({BIND(n, lane): me}, {}), '@' + p + 'L'))]
        if design == 'release_lane_early':
            # Each child's lane read now, before its first arrival says it.
            Lr = []
            for i, (k, c) in enumerate(zip(kids, kid_pos)):
                nxt = f'@{p}L{i + 1}' if i + 1 < len(kids) else '@' + p + 'E'
                Lr.append((p + 'L' if i == 0 else f'{p}L{i}',
                           ('load', lambda L, c=c, team=team: ALANE(team, n, c), f'k{c}', nxt)))
            B += Lr or [(p + 'L', ('br', lambda L: True, '@' + p + 'E', '@' + p + 'E'))]
        else:
            B += [(p + 'L', ('br', lambda L: True, '@' + p + 'E', '@' + p + 'E'))]
        E = []
        for i, k in enumerate(kids if kind == 'r' else []):
            here = p + 'E' if i == 0 else f'{p}E{i}'
            nxt = f'@{p}E{i + 1}' if i + 1 < len(kids) else '@' + p + 'P'
            E += [(here, ('store', lambda L, k=k, team=team: GLANE(team, k), lambda L: lane + 1,
                          True, f'@{here}G')),
                  (f'{here}G', ('add', lambda L, k=k, team=team: GRANT(team, k), lambda L: 1, '_',
                                nxt))]
        if not kids or kind != 'r':
            E.append((p + 'E', ('br', lambda L: True, '@' + p + 'P', '@' + p + 'P')))
        B += E
        if design == 'lane_early' and parent is not None:
            B += [(p + 'P', ('load', lambda L, team=team: GLANE(team, n), 'plane', '@' + p + 'U'))]
        else:
            B += [(p + 'P', ('set', 'plane', lambda L: DEL, '@' + p + 'U'))]
        B += [(p + 'U', ('set', 'up', lambda L: 0, '@' + p + 'U1')),
              (p + 'U1', ('set', 'down', lambda L: 0, '@' + p + 'LOOP')),
              (p + 'LOOP', ('br', lambda L, P=pieces: L['down'] >= P, '@' + p + 'R',
                            '@' + p + 'UPQ')),
              # Returning: the lane is free, and the team's next collective on
              # this node may begin.
              (p + 'R', ('ghost', lambda M, L, team=team: (
                  {BIND(n, lane): 0, TURN(team, n): M[TURN(team, n)] + 1}, {}), '@' + p + 'R1')),
              (p + 'R1', ('set', 'plane', lambda L: DEL, after))]

        def children_in(M, piece, team=team, base=arr, kid_pos=kid_pos):
            return all(M[ARR(team, n, c)] >= base[c] + piece + 1 for c in kid_pos)

        def wait_children(label, var, then, kid_pos=kid_pos, tagbase=tagbase,
                          children_in=children_in, kind=kind, team=team):
            if not kid_pos:
                return [(label, ('br', lambda L: True, then, then))]
            after = then if kind == 'b' else f'@{label}K0'
            W = [(label, ('await', lambda M, L, v=var: children_in(M, L[v]),
                          f'@{label}A0' if kind != 's' else after))]
            if kind != 's':
                # Each child's lane, read as its first piece of the collective is
                # taken - the arrival that shows the word written - and kept for
                # the collective (E4, as the model corrected it): nothing of the
                # team's next collective is written before this node's last piece
                # goes up, which is after this read.
                for i, c in enumerate(kid_pos):
                    nxt = f'@{label}A{i + 1}' if i + 1 < len(kid_pos) else after
                    W += [(f'{label}A{i}', ('br', lambda L, v=var: L[v] == 0 and design in READS_AT_ARRIVAL,
                                            f'@{label}AL{i}', nxt)),
                          (f'{label}AL{i}', ('load', lambda L, c=c, team=team: ALANE(team, n, c),
                                             f'k{c}', nxt))]
            if kind == 'b':
                # A broadcast's pieces up are empty: an arrival is all there is.
                return W
            if kind == 's':
                for i, c in enumerate(kid_pos):
                    nxt = f'@{label}K{i + 1}' if i + 1 < len(kid_pos) else then
                    W += [(f'{label}K{i}', ('check', lambda M, L, c=c, st=small_team:
                                            None if M[SMALL(st, n, c)] == tagbase else
                                            f'UP: node {n} read {M[SMALL(st, n, c)]} in its small '
                                            f'slot {c}, waiting for piece {tagbase}', nxt))]
                return W
            for i, c in enumerate(kid_pos):
                nxt = f'@{label}K{i + 1}' if i + 1 < len(kid_pos) else then
                W += [(f'{label}K{i}', ('check', lambda M, L, c=c, v=var:
                                        None if M[SLOT(n, lane, c, L[v] % depth)] == tagbase + L[v]
                                        else f'UP: node {n} lane {lane} read '
                                             f'{M[SLOT(n, lane, c, L[v] % depth)]} in its row {c}, '
                                             f'waiting for piece {tagbase + L[v]}', nxt))]
            return W

        def forward(label, var, then, kids=kids, kid_pos=kid_pos, tagbase=tagbase, team=team, me=me):
            """Release piece L[var] to every child: the first time, the child's
            lane read from beside its arrival counter (E4), then the put and the
            add."""
            if not kids:
                return [(label, ('br', lambda L: True, then, then))]
            F = []
            if kind == 's':
                for i, k in enumerate(kids):
                    here = label if i == 0 else f'{label}{i}'
                    nxt = f'@{label}{i + 1}' if i + 1 < len(kids) else then
                    F += [(here, ('store', lambda L, k=k, st=small_down_team: SMALL_DOWN(st, k),
                                  lambda L: tagbase, True, f'@{here}A')),
                          (f'{here}A', ('add', lambda L, k=k: REL(team, k), lambda L: 1, '_', nxt))]
                return F
            for i, (k, c) in enumerate(zip(kids, kid_pos)):
                here = label if i == 0 else f'{label}{i}'
                nxt = f'@{label}{i + 1}' if i + 1 < len(kids) else then
                if design == 'no_grant':
                    F += [(here, ('set', f'k{c}', lambda L: 1, f'@{here}C'))]
                elif design == 'release_lane_late':
                    # The proposal's rule: read as the first release goes down.
                    F += [(here, ('br', lambda L, c=c: L[f'k{c}'] is not None, f'@{here}C',
                                  f'@{here}Q')),
                          (f'{here}Q', ('load', lambda L, c=c, team=team: ALANE(team, n, c),
                                        f'k{c}', f'@{here}C'))]
                else:
                    # Read at entry (release_lane_early) or as the child's first
                    # piece was taken (wait_children).
                    F += [(here, ('br', lambda L: True, f'@{here}C', f'@{here}C'))]
                F += [(f'{here}C', ('check', lambda M, L, k=k, c=c, me=me:
                                    None if L[f'k{c}'] and M[BIND(k, L[f"k{c}"] - 1)] == me else
                                    f'LANE: node {n} releases into node {k} lane '
                                    f'{(L[f"k{c}"] or 0) - 1}, bound to '
                                    f'{M[BIND(k, max((L[f"k{c}"] or 0) - 1, 0))]}, not to '
                                    f'collective {me}', f'@{here}S')),
                      (f'{here}S', ('store', lambda L, k=k, c=c, v=var:
                                    RSLOT(k, L[f'k{c}'] - 1, L[v] % depth),
                                    lambda L, v=var: tagbase + L[v], True, f'@{here}A')),
                      (f'{here}A', ('add', lambda L, k=k: REL(team, k), lambda L: 1, '_', nxt))]
            return F

        clear_kids = [(p + 'NX', ('set', 'down', lambda L: L['down'] + 1, '@' + p + 'NXK'))]
        # The children's lanes are this collective's: forgotten as it ends.
        def forget(L, kid_pos=kid_pos, P=pieces):
            return DEL
        clear_kids += [(p + 'NXK', ('br', lambda L, P=pieces: L['down'] >= P, '@' + p + 'NXF',
                                    '@' + p + 'LOOP'))]
        F = []
        for i, c in enumerate(kid_pos):
            nxt = f'@{p}NXF{i + 1}' if i + 1 < len(kid_pos) else '@' + p + 'LOOP'
            F.append((p + 'NXF' if i == 0 else f'{p}NXF{i}', ('set', f'k{c}', forget, nxt)))
        if not kid_pos:
            F.append((p + 'NXF', ('br', lambda L: True, '@' + p + 'LOOP', '@' + p + 'LOOP')))
        clear_kids += F

        if parent is None:
            B += [(p + 'UPQ', ('br', lambda L: True, '@' + p + 'RW', '@' + p + 'RW'))]
            B += wait_children(p + 'RW', 'down', '@' + p + 'FW')
            B += forward(p + 'FW', 'down', '@' + p + 'NX')
            B += clear_kids
        else:
            def granted(M, team=team, grant=grant, kind=kind):
                if design == 'no_grant' or kind != 'r':
                    return True     # a broadcast's and a small collective's need none
                if design == 'stale_grant':
                    return M[GRANT(team, n)] >= 1
                return M[GRANT(team, n)] >= grant + 1

            def choose(M, L, P=pieces, w=window, rel=rel, team=team, children_in=children_in,
                       granted=granted):
                alts = []
                if L['up'] < P and L['up'] < L['down'] + w and children_in(M, L['up']) and \
                        granted(M):
                    alts.append(({}, {'go': 1}, f'granted, and children hold piece {L["up"]}: '
                                                'send it up'))
                if L['down'] < P and M[REL(team, n)] >= rel + L['down'] + 1:
                    alts.append(({}, {'go': 2}, f'release {L["down"]} in: consume it'))
                return alts

            B += [(p + 'UPQ', ('nd', choose, '@' + p + 'GO')),
                  (p + 'GO', ('br', lambda L: L['go'] == 1, '@' + p + 'GW', '@' + p + 'GR')),
                  (p + 'GW', ('set', 'go', lambda L: DEL, '@' + p + 'W')),
                  (p + 'GR', ('set', 'go', lambda L: DEL, '@' + p + 'RW'))]
            B += wait_children(p + 'W', 'up', '@' + p + ('SS' if kind == 's' else 'PL'))
            if kind == 's':
                # A small piece: the team number's own small slot, then the add.
                B += [(p + 'SS', ('store', lambda L, pa=parent, pos=position, st=small_team:
                                  SMALL(st, pa, pos), lambda L, tb=tagbase: tb, True,
                                  '@' + p + 'SEND1'))]
            # The parent's lane, from the grant line, once (E4): the grant was
            # seen above, so the line is this collective's.
            B += [(p + 'PL', ('br', lambda L, kind=kind: L['plane'] is not None or kind != 'r',
                              '@' + p + 'FIRST', '@' + p + 'PLL')),
                  # no_grant's lane is the node's one, by arithmetic.
                  (p + 'PLL', ('set', 'plane', lambda L: 1, '@' + p + 'FIRST')
                   if design == 'no_grant' else
                   ('load', lambda L, team=team: GLANE(team, n), 'plane', '@' + p + 'FIRST')),
                  # Before the first add of the collective: this thread's lane,
                  # beside the parent's arrival counter for this position.
                  (p + 'FIRST', ('br', lambda L: L['up'] == 0, '@' + p + 'AL', '@' + p + 'SC')),
                  (p + 'AL', ('store', lambda L, pa=parent, pos=position, team=team:
                              ALANE(team, pa, pos), lambda L: lane + 1, True, '@' + p + 'SC')),
                  # A broadcast's piece up is empty: nothing goes into the lane.
                  (p + 'SC', ('br', lambda L: True, '@' + p + 'SEND1', '@' + p + 'SEND1'))
                  if kind != 'r' else
                  (p + 'SC', ('check', lambda M, L, pa=parent, me=me:
                              None if L['plane'] and M[BIND(pa, L['plane'] - 1)] == me else
                              f'LANE: node {n} puts into node {pa} lane {(L["plane"] or 0) - 1}, '
                              f'bound to {M[BIND(pa, max((L["plane"] or 0) - 1, 0))]}, not to '
                              f'collective {me}', '@' + p + 'SEND')),
                  (p + 'SEND', ('store', lambda L, pa=parent, pos=position:
                                SLOT(pa, L['plane'] - 1, pos, L['up'] % depth),
                                lambda L, tb=tagbase: tb + L['up'], True, '@' + p + 'SEND1')),
                  (p + 'SEND1', ('add', lambda L, pa=parent, pos=position, team=team:
                                 ARR(team, pa, pos), lambda L: 1, '_', '@' + p + 'SEND2')),
                  (p + 'SEND2', ('set', 'up', lambda L: L['up'] + 1, '@' + p + 'LOOP')),
                  (p + 'RW', ('await', lambda M, L, rel=rel, team=team:
                              M[REL(team, n)] >= rel + L['down'] + 1, '@' + p + 'RX')),
                  (p + 'RX', ('check', lambda M, L, tb=tagbase, st=small_down_team:
                              None if M[SMALL_DOWN(st, n)] == tb else
                              f'DOWN: node {n} read small release {M[SMALL_DOWN(st, n)]}, '
                              f'waiting for {tb}', '@' + p + 'FW')
                   if kind == 's' else
                   ('check', lambda M, L, tb=tagbase:
                    None if M[RSLOT(n, lane, L['down'] % depth)] == tb + L['down'] else
                    f'DOWN: node {n} lane {lane} read release '
                    f'{M[RSLOT(n, lane, L["down"] % depth)]}, '
                    f'waiting for {tb + L["down"]}', '@' + p + 'FW'))]
            B += forward(p + 'FW', 'down', '@' + p + 'NX')
            B += clear_kids
    B += [('DONE', ('gpost', lambda M, L: ({FIN: M[FIN] + 1}, {}), '@X')), ('X', ('done',))]
    if not plan:
        return assemble([('START', ('br', lambda L: True, '@DONE', '@DONE'))] + B)
    return assemble(B)


def case(design, nodes, threads_per_node, fanout, depth, teams, order, pieces):
    """`teams`: {name: member nodes}; `order`: [(team, root index, {node: thread}
    [, kind])], every member's order of its collectives, which of its node's
    threads leads each there (a node not named leads with thread 0), and what it
    is: 'r' a reduction (the default), 'b' a broadcast, 's' a small reduction -
    one piece through the team's own small slots."""
    collectives = []
    for x, entry in enumerate(order):
        team, root, leads = entry[:3]
        kind = entry[3] if len(entry) > 3 else 'r'
        members = teams[team]
        collectives.append((team, members, root, 100 * (x + 1),
                            {n: leads.get(n, 0) for n in members}, kind))
    progs = [thread(design, n, h, fanout, depth, collectives, pieces)
             for n in range(nodes) for h in range(threads_per_node)]
    mem = {FIN: 0}
    for n in range(nodes):
        for h in range(threads_per_node):
            mem[BIND(n, h)] = 0
            for j in range(depth):
                mem[RSLOT(n, h, j)] = 0
            for c in range(fanout):
                for j in range(depth):
                    mem[SLOT(n, h, c, j)] = 0
        for team in list(teams) + ['*']:
            mem[SMALL_DOWN(team, n)] = 0
            for c in range(fanout):
                mem[SMALL(team, n, c)] = 0
        for team in teams:
            mem[REL(team, n)] = 0
            mem[GRANT(team, n)] = 0
            mem[GLANE(team, n)] = 0
            mem[TURN(team, n)] = 0
            for c in range(fanout):
                mem[ARR(team, n, c)] = 0
                mem[ALANE(team, n, c)] = 0
    shape = ' '.join(f'{t}={"".join(map(str, m))}' for t, m in teams.items())
    def said(leads):
        return ''.join(f'{n}{h}' for n, h in sorted(leads.items()) if h)
    name = (f'lane/{design} nodes={nodes} threads={threads_per_node} fanout={fanout} '
            f'depth={depth} {shape} '
            f'order={",".join(f"{e[0]}@{e[1]}" + (e[3] if len(e) > 3 else "") + (":" + said(e[2]) if said(e[2]) else "") for e in order)} '
            f'pieces={pieces}')
    return name, progs, mem, len(progs)


T01_U12 = {'T': [0, 1], 'U': [1, 2]}
ALL = {'T': [0, 1, 2], 'U': [0, 1, 2]}
T01 = {'T': [0, 1]}
ROTATING = {'T': [0, 1, 2, 3]}
# (design, nodes, threads a node, fanout, depth, teams, order, pieces, expect_ok);
# an order entry is (team, root index, {node: leading thread}).
CASES = [
    # The first model's cases, one thread a node: node 1 the parent in both
    # teams, its two children - nodes 0 and 2 - at position 0 of each tree.
    ('design', 3, 1, 2, 2, T01_U12, [('T', 1, {}), ('U', 0, {})], 3, True),
    ('design', 3, 1, 2, 1, T01_U12, [('T', 1, {}), ('U', 0, {})], 3, True),
    ('design', 3, 1, 2, 2, T01_U12, [('T', 1, {}), ('U', 0, {}), ('T', 1, {})], 2, True),
    ('design', 3, 1, 2, 2, T01_U12, [('T', 1, {}), ('U', 1, {})], 3, True),
    ('design', 3, 1, 2, 2, ALL, [('T', 0, {}), ('U', 1, {})], 3, True),
    ('design', 3, 1, 1, 2, ALL, [('T', 0, {}), ('U', 2, {})], 3, True),          # chains
    ('design', 4, 1, 2, 2, {'T': [0, 1, 2], 'U': [1, 2, 3]}, [('T', 1, {}), ('U', 0, {})], 3, True),
    ('no_grant', 3, 1, 2, 2, T01_U12, [('T', 1, {}), ('U', 0, {})], 3, False),
    # With every node in both teams and one order, pushing without the grant
    # corrupts no piece - the first model found UP holding here, consecutive
    # collectives on one team again - but it puts into a lane its parent has
    # not yet bound to the collective, which E5's rule forbids (LANE).
    ('no_grant', 3, 1, 2, 2, ALL, [('T', 0, {}), ('U', 1, {})], 3, False),
    ('stale_grant', 3, 1, 2, 2, T01_U12, [('T', 1, {}), ('U', 0, {}), ('T', 1, {})], 2, False),
    # Two threads on node 1, leading T and U at once: each its own lane.
    ('design', 3, 2, 2, 2, T01_U12, [('T', 1, {1: 0}), ('U', 0, {1: 1})], 2, True),
    ('one_lane', 3, 2, 2, 2, T01_U12, [('T', 1, {1: 0}), ('U', 0, {1: 1})], 2, False),
    # T's parent, node 1, leads its two collectives on two threads in turn:
    # the child must put each into the lane its grant names.
    ('design', 2, 2, 1, 1, T01, [('T', 1, {1: 0}), ('T', 1, {1: 1})], 2, True),
    ('lane_early', 2, 2, 1, 1, T01, [('T', 1, {1: 0}), ('T', 1, {1: 1})], 2, False),
    ('stale_grant', 2, 2, 1, 1, T01, [('T', 1, {1: 0}), ('T', 1, {1: 1})], 2, False),
    # T's child, node 1, on two threads in turn: the parent must release into
    # the lane the child's first arrival names.
    ('design', 2, 2, 1, 1, T01, [('T', 0, {1: 0}), ('T', 0, {1: 1})], 2, True),
    ('release_lane_early', 2, 2, 1, 1, T01, [('T', 0, {1: 0}), ('T', 0, {1: 1})], 2, False),
    # Broadcasts need no grant: node 1 leads T's broadcast and U's reduction at
    # once on two threads; a child's lanes across broadcasts on two threads; a
    # team's reduction, broadcast and reduction led by its threads in turn.
    ('design', 3, 2, 2, 2, T01_U12, [('T', 1, {1: 0}, 'b'), ('U', 0, {1: 1})], 2, True),
    ('design', 2, 2, 1, 1, T01, [('T', 0, {1: 0}, 'b'), ('T', 0, {1: 1}, 'b')], 2, True),
    ('release_lane_early', 2, 2, 1, 1, T01, [('T', 0, {1: 0}, 'b'), ('T', 0, {1: 1}, 'b')], 2,
     False),
    ('design', 2, 2, 1, 1, T01, [('T', 1, {1: 0}), ('T', 1, {1: 1}, 'b'), ('T', 1, {1: 0})], 2,
     True),
    ('lane_early', 2, 2, 1, 1, T01, [('T', 1, {1: 0}), ('T', 1, {1: 1}, 'b'), ('T', 1, {1: 1})],
     2, False),
    # Small collectives through each team number's own slots, between large
    # ones through the lanes: node 1 the parent in both teams, one thread, and
    # two threads leading the two teams at once.
    ('design', 3, 1, 2, 2, T01_U12,
     [('T', 1, {}, 's'), ('U', 0, {}, 's'), ('T', 1, {}), ('U', 0, {}, 's'), ('T', 1, {}, 's')],
     2, True),
    ('design', 3, 2, 2, 2, T01_U12,
     [('T', 1, {1: 0}, 's'), ('U', 0, {1: 1}), ('T', 1, {1: 0}), ('U', 0, {1: 1}, 's')], 2, True),
    ('node_small', 3, 1, 2, 2, T01_U12, [('T', 1, {}, 's'), ('U', 0, {}, 's')], 2, False),
    # The small path's release slot shared by a node's teams (node_small_down,
    # round six's H3): the only breakage that reaches the small path's DOWN
    # check, which every large-path breakage meets LANE before. Two teams that
    # share node 0 as a parent: with one thread a node their small reductions
    # take turns and the sharing holds; with two threads leading them at once
    # node 0 reads one team's release in the other's slot. The design holds
    # the last case.
    ('node_small_down', 3, 1, 2, 2, {'T': [0, 1], 'U': [0, 2]},
     [('T', 1, {}, 's'), ('U', 1, {}, 's'), ('T', 1, {}, 's')], 2, True),
    ('node_small_down', 3, 2, 2, 2, {'T': [0, 1], 'U': [0, 2]},
     [('T', 1, {0: 0}, 's'), ('U', 1, {0: 1}, 's')], 2, False),
    ('design', 3, 2, 2, 2, {'T': [0, 1], 'U': [0, 2]},
     [('T', 1, {0: 0}, 's'), ('U', 1, {0: 1}, 's')], 2, True),
    # One team over four nodes whose tree turns between collectives, as a
    # broadcast's does: rooted at node 0, node 1's child at position 0 is node
    # 3; rooted at node 1 it is node 2, which leads the second on its thread 1.
    # With no more pieces than the window, node 1 sends its last piece up before
    # it takes its first release, so node 2 can finish the first collective and
    # write its lane beside node 1's counter for position 0 - the model's
    # finding: read at the first release, as the proposal had it, the word may
    # be the next collective's.
    ('design', 4, 2, 2, 2, ROTATING, [('T', 0, {}), ('T', 1, {2: 1}, 'b')], 1, True),
    ('design', 4, 2, 2, 2, ROTATING, [('T', 0, {}, 'b'), ('T', 1, {2: 1}, 'b')], 2, True),
    ('release_lane_late', 4, 2, 2, 2, ROTATING, [('T', 0, {}), ('T', 1, {2: 1}, 'b')], 1, False),
    ('release_lane_late', 4, 2, 2, 2, ROTATING, [('T', 0, {}, 'b'), ('T', 1, {2: 1}, 'b')], 2,
     False),
    # More pieces than the window keep the rule safe even read late: node 1
    # takes release 0 before it can send its last piece up.
    ('release_lane_late', 4, 2, 2, 1, ROTATING, [('T', 0, {}), ('T', 1, {2: 1}, 'b')], 2, True),
]


def main():
    want = select(sys.argv)
    ok = True
    for design, nodes, per_node, fanout, depth, teams, order, pieces, expect in CASES:
        name, progs, mem, count = case(design, nodes, per_node, fanout, depth, teams, order,
                                       pieces)
        if not want(name):
            continue
        res = explore(name, progs, mem, max_seconds=900, liveness_max_states=3_000_000,
                      done=lambda M, count=count: M[FIN] == count)
        ok &= report(res, expect_ok=expect)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
