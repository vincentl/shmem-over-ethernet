#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The scans' lanes beside the reductions' (the team redesign's step E, its
second part; design §5.8): the reductions, broadcasts and small reductions of
lane_model.py, and beside them the scans of osoe_internode_generic_scan - every
member a participant, a pre-order k-ary tree over the team's ranks, the scans'
own counters, grant and lane words per team number, and the SAME lanes (one per
thread) as the reductions. Written by the second part's adversarial read, which
kept it outside the tree; brought in with round six's review (its H2: the
scans' lane protocol - the lane a grant names, the lane word, threads - was
model-checked nowhere in the tree), with the breakage that review found the
suite blind to, scan_one_lane.

A scan entry in a case's order has kind 'S' (large: lanes, grant, lane word)
or 'z' (small: the team number's own scan small slots). Its root is rank 0 of
the team's member list (the list is in rank order); the entry's root index is
ignored.

What the scans model from the code:
  * at entry (large) the member binds its thread's lane and grants each scan
    child: the grant word (collective, lane) into the child's scan grant line,
    then one more on the child's scan grant counter;
  * a member takes piece `up` up when the window allows (up < down + D), every
    child's total for it is in (scan row counters, base = the team's scan
    pieces so far), and (large, not root) the grant is in - counter >= the
    member's own count of grants taken + 1; it reads the grant once (clearing
    it, checking it names this collective), reads each child's lane word at
    up == 0 (clearing it, checking it names this collective), checks every
    child's tag in its own lane slot (base + up) mod D, then (not root) puts
    its own lane word before its first add, checks the parent's lane is bound
    to this collective, puts its tag into the parent's lane slot and adds;
  * a member takes piece `down` down when down < up and (root or its down
    counter shows it), checks its tag, then for each child checks the child's
    lane is bound to this collective, puts into it and adds.
