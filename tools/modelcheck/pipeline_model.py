#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Pipelined pieces through the internode tree (design §5.5, §5.8).

The generic internode phase moves a collective's buffer through the tree of node
leads in 16 KiB pieces. As built, each piece makes a whole pass - up the tree,
then down - before the next starts, so a large buffer costs a round trip per
piece. The author's decision of 2026-09-25 is to keep D pieces in flight.

The protocol checked here:
  * A parent keeps D slots per child position and D release slots per node,
    piece k in slot k mod D.
  * A child puts piece k into its slot at the parent, then adds one to the
    parent's arrival counter for its position - one counter per position, not
    one per node: with pieces in flight a fast child's second arrival must not
    stand in for a slow child's first.
  * A node sends piece k up only once it has consumed piece k - D's release,
    which frees its slot at the parent for reuse (the parent read piece k - D
    before releasing it).
  * A node takes whichever of its two inputs is ready - its children's copies
    of piece `up`, while the window allows, or release `down` - in either
    order when both are, and waits only when neither is. The code takes the
    children's first (internode-generic.c); here either may go first, which
    covers it. A node that waited for its children while a release sat
    unconsumed would be as safe, and slow: a middle node would then consume
    release k + 1 no sooner than a round trip to the bottom of its subtree
    after release k, whatever D (design §5.8). This model checks safety and
    liveness, not speed.
  * A parent waits until every child position's counter has reached piece k,
    reads the slots, and sends piece k down: into each child's release slot,
    then an add to the child's release counter. The child sends its arrival for
    piece k + D only after copying release k out, so the parent never writes a
    release slot its child has not emptied.
  * Counters are cumulative over the team's life and compared as at least, not
    equal: within a collective a child may be up to D pieces ahead.
  * Collectives follow one another with different trees - a broadcast's is the
    reduction's rotated to its root - so a position's counter is fed by
    different children from one collective to the next. Each node adds to its
    per-position bases what its own tree says each position received.

Checked
  UP      the slot a parent reads holds the piece it waits for.
  DOWN    the release slot a node reads holds the piece it waits for.
  and, by the engine: no stuck node, and from every state the run can finish.

Designs
  design              as above.
Breakages
  window              a node sends piece k once it holds release k - D - 1:
                      one piece too many in flight (UP).
  summed              one arrival counter per node, summed over positions (UP).
  count_before_copy   a node counts a release consumed when it arrives and copies
                      it out only after sending what that count lets through:
                      its next arrival lets its parent overwrite the release it
                      has not copied (DOWN).

Waits are abstract: the wake on a counter is the region wait's, checked in
wait_word_model.py. Every counter add is fenced after its data (invariant 5),
so the model's stores are ordered as the code's are; runs are SC.

Usage: pipeline_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

ARR = lambda n, c: ('arr', n, c)      # node n's arrival counter for its child position c
REL = lambda n: ('rel', n)            # node n's release counter
SLOT = lambda n, c, j: ('slot', n, c, j)
RSLOT = lambda n, j: ('rslot', n, j)
FIN = ('$fin',)                        # nodes finished


