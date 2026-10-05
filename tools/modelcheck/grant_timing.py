#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Timing beside lane_model.py (the rule of performance-claims-need-timing): a
pipelined reduction through a k-ary tree of node leads, with the slots a team
number's own (before step E: a child pushes as soon as it has a piece) or a
lane of the parent's thread (step E, docs/design/team-resources-proposal.md
§4, "What it costs": a child's first piece up waits for the parent's grant,
which the parent sends as it enters the collective).

The loop is internode-generic.c's in_pieces: a lead sends piece `up` once every
child's copy is in and fewer than D pieces are up without their release; it
takes release `down` when it is in, forwards it, then counts it. A hop costs h;
a lead spends t0 between entering and holding its node's first partial result
(the intranode phase), and c per piece combining or copying. Times in units of h.

The first table has every lead enter at once; the second has one lead enter
late - the root, a lead with children, or a leaf - which a child's grant waits
for: a parent late by L hops grants as it enters, the grant takes a hop and the
piece another, where the team number's own slots had the piece waiting for it.
"""
import heapq, sys

def run(nodes, k, D, pieces, h, t0, c, grant, entry=None):
    parent = [None] + [(i - 1) // k for i in range(1, nodes)]
    children = [[j for j in range(k * i + 1, min(k * i + k + 1, nodes))] for i in range(nodes)]
    events, seq = [], 0
    def at(t, f, *a):
        nonlocal seq
        heapq.heappush(events, (t, seq, f, a)); seq += 1
    arrived = [[0] * len(children[i]) for i in range(nodes)]   # pieces in from each child
    released = [0] * nodes                                      # releases in from the parent
    up = [0] * nodes; down = [0] * nodes
    if entry is None:
        entry = [0.0] * nodes        # every lead enters at 0
    ready_at = [entry[i] + t0 for i in range(nodes)]   # when the node's own partial exists
    granted_at = [0.0] * nodes       # when the parent's grant is held
    busy_until = [0.0] * nodes
    done = [None] * nodes
    if grant:
        for i in range(1, nodes):
            granted_at[i] = entry[parent[i]] + h   # the parent grants as it enters
    def step(i, now):
        # the lead's loop: one action at a time, each costing c
        now = max(now, busy_until[i], ready_at[i])
        root = parent[i] is None
        while down[i] < pieces:
            sending = (not root) and up[i] < pieces and up[i] < down[i] + D
            if sending and all(a > up[i] for a in arrived[i]) and now >= granted_at[i]:
                now += c
                p, pos = parent[i], children[parent[i]].index(i)
                at(now + h, child_in, p, pos)
                up[i] += 1
                continue
            have = all(a > down[i] for a in arrived[i]) if root else released[i] > down[i]
            if have:
                now += c
                for ch in children[i]:
                    at(now + h, release_in, ch)
                down[i] += 1
                continue
            if sending and all(a > up[i] for a in arrived[i]) and now < granted_at[i]:
                at(granted_at[i], step, i)      # wake when the grant lands
            break
        busy_until[i] = now
        if down[i] == pieces and done[i] is None:
            done[i] = now
    def child_in(p, pos, now):
        arrived[p][pos] += 1; step(p, now)
    def release_in(i, now):
        released[i] += 1; step(i, now)
    for i in range(nodes):
        at(ready_at[i], step, i)
    while events:
        t, _, f, a = heapq.heappop(events)
        f(*a, t)
    return max(done)

if __name__ == '__main__':
    h, c = 1.0, 0.02
    print("nodes  k  D pieces   t0/h | today   granted   extra hops")
    for nodes, k in ((4, 4), (5, 4), (21, 4), (85, 4), (500, 4), (4096, 4)):
        for pieces in (1, 8, 64):
            for t0 in (0.0, 0.3, 1.0):
                a = run(nodes, k, 2, pieces, h, t0, c, False)
                b = run(nodes, k, 2, pieces, h, t0, c, True)
                print(f"{nodes:5d} {k:2d}  2 {pieces:6d}   {t0:4.1f} | {a:6.2f}   {b:6.2f}   {b - a:+.2f}")
    print()
    print("nodes  k  D pieces   t0/h  late lead, by | today   granted   extra hops")
    for nodes, k in ((21, 4), (85, 4), (500, 4)):
        interior, leaf = 1, nodes - 1
        for pieces in (1, 8, 64):
            for t0 in (0.3, 1.0):
                for who, name, by in ((0, "root", 3.0), (interior, "middle", 0.5),
                                      (interior, "middle", 1.0), (interior, "middle", 2.0),
                                      (interior, "middle", 3.0), (leaf, "leaf", 3.0)):
                    entry = [0.0] * nodes
                    entry[who] = by
                    a = run(nodes, k, 2, pieces, h, t0, c, False, entry)
                    b = run(nodes, k, 2, pieces, h, t0, c, True, entry)
                    print(f"{nodes:5d} {k:2d}  2 {pieces:6d}   {t0:4.1f}  {name:6s} {by:4.1f} | "
                          f"{a:6.2f}   {b:6.2f}   {b - a:+.2f}")