Slot indices are (team's scan pieces before + piece) mod D, as the code's
lane_slot(lane, position, base + up) has them; the reductions' are piece mod D.

Designs for the scans (the reductions run as lane_model.py's `design`):
  design             the code.
  scan_shared_words  the scans count on the reductions' words (arrival,
                     release, grant, grant line, lane words, small slots) with
                     their own bases: should fail where both run on a team.
  scan_no_grant      a large scan's child finds its parent's lane by arithmetic
                     (thread 0's) and does not wait for the grant (LANE).
  scan_early_word    a parent reads its children's lane words as it enters,
                     before their first add (WORD, LANE).
  scan_late_word     a parent reads a child's lane word as it first sends it a
                     down, not with its first total - the record says the scan
                     tree never turns, so this is safe (expected: ok).
  scan_one_lane      every large scan of a node through lane 0, whatever its
                     thread (round six's H1 mutation): fails where two threads
                     of a node scan two teams at once (LANE), and holds where a
                     node has one thread.

Usage: lane_scan_model.py [substring ...]
"""
import sys

from engine import DEL, assemble, explore, report, select  # noqa: E402

ARR = lambda t, n, c: ('arr', t, n, c)
REL = lambda t, n: ('rel', t, n)
GRANT = lambda t, n: ('grant', t, n)
GLANE = lambda t, n: ('glane', t, n)
ALANE = lambda t, n, c: ('alane', t, n, c)
SLOT = lambda n, h, c, j: ('slot', n, h, c, j)
SMALL = lambda t, n, c: ('small', t, n, c)
SMALL_DOWN = lambda t, n: ('small_down', t, n)
RSLOT = lambda n, h, j: ('rslot', n, h, j)
# The scans' own words, per team number.
SARR = lambda t, n, c: ('sarr', t, n, c)
SDOWN = lambda t, n: ('sdown', t, n)
SGRANT = lambda t, n: ('sgrant', t, n)
SGLANE = lambda t, n: ('sglane', t, n)
SALANE = lambda t, n, c: ('salane', t, n, c)
SSMALL = lambda t, n, c: ('ssmall', t, n, c)
SSMALL_DOWN = lambda t, n: ('ssmall_down', t, n)
BIND = lambda n, h: ('$bind', n, h)
TURN = lambda t, n: ('$turn', t, n)
FIN = ('$fin',)
WORD = 1000                     # a word: collective * WORD + lane + 1


def tree(members, root, fanout):
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


def scan_places(members, k):
    """The scan's pre-order k-ary tree over ranks 0 .. M - 1 (scan_place):
    {node: (parent node or None, position, [child nodes])}."""
    info = {}

    def kids_of(a, b):
        rest = b - (a + 1)
        if rest <= 0:
            return []
        m = min(k, rest)
        out, start = [], a + 1
        for j in range(m):
            size = rest // m + (1 if j < rest % m else 0)
            out.append((start, start + size))
            start += size
        return out

    def walk(a, b, parent, position):
        kids = kids_of(a, b)
        info[members[a]] = (None if parent is None else members[parent], position or 0,
                            [members[c] for c, _ in kids])
        for j, (c, e) in enumerate(kids):
            walk(c, e, a, j)
    walk(0, len(members), None, None)
    return info


def chain(prefix, steps, end):
    """[(name, instruction without its next)] -> blocks, each to the next."""
    out = []
    for i, (name, ins) in enumerate(steps):
        nxt = '@' + prefix + steps[i + 1][0] if i + 1 < len(steps) else end
        out.append((prefix + name, ins if ins[0] == 'br' else ins + (nxt,)))
    return out


def reduction_blocks(design, n, lane, fanout, depth, entry, after, p):
    """lane_model.py's program for one collective, design 'design'."""
    (x, team, parent, position, kids, kid_pos, arr, rel, grant, turn, tagbase, kind,
     pieces) = entry
    window = depth
    small_team = team
    me = x + 1
    B = []
    B += [(p + 'T', ('await', lambda M, L, team=team, turn=turn: M[TURN(team, n)] == turn,
                     '@' + p + 'B')),
          (p + 'B', ('ghost', lambda M, L, me=me: ({BIND(n, lane): me}, {}), '@' + p + 'L')),
          (p + 'L', ('br', lambda L: True, '@' + p + 'E', '@' + p + 'E'))]
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
    B += [(p + 'P', ('set', 'plane', lambda L: DEL, '@' + p + 'U'))]
    B += [(p + 'U', ('set', 'up', lambda L: 0, '@' + p + 'U1')),
          (p + 'U1', ('set', 'down', lambda L: 0, '@' + p + 'LOOP')),
          (p + 'LOOP', ('br', lambda L, P=pieces: L['down'] >= P, '@' + p + 'R',
                        '@' + p + 'UPQ')),
          (p + 'R', ('ghost', lambda M, L, team=team: (
              {BIND(n, lane): 0, TURN(team, n): M[TURN(team, n)] + 1}, {}), '@' + p + 'R1')),
          (p + 'R1', ('set', 'plane', lambda L: DEL, after))]

    def children_in(M, piece, team=team, base=arr, kid_pos=kid_pos):
        return all(M[ARR(team, n, c)] >= base[c] + piece + 1 for c in kid_pos)

    def wait_children(label, var, then, kid_pos=kid_pos, tagbase=tagbase,
                      children_in=children_in, kind=kind, team=team):
        if not kid_pos:
            return [(label, ('br', lambda L: True, then, then))]
        after_ = then if kind == 'b' else f'@{label}K0'
        W = [(label, ('await', lambda M, L, v=var: children_in(M, L[v]),
                      f'@{label}A0' if kind != 's' else after_))]
        if kind != 's':
            for i, c in enumerate(kid_pos):
                nxt = f'@{label}A{i + 1}' if i + 1 < len(kid_pos) else after_
                W += [(f'{label}A{i}', ('br', lambda L, v=var: L[v] == 0,
                                        f'@{label}AL{i}', nxt)),
                      (f'{label}AL{i}', ('load', lambda L, c=c, team=team: ALANE(team, n, c),
                                         f'k{c}', nxt))]
        if kind == 'b':
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
        if not kids:
            return [(label, ('br', lambda L: True, then, then))]
        F = []
        if kind == 's':
            for i, k in enumerate(kids):
                here = label if i == 0 else f'{label}{i}'
                nxt = f'@{label}{i + 1}' if i + 1 < len(kids) else then
                F += [(here, ('store', lambda L, k=k, st=small_team: SMALL_DOWN(st, k),
                              lambda L: tagbase, True, f'@{here}A')),
                      (f'{here}A', ('add', lambda L, k=k: REL(team, k), lambda L: 1, '_', nxt))]
            return F
        for i, (k, c) in enumerate(zip(kids, kid_pos)):
            here = label if i == 0 else f'{label}{i}'
            nxt = f'@{label}{i + 1}' if i + 1 < len(kids) else then
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
    clear_kids += [(p + 'NXK', ('br', lambda L, P=pieces: L['down'] >= P, '@' + p + 'NXF',
                                '@' + p + 'LOOP'))]
    F = []
    for i, c in enumerate(kid_pos):
        nxt = f'@{p}NXF{i + 1}' if i + 1 < len(kid_pos) else '@' + p + 'LOOP'
        F.append((p + 'NXF' if i == 0 else f'{p}NXF{i}', ('set', f'k{c}', lambda L: DEL, nxt)))
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
            if kind != 'r':
                return True
            return M[GRANT(team, n)] >= grant + 1

        def choose(M, L, P=pieces, w=window, rel=rel, team=team, children_in=children_in,
                   granted=granted):
            alts = []
            if L['up'] < P and L['up'] < L['down'] + w and children_in(M, L['up']) and \
                    granted(M):
                alts.append(({}, {'go': 1}, f'node {n}: piece {L["up"]} up'))
            if L['down'] < P and M[REL(team, n)] >= rel + L['down'] + 1:
                alts.append(({}, {'go': 2}, f'node {n}: release {L["down"]} in'))
            return alts

        B += [(p + 'UPQ', ('nd', choose, '@' + p + 'GO')),
              (p + 'GO', ('br', lambda L: L['go'] == 1, '@' + p + 'GW', '@' + p + 'GR')),
              (p + 'GW', ('set', 'go', lambda L: DEL, '@' + p + 'W')),
              (p + 'GR', ('set', 'go', lambda L: DEL, '@' + p + 'RW'))]
        B += wait_children(p + 'W', 'up', '@' + p + ('SS' if kind == 's' else 'PL'))
        if kind == 's':
            B += [(p + 'SS', ('store', lambda L, pa=parent, pos=position, st=small_team:
                              SMALL(st, pa, pos), lambda L, tb=tagbase: tb, True,
                              '@' + p + 'SEND1'))]
        B += [(p + 'PL', ('br', lambda L, kind=kind: L['plane'] is not None or kind != 'r',
                          '@' + p + 'FIRST', '@' + p + 'PLL')),
              (p + 'PLL', ('load', lambda L, team=team: GLANE(team, n), 'plane', '@' + p + 'FIRST')),
              (p + 'FIRST', ('br', lambda L: L['up'] == 0, '@' + p + 'AL', '@' + p + 'SC')),
              (p + 'AL', ('store', lambda L, pa=parent, pos=position, team=team:
                          ALANE(team, pa, pos), lambda L: lane + 1, True, '@' + p + 'SC')),
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
              (p + 'RX', ('check', lambda M, L, tb=tagbase, st=small_team:
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
    return B


def scan_blocks(design, n, lane, fanout, depth, entry, after, p):
    """osoe_internode_generic_scan for one scan, at node (PE) n."""
    (x, team, parent, position, kids, kid_pos, sbase, sgrant, turn, tagbase, kind,
     count) = entry
    window = depth
    me = x + 1
    large = kind == 'S'
    root = parent is None
    shared = design == 'scan_shared_words'
    K_ARR = ARR if shared else SARR
    K_DOWN = REL if shared else SDOWN
    K_GRANT = GRANT if shared else SGRANT
    K_GLANE = GLANE if shared else SGLANE
    K_ALANE = ALANE if shared else SALANE
    K_SMALL = SMALL if shared else SSMALL
    K_SMALL_DOWN = SMALL_DOWN if shared else SSMALL_DOWN

    def slot(q):
        return (sbase + q) % depth

    B = [(p + 'T', ('await', lambda M, L: M[TURN(team, n)] == turn, '@' + p + 'B'))]
    if large:
        B.append((p + 'B', ('ghost', lambda M, L: ({BIND(n, lane): me}, {}), '@' + p + 'E')))
    else:
        B.append((p + 'B', ('br', lambda L: True, '@' + p + 'E', '@' + p + 'E')))
    gk = kids if large else []
    E = []
    for i, k in enumerate(gk):
        here = p + 'E' if i == 0 else f'{p}E{i}'
        nxt = f'@{p}E{i + 1}' if i + 1 < len(gk) else '@' + p + 'I'
        E += [(here, ('store', lambda L, k=k: K_GLANE(team, k), lambda L: me * WORD + lane + 1,
                      True, f'@{here}G')),
              (f'{here}G', ('add', lambda L, k=k: K_GRANT(team, k), lambda L: 1, '_', nxt))]
    if not gk:
        E.append((p + 'E', ('br', lambda L: True, '@' + p + 'I', '@' + p + 'I')))
    B += E
    early = []
    if design == 'scan_early_word' and large:
        for c in kid_pos:
            early += [(f'EW{c}', ('load', lambda L, c=c: K_ALANE(team, n, c), f'w{c}')),
                      (f'EX{c}', ('check', lambda M, L, c=c:
                                  None if L[f'w{c}'] and L[f'w{c}'] // WORD == me else
                                  f'WORD: node {n} read lane word {L[f"w{c}"]} of position {c} '
                                  f'in collective {me}')),
                      (f'EC{c}', ('store', lambda L, c=c: K_ALANE(team, n, c), lambda L: 0, True)),
                      (f'ES{c}', ('set', f'k{c}', lambda L, c=c: L[f'w{c}'] % WORD)),
                      (f'ED{c}', ('set', f'w{c}', lambda L: DEL))]
    B += chain(p, [('I', ('set', 'up', lambda L: 0)),
                   ('I1', ('set', 'down', lambda L: 0)),
                   ('I2', ('set', 'plane', lambda L: DEL))] + early, '@' + p + 'LOOP')
    B += [(p + 'LOOP', ('br', lambda L: L['down'] >= count, '@' + p + 'R', '@' + p + 'CH')),
          (p + 'R', ('ghost', lambda M, L: (
              ({BIND(n, lane): 0} if large else {}) | {TURN(team, n): M[TURN(team, n)] + 1}, {}),
              '@' + p + 'R1'))]
    B += chain(p, [('R1', ('set', 'plane', lambda L: DEL))] +
               [(f'R2_{c}', ('set', f'k{c}', lambda L: DEL)) for c in kid_pos], after)

    def children_in(M, q):
        return all(M[K_ARR(team, n, c)] >= sbase + q + 1 for c in kid_pos)

    def granted(M, L):
        if root or not large or design == 'scan_no_grant' or L['plane'] is not None:
            return True
        return M[K_GRANT(team, n)] >= sgrant + 1

    def choose(M, L):
        alts = []
        if L['up'] < count and L['up'] < L['down'] + window and children_in(M, L['up']) and \
                granted(M, L):
            alts.append(({}, {'go': 1}, f'node {n}: scan {me} piece {L["up"]} up'))
        if L['down'] < L['up'] and (root or M[K_DOWN(team, n)] >= sbase + L['down'] + 1):
            alts.append(({}, {'go': 2}, f'node {n}: scan {me} piece {L["down"]} down'))
        return alts

    B += [(p + 'CH', ('nd', choose, '@' + p + 'GO')),
          (p + 'GO', ('br', lambda L: L['go'] == 1, '@' + p + 'UG', '@' + p + 'DG'))]

    # Up.
    U = [('UG', ('set', 'go', lambda L: DEL))]
    if large and design not in ('scan_early_word', 'scan_late_word'):
        # Each child's lane word, at up == 0, read, checked, cleared.
        words = []
        for c in kid_pos:
            words += [(f'UW{c}', ('load', lambda L, c=c: K_ALANE(team, n, c), f'w{c}')),
                      (f'UX{c}', ('check', lambda M, L, c=c:
                                  None if L[f'w{c}'] and L[f'w{c}'] // WORD == me else
                                  f'WORD: node {n} read lane word {L[f"w{c}"]} of position {c} '
                                  f'in collective {me}')),
                      (f'UC{c}', ('store', lambda L, c=c: K_ALANE(team, n, c), lambda L: 0, True)),
                      (f'US{c}', ('set', f'k{c}', lambda L, c=c: L[f'w{c}'] % WORD)),
                      (f'UD{c}', ('set', f'w{c}', lambda L: DEL))]
        if words:
            U += [('UA', ('br', lambda L: L['up'] == 0, '@' + p + words[0][0],
                          '@' + p + 'UK'))]
            U += words
    U += [('UK', ('set', '_k', lambda L: DEL))]
    for c in kid_pos:
        if large:
            U.append((f'UT{c}', ('check', lambda M, L, c=c:
                                 None if M[SLOT(n, lane, c, slot(L['up']))] == tagbase + L['up']
                                 else f'UP: node {n} lane {lane} read '
                                      f'{M[SLOT(n, lane, c, slot(L["up"]))]} in row {c}, '
                                      f'waiting for scan piece {tagbase + L["up"]}')))
        else:
            U.append((f'UT{c}', ('check', lambda M, L, c=c:
                                 None if M[K_SMALL(team, n, c)] == tagbase else
                                 f'UP: node {n} read {M[K_SMALL(team, n, c)]} in its scan small '
                                 f'slot {c}, waiting for {tagbase}')))
    if not root:
        if large:
            if design == 'scan_no_grant':
                U += [('PL', ('br', lambda L: L['plane'] is not None, '@' + p + 'FW',
                              '@' + p + 'PA')),
                      ('PA', ('set', 'plane', lambda L: 1))]
            else:
                U += [('PL', ('br', lambda L: L['plane'] is not None, '@' + p + 'FW',
                              '@' + p + 'PG')),
                      ('PG', ('load', lambda L: K_GLANE(team, n), 'gw')),
                      ('PX', ('check', lambda M, L:
                              None if L['gw'] and L['gw'] // WORD == me else
                              f'GRANT: node {n} read grant {L["gw"]} in collective {me}')),
                      ('PC', ('store', lambda L: K_GLANE(team, n), lambda L: 0, True)),
                      ('PS', ('set', 'plane', lambda L: L['gw'] % WORD)),
                      ('PD', ('set', 'gw', lambda L: DEL))]
            U += [('FW', ('br', lambda L: L['up'] == 0, '@' + p + 'AW', '@' + p + 'SC')),
                  ('AW', ('store', lambda L: K_ALANE(team, parent, position),
                          lambda L: me * WORD + lane + 1, True)),
                  ('SC', ('check', lambda M, L:
                          None if L['plane'] and M[BIND(parent, L['plane'] - 1)] == me else
                          f'LANE: node {n} puts scan piece into node {parent} lane '
                          f'{(L["plane"] or 0) - 1}, bound to '
                          f'{M[BIND(parent, max((L["plane"] or 0) - 1, 0))]}, not to '
                          f'collective {me}')),
                  ('SP', ('store', lambda L: SLOT(parent, L['plane'] - 1, position, slot(L['up'])),
                          lambda L: tagbase + L['up'], True))]
        else:
            U += [('SP', ('store', lambda L: K_SMALL(team, parent, position),
                          lambda L: tagbase, True))]
        U += [('SA', ('add', lambda L: K_ARR(team, parent, position), lambda L: 1, '_'))]
    U += [('UN', ('set', 'up', lambda L: L['up'] + 1))]
    B += chain(p, U, '@' + p + 'LOOP')

    # Down.
    D = [('DG', ('set', 'go', lambda L: DEL))]
    if not root:
        if large:
            D.append(('DK', ('check', lambda M, L:
                             None if M[RSLOT(n, lane, slot(L['down']))] == tagbase + L['down']
                             else f'DOWN: node {n} lane {lane} read '
                                  f'{M[RSLOT(n, lane, slot(L["down"]))]}, waiting for scan '
                                  f'piece {tagbase + L["down"]}')))
        else:
            D.append(('DK', ('check', lambda M, L:
                             None if M[K_SMALL_DOWN(team, n)] == tagbase else
                             f'DOWN: node {n} read {M[K_SMALL_DOWN(team, n)]} in its scan small '
                             f'down slot, waiting for {tagbase}')))
    for k, c in zip(kids, kid_pos):
        if large:
            if design == 'scan_late_word':
                D += [(f'LW{c}', ('br', lambda L, c=c: L[f'k{c}'] is not None,
                                  '@' + p + f'DC{c}', '@' + p + f'LL{c}')),
                      (f'LL{c}', ('load', lambda L, c=c: K_ALANE(team, n, c), f'w{c}')),
                      (f'LX{c}', ('check', lambda M, L, c=c:
                                  None if L[f'w{c}'] and L[f'w{c}'] // WORD == me else
                                  f'WORD: node {n} read lane word {L[f"w{c}"]} of position {c} '
                                  f'in collective {me}')),
                      (f'LC{c}', ('store', lambda L, c=c: K_ALANE(team, n, c), lambda L: 0,
                                  True)),
                      (f'LS{c}', ('set', f'k{c}', lambda L, c=c: L[f'w{c}'] % WORD)),
                      (f'LD{c}', ('set', f'w{c}', lambda L: DEL))]
            D += [(f'DC{c}', ('check', lambda M, L, k=k, c=c:
                              None if L[f'k{c}'] and M[BIND(k, L[f'k{c}'] - 1)] == me else
                              f'LANE: node {n} puts a scan down into node {k} lane '
                              f'{(L[f"k{c}"] or 0) - 1}, bound to '
                              f'{M[BIND(k, max((L[f"k{c}"] or 0) - 1, 0))]}, not to '
                              f'collective {me}')),
                  (f'DS{c}', ('store', lambda L, k=k, c=c: RSLOT(k, L[f'k{c}'] - 1,
                                                                 slot(L['down'])),
                              lambda L: tagbase + L['down'], True))]
        else:
            D.append((f'DS{c}', ('store', lambda L, k=k: K_SMALL_DOWN(team, k),
                                 lambda L: tagbase, True)))
        D.append((f'DA{c}', ('add', lambda L, k=k: K_DOWN(team, k), lambda L: 1, '_')))
    D.append(('DN', ('set', 'down', lambda L: L['down'] + 1)))
    B += chain(p, D, '@' + p + 'LOOP')
    return B


def thread(design, n, h, fanout, depth, collectives, pieces):
    lane = h
    B = []
    base_arr, base_rel, base_grant, turns = {}, {}, {}, {}
    scan_moved, scan_granted = {}, {}
    plan = []
    for x, (team, members, root, tagbase, leads, kind) in enumerate(collectives):
        if n not in members:
            continue
        turn = turns.setdefault(team, 0)
        if kind in ('S', 'z'):
            parent, position, kids = scan_places(members, fanout)[n]
            places = scan_places(members, fanout)
            kid_pos = [places[k][1] for k in kids]
            count = 1 if kind == 'z' else pieces
            sbase = scan_moved.get(team, 0)
            sgrant = scan_granted.get(team, 0)
            if leads[n] == h:
                plan.append(('scan', (x, team, parent, position, kids, kid_pos, sbase, sgrant,
                                      turn, tagbase, kind, count)))
            scan_moved[team] = sbase + count
            if kind == 'S' and parent is not None:
                scan_granted[team] = sgrant + 1
            turns[team] = turn + 1
            continue
        t = tree(members, root, fanout)
        parent, position, kids = t[n]
        arr = base_arr.setdefault(team, [0] * fanout)
        rel = base_rel.setdefault(team, 0)
        grant = base_grant.setdefault(team, 0)
        count = 1 if kind == 's' else pieces
        if leads[n] == h:
            plan.append(('red', (x, team, parent, position, kids, [t[k][1] for k in kids],
                                 list(arr), rel, grant, turn, tagbase, kind, count)))
        for k in kids:
            arr[t[k][1]] += count
        if parent is not None:
            base_rel[team] = rel + count
            if kind == 'r':
                base_grant[team] = grant + 1
        turns[team] = turn + 1

    for at, (what, entry) in enumerate(plan):
        x = entry[0]
        p = f'n{n}h{h}c{x}_'
        after = f'@n{n}h{h}c{plan[at + 1][1][0]}_T' if at + 1 < len(plan) else '@DONE'
        if what == 'scan':
            B += scan_blocks(design, n, 0 if design == 'scan_one_lane' else lane, fanout, depth,
                             entry, after, p)
        else:
            B += reduction_blocks('design', n, lane, fanout, depth, entry, after, p)
    B += [('DONE', ('gpost', lambda M, L: ({FIN: M[FIN] + 1}, {}), '@X')), ('X', ('done',))]
    if not plan:
        return assemble([('START', ('br', lambda L: True, '@DONE', '@DONE'))] + B)
    return assemble(B)


def case(design, nodes, threads_per_node, fanout, depth, teams, order, pieces):
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
        for team in teams:
            for K in (REL, GRANT, GLANE, TURN, SDOWN, SGRANT, SGLANE, SMALL_DOWN, SSMALL_DOWN):
                mem[K(team, n)] = 0
            for c in range(fanout):
                for K in (ARR, ALANE, SARR, SALANE, SMALL, SSMALL):
                    mem[K(team, n, c)] = 0
    shape = ' '.join(f'{t}={"".join(map(str, m))}' for t, m in teams.items())

    def said(leads):
        return ''.join(f'{n}{h}' for n, h in sorted(leads.items()) if h)
    name = (f'lanescan/{design} nodes={nodes} threads={threads_per_node} fanout={fanout} '
            f'depth={depth} {shape} '
            f'order={",".join(f"{e[0]}@{e[1]}" + (e[3] if len(e) > 3 else "") + (":" + said(e[2]) if said(e[2]) else "") for e in order)} '
            f'pieces={pieces}')
    return name, progs, mem, len(progs)


T3 = {'T': [0, 1, 2]}
# Node 1 rank 0 of both: the scans' shared parent (scan_pipeline_model.py's PQ).
P10_Q12 = {'P': [1, 0], 'Q': [1, 2]}
# Node 1 the scan parent of node 0 in P; Q's reduction tree rooted at index 0,
# node 1, with node 2 its child.
CASES = [
    # One team, one thread a node: scans, reductions and broadcasts in turn, the
    # broadcast's tree turned so that it differs from the scan's.
    ('design', 3, 1, 1, 2, T3, [('T', 0, {}, 'S'), ('T', 0, {}), ('T', 0, {}, 'S')], 2, True),
    ('design', 3, 1, 1, 2, T3, [('T', 0, {}, 'S'), ('T', 1, {}, 'b'), ('T', 0, {}, 'z'),
                                ('T', 0, {}, 'S')], 2, True),
    ('design', 3, 1, 2, 2, T3, [('T', 0, {}), ('T', 0, {}, 'S'), ('T', 2, {}, 'b'),
                                ('T', 0, {}, 'S')], 2, True),
    # A scan's shared parent, node 1: P's scan and Q's scan or reduction, node
    # 2 outside P entering Q at once - one thread, then two threads at once.
    ('design', 3, 1, 1, 2, P10_Q12, [('P', 0, {}, 'S'), ('Q', 0, {}, 'S')], 2, True),
    ('design', 3, 1, 1, 2, P10_Q12, [('P', 0, {}, 'S'), ('Q', 0, {})], 2, True),
    ('design', 3, 1, 1, 2, P10_Q12, [('P', 0, {}), ('Q', 0, {}, 'S'), ('P', 0, {}, 'S')], 2,
     True),
    ('design', 3, 2, 1, 2, P10_Q12, [('P', 0, {}, 'S'), ('Q', 0, {1: 1})], 2, True),
    ('design', 3, 2, 1, 2, P10_Q12, [('P', 0, {}, 'S'), ('Q', 0, {1: 1}, 'S')], 2, True),
    # Threads taking turns on a team's scans and reductions: node 1's threads
    # alternate, node 0's too, out of step.
    ('design', 2, 2, 1, 1, {'T': [0, 1]},
     [('T', 0, {}, 'S'), ('T', 0, {1: 1}, 'S'), ('T', 0, {0: 1}), ('T', 0, {1: 1}, 'S'),
      ('T', 1, {0: 1, 1: 0}, 'b')], 2, True),
    ('design', 2, 2, 1, 2, {'T': [1, 0]},
     [('T', 0, {}, 'S'), ('T', 0, {1: 1}, 'S'), ('T', 0, {0: 1}, 'S'), ('T', 1, {1: 1})], 2,
     True),
    # Small and large scans and reductions mixed on teams sharing node 1.
    ('design', 3, 1, 1, 2, P10_Q12, [('P', 0, {}, 'z'), ('Q', 0, {}, 'S'), ('P', 0, {}, 'S'),
                                     ('Q', 0, {}, 's'), ('Q', 0, {}, 'z')], 2, True),

    # Bigger: a middle member (four members, fan-out 1 and 2), two teams that
    # share members with different trees, two threads a node.
    ('design', 4, 1, 2, 2, {'T': [0, 1, 2, 3]},
     [('T', 0, {}, 'S'), ('T', 1, {}, 'b'), ('T', 0, {}, 'S')], 2, True),
    ('design', 4, 1, 1, 2, {'T': [0, 1, 2, 3]},
     [('T', 0, {}, 'S'), ('T', 0, {}, 'z'), ('T', 0, {}, 'S')], 3, True),
    ('design', 4, 2, 1, 2, {'T': [0, 1, 2], 'U': [2, 1, 3]},
     [('T', 0, {}, 'S'), ('U', 0, {1: 1}, 'S'), ('T', 0, {2: 1}, 'S'), ('U', 1, {}, 'b')], 2,
     True),
    ('design', 4, 2, 2, 2, {'T': [0, 1, 2], 'U': [1, 2, 3]},
     [('T', 0, {}, 'S'), ('U', 0, {1: 1}), ('U', 0, {1: 1, 2: 1}, 'S'), ('T', 0, {1: 1}, 'S')],
     2, True),
    ('design', 3, 2, 1, 1, P10_Q12,
     [('P', 0, {}, 'S'), ('Q', 0, {1: 1}, 'S'), ('Q', 0, {1: 0}), ('P', 0, {1: 1}, 'S')], 2,
     True),

    # Two teams of the same two PEs, the child's two threads scanning both at
    # once, the parent single-threaded; and both sides two-threaded.
    ('design', 2, 2, 1, 2, {'P': [1, 0], 'Q': [1, 0]},
     [('P', 0, {}, 'S'), ('Q', 0, {0: 1}, 'S'), ('P', 0, {0: 1}, 'S')], 2, True),
    ('design', 2, 2, 1, 2, {'P': [1, 0], 'Q': [1, 0]},
     [('P', 0, {}, 'S'), ('Q', 0, {0: 1, 1: 1}, 'S'), ('Q', 0, {}), ('P', 0, {1: 1}, 'z')], 2,
     True),
    # The record's claim: the late read is safe for scans.
    ('scan_late_word', 3, 1, 1, 2, T3, [('T', 0, {}, 'S'), ('T', 0, {}, 'S')], 1, True),
    ('scan_late_word', 2, 2, 1, 2, {'T': [0, 1]},
     [('T', 0, {}, 'S'), ('T', 0, {1: 1}, 'S'), ('T', 0, {}, 'S')], 1, True),
    ('scan_late_word', 3, 2, 1, 2, P10_Q12, [('P', 0, {}, 'S'), ('Q', 0, {1: 1}, 'S'),
                                             ('P', 0, {0: 1}, 'S')], 1, True),
    # Breakages, each expected to fail.
    ('scan_no_grant', 3, 2, 1, 2, P10_Q12, [('P', 0, {}, 'S'), ('Q', 0, {1: 1}, 'S')], 2, 'LANE'),
    ('scan_no_grant', 3, 1, 1, 2, P10_Q12, [('P', 0, {}, 'S'), ('Q', 0, {}, 'S')], 2, 'LANE'),
    ('scan_early_word', 2, 2, 1, 2, {'T': [0, 1]},
     [('T', 0, {}, 'S'), ('T', 0, {1: 1}, 'S')], 2, 'WORD'),
    ('scan_shared_words', 3, 1, 1, 2, T3, [('T', 0, {}, 'S'), ('T', 1, {}, 'b'),
                                           ('T', 0, {}, 'S')], 2, 'DOWN'),
    ('scan_shared_words', 3, 1, 1, 2, P10_Q12, [('Q', 0, {}), ('Q', 0, {}, 'S')], 2, 'GRANT'),
    ('scan_one_lane', 3, 2, 1, 2, P10_Q12, [('P', 0, {}, 'S'), ('Q', 0, {1: 1}, 'S')], 2, 'LANE'),
    ('scan_one_lane', 2, 2, 1, 2, {'P': [1, 0], 'Q': [1, 0]},
     [('P', 0, {}, 'S'), ('Q', 0, {}, 'S'), ('P', 0, {}, 'S')], 2, True),
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
        ok &= report(res, expect=expect)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
