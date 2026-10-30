#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""P4's model-side numbers (docs/decisions/2026-10-29-two-node-choice.md), from
tools/modelcheck/shape_timing.py as P4 left it, whose cost() charges a tree's
partial last piece the larger of its own bytes on the busiest link and the
window's pace less what its shorter loop gives back (`offset` below) and, with
LAST_PIECE_WHOLE, a whole period as before P4 (`whole`):
  1. the cost of a partial last piece against the simulation, step 2's fit:
     for sizes of whole pieces plus r bytes, the closed form's increment over
     the size of whole pieces against simulate()'s, on the Pi's link and on the
     grid's three others, for `whole`, `offset` and the two other forms
     considered - `link`, its own bytes on the busiest link alone, and
     `period`, the period's formula at its bytes;
  2. what `offset` changes against `whole` on the shape grid and the places
     grid, and the changed shapes on up to 33 nodes judged by the simulation.
  model-checks.py   (any directory; about two minutes)"""
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', '..', 'tools', 'modelcheck'))
import shape_timing as st  # noqa: E402

S = st.PIECE
LINKS = [st.PI_LINK, st.LOOPBACK_LINK, (1000000, 50000, 12000000), (5000, 500, 25000000000)]
cost_now = st.cost


def terms(op, nodes, k, W, n, s, h, o, B):
    first, loops, beta = st.tree_terms(op, nodes, k)
    pieces = (n + s - 1) // s
    t = (min(n, s) * 10**9 + B - 1) // B
    fill = max(a * h + b * t + m * o for a, b, m in first)
    loop = max(a * h + b * t + m * o for a, b, m in loops)
    period = max(beta * t, (loop + W - 1) // W)
    last = n - (pieces - 1) * s
    tl = (last * 10**9 + B - 1) // B
    loop_last = max(a * h + b * tl + m * o for a, b, m in loops)
    return fill, period, pieces, beta, tl, loop, loop_last, last


def form(name):
    def cost(op, nodes, k, W, n, s, h, o, B):
        st.LAST_PIECE_WHOLE = name == 'whole'
        c = cost_now(op, nodes, k, W, n, s, h, o, B)
        st.LAST_PIECE_WHOLE = False
        if name in ('whole', 'offset'):
            return c
        fill, period, pieces, beta, tl, loop, loop_last, last = terms(op, nodes, k, W, n, s, h,
                                                                      o, B)
        if pieces == 1 or last == s:
            return c
        if name == 'period':
            tail = max(beta * tl, (loop_last + W - 1) // W)
        else:
            tail = beta * tl
        return fill + (pieces - 2) * period + tail
    return cost


def increments():
    print('1. A partial last piece: |closed - simulated| increment, as a share of the call')
    forms = ('whole', 'offset', 'link', 'period')
    for link in LINKS:
        h, o, B = link[0] / 1000, link[1] / 1000, link[2] / 1e6
        errors = {f: [] for f in forms}
        for op in ('reduce', 'broadcast'):
            for nodes in (2, 3, 4, 8, 16):
                for k in (2, 4, 8):
                    for W in (2, 4, 8):
                        for j in (1, 4, 16):
                            base = j * S
                            before = st.simulate(op, nodes, k, W, base, S, h, o, B,
                                                 measure='completion')
                            for r in (1, 8192, 16383):
                                after = st.simulate(op, nodes, k, W, base + r, S, h, o, B,
                                                    measure='completion')
                                for f in forms:
                                    c = form(f)
                                    d = (c(op, nodes, k, W, base + r, S, *link) -
                                         c(op, nodes, k, W, base, S, *link)) / 1000
                                    errors[f].append(abs(d - (after - before)) / before)
        print(f'  link {link}: {len(errors["offset"])} cases')
        for f in forms:
            v = sorted(errors[f])
            print(f'    {f:7s} median {statistics.median(v):.4f}  p90 {v[int(len(v) * 0.9)]:.4f}'
                  f'  worst {v[-1]:.4f}')


def simulated(op, nodes, shape, n, link):
    algorithm, k, W = shape
    h, o, B = link[0] / 1000, link[1] / 1000, link[2] / 1e6
    if algorithm == 'circulant':
        return st.simulate_circulant(nodes, n, S, W, h, o, B)
    return st.simulate(op, nodes, k, W, n, S, h, o, B, measure='completion')


def grids():
    print('\n2. What charging the last piece its bytes changes on the grids')
    st.LAST_PIECE_WHOLE = True
    whole, whole_places = st.grid_lines(), st.places_grid_lines()
    st.LAST_PIECE_WHOLE = False
    now, now_places = st.grid_lines(), st.places_grid_lines()
    changed = [(a.split(), b.split()) for a, b in zip(whole, now) if a != b]
    shapes = [(a, b) for a, b in changed if a[10:13] != b[10:13]]
    algorithms = [(a, b) for a, b in shapes if a[10] != b[10]]
    sizes = sorted({a[2] for a, b in changed})
    print(f'  shape grid: {len(changed)} of {len(whole) - 1} lines, at {", ".join(sizes)} bytes; '
          f'{len(shapes)} change their shape, {len(algorithms)} their algorithm '
          f'({", ".join(sorted({a[10] + " to " + b[10] for a, b in algorithms}))})')
    by_link = {}
    for a, b in algorithms:
        by_link.setdefault(a[3], []).append(int(a[1]))
    for hop, nodes in sorted(by_link.items()):
        print(f'    hop {hop} ns: on {", ".join(str(n) for n in sorted(set(nodes)))} nodes')
    places = [(a.split(), b.split()) for a, b in zip(whole_places, now_places) if a != b]
    ways = sum(1 for a, b in places if a[12] != b[12])
    columns = [sum(1 for a, b in places if a[13 + i] != b[13 + i]) for i in range(4)]
    print(f'  places grid: {len(places)} of {len(whole_places) - 1} lines; the way in {ways}; '
          f'the members, totals, scan and exchange costs in {columns}')
    ratios = []
    for a, b in shapes:
        nodes = int(a[1])
        if nodes > 33:
            continue
        link = (int(a[3]), int(a[4]), int(a[5]))
        old = (a[10], int(a[11]), int(a[12]))
        new = (b[10], int(b[11]), int(b[12]))
        n = int(a[2])
        ratios.append((simulated(a[0], nodes, new, n, link) /
                       simulated(a[0], nodes, old, n, link), a[0], nodes, link[0], old, new))
    ratios.sort()
    faster = sum(1 for r in ratios if r[0] < 0.999)
    slower = sum(1 for r in ratios if r[0] > 1.001)
    print(f'  the {len(ratios)} changed shapes on up to 33 nodes, by the simulation: the new one '
          f'faster in {faster}, slower in {slower}, the same in {len(ratios) - faster - slower}; '
          f'the geometric mean of new over old {statistics.geometric_mean(r[0] for r in ratios):.3f}')
    for r in ratios[:3] + ratios[-3:]:
        print(f'    {r[0]:.3f}  {r[1]} on {r[2]} nodes, hop {r[3]} ns: {r[4]} to {r[5]}')


if __name__ == '__main__':
    increments()
    grids()
