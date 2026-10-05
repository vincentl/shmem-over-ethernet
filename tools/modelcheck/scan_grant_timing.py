#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Timing beside scan_pipeline_model.py and grant_timing.py (the rule of
performance-claims-need-timing): a large scan's loop, osoe_internode_generic_scan's
(every member at its rank's place in a pre-order k-ary tree over the ranks, D
pieces in flight, a piece up once every child's total is in, down only after
up), with each member's slots its team number's own (before the team
redesign's step E: a child sends its first total as soon as it has it) or its
lane (step E's second part: a child's first total up waits for its parent's
grant, which the parent sends as it enters the scan).

Every member is its own node here, a hop h = 1 between any two, and c per piece
step. The members enter at once, or one enters late - the root, a member with
children, or a leaf: a parent late by L hops grants only as it enters, and its
children's first totals arrive a round trip after that, where its own slots
had them waiting. Written by the second part's adversarial read, which found
that case; grant_timing.py has the same for the reductions.
"""
import heapq


def children(a, b, k):
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


def tree(count, k):
    parent, kids = {}, {}

    def walk(a, b, p):
        parent[a] = p
        kids[a] = [c for c, _ in children(a, b, k)]
        for c, e in children(a, b, k):
            walk(c, e, a)
    walk(0, count, None)
    return parent, kids


def run(M, k, D, pieces, entry, grant, h=1.0, c=0.02):
    parent, kids = tree(M, k)
    events, seq = [], 0

    def at(t, f, *a):
        nonlocal seq
        heapq.heappush(events, (t, seq, f, a))
        seq += 1
    arrived = {i: [0] * len(kids[i]) for i in range(M)}
    downs_in = [0] * M
    up = [0] * M
    down = [0] * M
    busy = [0.0] * M
    done = [None] * M
    granted_at = [0.0] * M
    for i in range(M):
        if grant and parent[i] is not None:
            granted_at[i] = entry[parent[i]] + h

    def step(i, now):
        now = max(now, busy[i], entry[i])
        root = parent[i] is None
        while down[i] < pieces:
            sending = up[i] < pieces and up[i] < down[i] + D
            ready_up = sending and all(a > up[i] for a in arrived[i])
            if ready_up and (root or now >= granted_at[i]):
                now += c
                if not root:
                    p = parent[i]
                    at(now + h, child_in, p, kids[p].index(i))
                up[i] += 1
                continue
            if down[i] < up[i] and (root or downs_in[i] > down[i]):
                now += c
                for ch in kids[i]:
                    at(now + h, down_in, ch)
                down[i] += 1
                continue
            if ready_up and now < granted_at[i]:
                at(granted_at[i], step, i)
            break
        busy[i] = now
        if down[i] == pieces and done[i] is None:
            done[i] = now

    def child_in(p, pos, now):
        arrived[p][pos] += 1
        step(p, now)

    def down_in(i, now):
        downs_in[i] += 1
        step(i, now)
    for i in range(M):
        at(entry[i], step, i)
    while events:
        t, _, f, a = heapq.heappop(events)
        f(*a, t)
    return max(done)


if __name__ == '__main__':
    print('   M k D pieces  late member (rank, by)   | no grant  grant  extra')
    for M, k in ((16, 4), (64, 4), (256, 4)):
        parent, kids = tree(M, k)
        interior = next(i for i in range(1, M) if kids[i])
        leaf = M - 1
        for pieces in (1, 4, 16):
            for who, by in ((None, 0), (0, 3.0), (interior, 0.5), (interior, 1.0),
                            (interior, 2.0), (interior, 3.0), (leaf, 3.0)):
                entry = [0.0] * M
                if who is not None:
                    entry[who] = by
                a = run(M, k, 2, pieces, entry, False)
                b = run(M, k, 2, pieces, entry, True)
                label = 'none' if who is None else f'rank {who} by {by}'
                print(f'{M:4d} {k} 2 {pieces:4d}   {label:22s} | {a:7.2f} {b:7.2f} {b - a:+6.2f}')