def tree(nodes, root, fanout):
    """The reduction's k-ary tree over node indices, rotated to `root`:
    {node: (parent or None, position, [children])}."""
    out = {}
    for n in range(nodes):
        i = (n - root) % nodes
        parent = None if i == 0 else ((i - 1) // fanout + root) % nodes
        position = 0 if i == 0 else (i - 1) % fanout
        kids = [(fanout * i + c + 1 + root) % nodes for c in range(fanout)
                if fanout * i + c + 1 < nodes]
        out[n] = (parent, position, kids)
    return out


def children_in(M, n, piece, base, kid_pos, fanout, design):
    """Whether every child position of node n shows piece `piece` arrived."""
    if design == 'summed':
        need = sum(base[c] for c in kid_pos)
        return sum(M[ARR(n, c)] for c in range(fanout)) >= need + len(kid_pos) * (piece + 1)
    return all(M[ARR(n, c)] >= base[c] + piece + 1 for c in kid_pos)


def node(design, n, nodes, fanout, depth, collectives, pieces):
    """One node lead running `collectives` one after another.

    Its loop: whichever is ready of sending piece `up` - the window open and
    every child's copy of it arrived - and consuming release `down`, which it
    copies out and forwards down; either when both are; a wait while neither
    is. The root sends nothing up: its release of a piece is its own result,
    forwarded once every child's copy has arrived. `count_before_copy` counts a
    release consumed when it arrives and copies it out only after sending up
    whatever that count lets through.
    """
    window = depth + (1 if design == 'window' else 0)
    B = []
    base_arr = [0] * fanout
    base_rel = 0
    for x, (root, tagbase) in enumerate(collectives):
        t = tree(nodes, root, fanout)
        parent, position, kids = t[n]
        kid_pos = [t[k][1] for k in kids]
        p = f'n{n}c{x}_'
        after = f'@n{n}c{x + 1}_U' if x + 1 < len(collectives) else '@DONE'
        B += [(p + 'U', ('set', 'up', lambda L: 0, '@' + p + 'U1')),
              (p + 'U1', ('set', 'down', lambda L: 0, '@' + p + 'U2')),
              (p + 'U2', ('set', 'pend', lambda L: 0, '@' + p + 'LOOP')),
              (p + 'LOOP', ('br', lambda L, P=pieces: L['down'] >= P and not L['pend'],
                            after, '@' + p + 'UPQ'))]

        def wait_children(label, var, then, p=p, kid_pos=kid_pos, base=list(base_arr), n=n,
                          tagbase=tagbase):
            if not kid_pos:
                return [(label, ('br', lambda L: True, then, then))]
            W = [(label, ('await', lambda M, L, v=var:
                          children_in(M, n, L[v], base, kid_pos, fanout, design),
                          f'{label}K0'))]
            for i, c in enumerate(kid_pos):
                nxt = f'{label}K{i + 1}' if i + 1 < len(kid_pos) else then
                W += [(f'{label}K{i}', ('check', lambda M, L, c=c, v=var:
                                        None if M[SLOT(n, c, L[v] % depth)] == tagbase + L[v] else
                                        f'node {n} read {M[SLOT(n, c, L[v] % depth)]} from child '
                                        f'position {c}, waiting for piece {tagbase + L[v]}',
                                        '@' + nxt if not nxt.startswith('@') else nxt))]
            W[0] = (W[0][0], W[0][1][:2] + ('@' + W[0][1][2],))
            return W

        def forward(label, var, then, p=p, kids=kids, tagbase=tagbase):
            if not kids:
                return [(label, ('br', lambda L: True, then, then))]
            F = []
            for i, k in enumerate(kids):
                here = label if i == 0 else f'{label}{i}'
                nxt = f'@{label}{i + 1}' if i + 1 < len(kids) else then
                F += [(here, ('store', lambda L, k=k, v=var: RSLOT(k, L[v] % depth),
                              lambda L, v=var: tagbase + L[v], True, f'@{here}A')),
                      (f'{here}A', ('add', lambda L, k=k: REL(k), lambda L: 1, '_', nxt))]
            return F

        if parent is None:
            # The root: wait for every child's piece `down`, forward it, count it.
            B += [(p + 'UPQ', ('br', lambda L: True, '@' + p + 'RW', '@' + p + 'RW'))]
            B += wait_children(p + 'RW', 'down', '@' + p + 'FW')
            B += forward(p + 'FW', 'down', '@' + p + 'NX')
            B += [(p + 'NX', ('set', 'down', lambda L: L['down'] + 1, '@' + p + 'LOOP'))]
        else:
            # Whichever is ready: 1, send piece `up`; 2, consume release `down`;
            # 3, under count_before_copy, copy out the release it counted. None
            # ready, no alternative: the node waits for either.
            def choose(M, L, P=pieces, w=window, br=base_rel, base=list(base_arr),
                       kid_pos=kid_pos):
                alts = []
                if L['up'] < P and L['up'] < L['down'] + w and \
                        children_in(M, n, L['up'], base, kid_pos, fanout, design):
                    alts.append(({}, {'go': 1}, f'children hold piece {L["up"]}: send it up'))
                if L['pend']:
                    alts.append(({}, {'go': 3}, 'copy out the release counted'))
                elif L['down'] < P and M[REL(n)] >= br + L['down'] + 1:
                    alts.append(({}, {'go': 2}, f'release {L["down"]} in: consume it'))
                return alts

            B += [(p + 'UPQ', ('nd', choose, '@' + p + 'GO')),
                  (p + 'GO', ('br', lambda L: L['go'] == 1, '@' + p + 'GW', '@' + p + 'GO2')),
                  (p + 'GO2', ('br', lambda L: L['go'] == 2, '@' + p + 'GR', '@' + p + 'GL')),
                  (p + 'GW', ('set', 'go', lambda L: DEL, '@' + p + 'W')),
                  (p + 'GR', ('set', 'go', lambda L: DEL, '@' + p + 'RW')),
                  (p + 'GL', ('set', 'go', lambda L: DEL, '@' + p + 'LATE'))]
            B += wait_children(p + 'W', 'up', '@' + p + 'SEND')
            B += [(p + 'SEND', ('store', lambda L, pa=parent, pos=position: SLOT(pa, pos, L['up'] % depth),
                                lambda L, tb=tagbase: tb + L['up'], True, '@' + p + 'SEND1')),
                  (p + 'SEND1', ('add', lambda L, pa=parent, pos=position: ARR(pa, pos),
                                 lambda L: 1, '_', '@' + p + 'SEND2')),
                  (p + 'SEND2', ('set', 'up', lambda L: L['up'] + 1, '@' + p + 'LOOP')),
                  (p + 'RW', ('await', lambda M, L, br=base_rel: M[REL(n)] >= br + L['down'] + 1,
                              '@' + p + 'RX'))]
            if design == 'count_before_copy':
                B += [(p + 'RX', ('set', 'pend', lambda L: 1, '@' + p + 'RX1')),
                      (p + 'RX1', ('set', 'down', lambda L: L['down'] + 1, '@' + p + 'LOOP')),
                      (p + 'LATE', ('set', 'fw', lambda L: L['down'] - 1, '@' + p + 'LATE1')),
                      (p + 'LATE1', ('check', lambda M, L, tb=tagbase:
                                     None if M[RSLOT(n, L['fw'] % depth)] == tb + L['fw'] else
                                     f'node {n} read release {M[RSLOT(n, L["fw"] % depth)]}, '
                                     f'waiting for {tb + L["fw"]}', '@' + p + 'LF'))]
                B += forward(p + 'LF', 'fw', '@' + p + 'LATE2')
                B += [(p + 'LATE2', ('set', 'pend', lambda L: 0, '@' + p + 'LOOP'))]
            else:
                B += [(p + 'RX', ('check', lambda M, L, tb=tagbase:
                                  None if M[RSLOT(n, L['down'] % depth)] == tb + L['down'] else
                                  f'node {n} read release {M[RSLOT(n, L["down"] % depth)]}, '
                                  f'waiting for {tb + L["down"]}', '@' + p + 'FW')),
                      (p + 'LATE', ('br', lambda L: True, '@' + p + 'RW', '@' + p + 'RW'))]   # not reached
                B += forward(p + 'FW', 'down', '@' + p + 'NX')
                B += [(p + 'NX', ('set', 'down', lambda L: L['down'] + 1, '@' + p + 'LOOP'))]
        for c in kid_pos:
            base_arr[c] += pieces
        if parent is not None:
            base_rel += pieces
    B += [('DONE', ('gpost', lambda M, L: ({FIN: M[FIN] + 1}, {}), '@X')),
          ('X', ('done',))]
    return assemble(B)


def case(design, nodes, fanout, depth, roots, pieces):
    collectives = [(r, 100 * (x + 1)) for x, r in enumerate(roots)]
    threads = [node(design, n, nodes, fanout, depth, collectives, pieces) for n in range(nodes)]
    mem = {FIN: 0}
    for n in range(nodes):
        mem[REL(n)] = 0
        for j in range(depth):
            mem[RSLOT(n, j)] = 0
        for c in range(fanout):
            mem[ARR(n, c)] = 0
            for j in range(depth):
                mem[SLOT(n, c, j)] = 0
    name = (f'pipeline/{design} nodes={nodes} fanout={fanout} depth={depth} '
            f'roots={",".join(map(str, roots))} pieces={pieces}')
    return name, threads, mem, nodes


# (design, nodes, fanout, depth, roots, pieces, expect_ok)
CASES = [
    ('design', 3, 2, 2, (0,), 4, True),
    ('design', 3, 1, 2, (0,), 4, True),          # a chain: a node both parent and child
    ('design', 3, 2, 2, (0, 1), 3, True),        # two collectives, the second's tree rotated
    ('design', 4, 2, 2, (0, 2), 3, True),
    ('design', 3, 1, 1, (0, 2), 3, True),        # depth 1 is today's protocol
    ('design', 5, 2, 2, (0, 2, 4), 3, True),     # three collectives, three trees
    ('design', 5, 4, 2, (0, 3), 4, True),        # a wide parent
    ('window', 3, 2, 2, (0,), 4, False),
    ('summed', 3, 2, 2, (0,), 4, False),
    ('count_before_copy', 3, 1, 2, (0,), 4, False),   # a chain: a release overwritten
    ('count_before_copy', 3, 2, 2, (0,), 4, False),   # leaves only: the same
]


def main():
    want = select(sys.argv)
    ok = True
    for design, nodes, fanout, depth, roots, pieces, expect in CASES:
        name, threads, mem, n = case(design, nodes, fanout, depth, roots, pieces)
        if not want(name):
            continue
        res = explore(name, threads, mem, max_seconds=900, liveness_max_states=3_000_000,
                      done=lambda M, n=n: M[FIN] == n)
        ok &= report(res, expect_ok=expect)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
