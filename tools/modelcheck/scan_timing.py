#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Timing beside scan_model.py (the rule of performance-claims-need-timing):
how long a scan takes, in network hops, under the two designs the brief
weighs (docs/prompts/done/phase1-step10-slice1-prompt.md).

  members  every member at its team rank's place in a pre-order k-ary tree
           over the ranks, on its own thread (scan_model.py).
  runs     design §5.8's sketch: maximal runs of consecutive ranks on one node,
           a pre-order k-ary tree over the runs, every run of a node served by
           that node's one lead thread.
  hierarchical
           the large-cluster playbook's C9 (hierarchical_scan_model.py,
           docs/design/hierarchical-scan-proposal.md, 2026-11-02): where every
           node is one run - a block layout - the members arrive as a
           reduction's do, the lead sums its members' sources and scans the
           nodes' totals with the other leads over a pre-order tree of the
           runs at the fan-out's ceiling (8, collect's leads' scan), then
           writes every member's prefix from its node's and releases. Under a
           cyclic layout a node is many runs and the design does not apply:
           the members' tree stays.

Units: one network hop is 1 (a Pi's one-way trip is about 77 us). A hop
between two PEs of one node is `local` (a futex wake through shared memory,
about 4 us on a Pi: 0.05). Every message leaving a node goes through that
node's one engine, which spends `send` on each (0.02). A lead serving runs
spends `serve` on each run it moves a step (0.01). The team is the world, of
N nodes and L PEs each, laid out in blocks (Slurm's block distribution) or
cyclically.

Usage: scan_timing.py            the table
       scan_timing.py check      the proposal's claims, pinned (model.scan_timing)
"""
import heapq
import sys


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


def simulate(count, node_of, server_of, k, local=0.05, send=0.02, serve=0.0):
    """Up then down over a pre-order tree of `count` places. node_of(place) is
    its node, server_of(place) the thread serving it: a place's step starts
    once its inputs are in and its thread is free."""
    parent, kids = tree(count, k)
    thread_free, engine_free = {}, {}

    def hop(a, b, at):
        if node_of(a) == node_of(b):
            return at + local
        n = node_of(a)
        start = max(at, engine_free.get(n, 0.0))
        engine_free[n] = start + send
        return start + send + 1.0

    def step(place, ready):
        s = server_of(place)
        start = max(ready, thread_free.get(s, 0.0))
        thread_free[s] = start + serve
        return start + serve

    # Up: leaves first, in order of readiness.
    up_arrive = {p: [] for p in range(count)}
    up_done = {}
    heap = [(0.0, p) for p in range(count) if not kids[p]]
    heapq.heapify(heap)
    while heap:
        ready, p = heapq.heappop(heap)
        t = step(p, ready)
        up_done[p] = t
        q = parent[p]
        if q is not None:
            up_arrive[q].append(hop(p, q, t))
            if len(up_arrive[q]) == len(kids[q]):
                heapq.heappush(heap, (max(up_arrive[q]), q))
    # Down: the root first.
    finish = 0.0
    heap = [(up_done[0], 0)]
    while heap:
        ready, p = heapq.heappop(heap)
        t = step(p, ready)
        finish = max(finish, t)
        for c in kids[p]:
            heapq.heappush(heap, (hop(p, c, t), c))
    return finish


def members_design(nodes, per_node, layout, k):
    count = nodes * per_node
    node = (lambda r: r // per_node) if layout == 'block' else (lambda r: r % nodes)
    return simulate(count, node, lambda r: r, k)


def runs_design(nodes, per_node, layout, k):
    count = nodes * per_node
    node = (lambda r: r // per_node) if layout == 'block' else (lambda r: r % nodes)
    runs = []                   # each run's node
    for r in range(count):
        if r == 0 or node(r) != node(r - 1):
            runs.append(node(r))
    return simulate(len(runs), lambda q: runs[q], lambda q: runs[q], k, serve=0.01)


def hierarchical_design(nodes, per_node, layout, k=8, local=0.05, member=0.002):
    """C9: under a block layout, the arrival (a hop within the node), the
    lead's sum of its members' sources (`member` each, about 0.15 us a member
    of a small scan at a Pi's hop), the leads' scan over the nodes at the
    ceiling's fan-out, the lead's prefixes (`member` each) and the release
    (a hop within the node). None under a cyclic layout."""
    if layout != 'block':
        return None
    leads = simulate(nodes, lambda q: q, lambda q: q, k, serve=0.01)
    return local + per_node * member + leads + per_node * member + local


def depth(nodes, k):
    d, span = 0, 1
    while span < nodes:
        span *= k
        d += 1
    return d


def main():
    k = 4
    print(f'k = {k} for the members\' and the runs\' trees, 8 for the hierarchical design\'s '
          'leads; a network hop is 1, a hop within a node 0.05, a send 0.02, '
          'a lead serving one run one step 0.01')
    print(f'{"layout":7} {"N":>5} {"L":>3} {"members":>8} {"runs":>8} {"hierarchical":>12}  '
          f'{"2 x ceil(log_4 N)":>17} {"2 x ceil(log_8 N)":>17}')
    for layout in ('block', 'cyclic'):
        for per_node in (4, 64, 256):
            for nodes in (4, 16, 64, 256, 512, 1024, 4096):
                if nodes * per_node > 140000:
                    continue
                m = members_design(nodes, per_node, layout, k)
                r = runs_design(nodes, per_node, layout, k)
                h = hierarchical_design(nodes, per_node, layout)
                hs = f'{h:12.2f}' if h is not None else f'{"-":>12}'
                print(f'{layout:7} {nodes:5} {per_node:3} {m:8.2f} {r:8.2f} {hs}  '
                      f'{2 * depth(nodes, k):17} {2 * depth(nodes, 8):17}')


def check():
    """The hierarchical scan proposal's claims, in hops: under a block layout
    the hierarchical design is faster than the members' tree at every node
    count from 4 and every PEs-a-node count from 4, by at least 1.35 from 64
    nodes; at 512 nodes x 256 PEs the members' tree takes more than 12 hops
    where the hierarchical design takes fewer than 8, 1.5 times as fast or
    better; and under a cyclic layout, where the design does not apply, the
    members' tree at 512 x 256 takes more than 40."""
    ok = True

    def pin(cond, what):
        nonlocal ok
        print(('ok   ' if cond else 'FAIL ') + what)
        ok &= cond
    for per_node in (4, 64, 256):
        for nodes in (4, 16, 64, 256, 512):
            if nodes * per_node > 140000:
                continue
            m = members_design(nodes, per_node, 'block', 4)
            h = hierarchical_design(nodes, per_node, 'block')
            pin(h < m, f'block {nodes} x {per_node}: hierarchical {h:.2f} < members {m:.2f}')
            if nodes >= 64:
                pin(m / h >= 1.35, f'block {nodes} x {per_node}: ratio {m / h:.2f} >= 1.35')
    m = members_design(512, 256, 'block', 4)
    h = hierarchical_design(512, 256, 'block')
    pin(m > 12 and h < 8 and m / h > 1.5,
        f'block 512 x 256: members {m:.2f} > 12, hierarchical {h:.2f} < 8, ratio {m / h:.2f} > 1.5')
    c = members_design(512, 256, 'cyclic', 4)
    pin(c > 40, f'cyclic 512 x 256: members {c:.2f} > 40 - the design does not apply there')
    return ok


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'check':
        sys.exit(0 if check() else 1)
    main()


