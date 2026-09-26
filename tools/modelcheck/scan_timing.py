#!/usr/bin/env python3
"""Timing beside scan_model.py (the rule of performance-claims-need-timing):
how long a scan takes, in network hops, under the two designs the brief
weighs (docs/prompts/done/phase1-step10-slice1-prompt.md).

  members  every member at its team rank's place in a pre-order k-ary tree
           over the ranks, on its own thread (scan_model.py).
  runs     design §5.8's sketch: maximal runs of consecutive ranks on one node,
           a pre-order k-ary tree over the runs, every run of a node served by
           that node's one lead thread.

Units: one network hop is 1 (a Pi's one-way trip is about 77 us). A hop
between two PEs of one node is `local` (a futex wake through shared memory,
about 4 us on a Pi: 0.05). Every message leaving a node goes through that
node's one engine, which spends `send` on each (0.02). A lead serving runs
spends `serve` on each run it moves a step (0.01). The team is the world, of
N nodes and L PEs each, laid out in blocks (Slurm's block distribution) or
cyclically.

Usage: scan_timing.py
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


def main():
    k = 4
    print(f'k = {k}; a network hop is 1, a hop within a node 0.05, a send 0.02, '
          'a lead serving one run one step 0.01')
    print(f'{"layout":7} {"N":>5} {"L":>3} {"members":>8} {"runs":>8}  '
          f'{"2 x ceil(log_k N)":>17}')
    for layout in ('block', 'cyclic'):
        for per_node in (4, 64):
            for nodes in (4, 16, 64, 256, 1024, 4096):
                if nodes * per_node > 70000:
                    continue
                m = members_design(nodes, per_node, layout, k)
                r = runs_design(nodes, per_node, layout, k)
                depth, span = 0, 1
                while span < nodes:
                    span *= k
                    depth += 1
                print(f'{layout:7} {nodes:5} {per_node:3} {m:8.2f} {r:8.2f}  {2 * depth:17}')


if __name__ == '__main__':
    main()
