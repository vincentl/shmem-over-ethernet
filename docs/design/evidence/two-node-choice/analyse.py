#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""P4's two-node runs (two-node.sh, two-node-again.sh): for each size, PEs a
node and arm, the slowest PE's median in each series, and their median over the
series; Träff's against the tree - the ratio of those medians, and the least
and the most of the series' own ratios; the tree with the link measured against
the tree on the link set, where a run has both; and what the arm as built took,
a letter a series (t the tree, c Träff's). Every call of the tree's arms must
have taken the tree and every call of Träff's arm Träff's, every PE alike, or a
line says so.
  analyse.py DIRECTORY [SERIES ...]
SERIES, the series run on pi-003 and pi-004 (two-node.sh's 4), are shown apart
from the rest, which ran on pi-001 and pi-002."""
import os
import re
import statistics
import sys

LINE = re.compile(r'two-node pes=(\d+) bytes=(\d+) slowest_median_us=([\d.]+) '
                  r'fastest_median_us=([\d.]+) p10_us=([\d.]+) p90_us=([\d.]+) '
                  r'algorithm=(-?\d+)(\(differs\))? window=(-?\d+)(\(differs\))? children=(-?\d+)')
NAME = re.compile(r'(tree-measured|tree|traff|built)-2x(\d+)-(\d+)\.txt$')
WANT = {'tree': 0, 'tree-measured': 0, 'traff': 1}


def main(directory, second):
    runs = {}               # (arm, PEs a node, series) -> {bytes: (us, algorithm)}
    links = []
    for name in sorted(os.listdir(directory)):
        m = NAME.match(name)
        if not m:
            continue
        arm, per_node, series = m.group(1), int(m.group(2)), int(m.group(3))
        rows = {}
        for line in open(os.path.join(directory, name)):
            if 'OSOE_LINK_' in line:
                links.append(f'{name}: {line.strip()}')
            r = LINE.match(line)
            if not r:
                continue
            n, us, algorithm = int(r.group(2)), float(r.group(3)), int(r.group(7))
            if r.group(8) or r.group(10):
                print(f'{name}: {n} bytes: the PEs read different shapes')
            if arm in WANT and algorithm != WANT[arm]:
                print(f'{name}: {n} bytes took algorithm {algorithm}, not {WANT[arm]}')
            rows[n] = (us, algorithm)
        runs[(arm, per_node, series)] = rows
    arms = {k[0] for k in runs}
    every = sorted({k[2] for k in runs})
    groups = [('pi-001,pi-002', [s for s in every if s not in second])]
    if any(s in every for s in second):
        groups.append(('pi-003,pi-004', [s for s in every if s in second]))
    for per_node in sorted({k[1] for k in runs}):
        for hosts, chosen in groups:
            def cells(arm, n):
                return [runs[(arm, per_node, s)][n] for s in chosen
                        if n in runs.get((arm, per_node, s), {})]
            print(f'\n{2 * per_node} PEs on {hosts}, {per_node} a node: the slowest PE\'s median, '
                  f'us, the median of series {",".join(map(str, chosen))}')
            head = f'{"bytes":>9} {"tree":>9} {"traff":>9} {"traff/tree":>10} {"series":>11}'
            if 'tree-measured' in arms:
                head += f' {"measured":>9} {"/tree":>6}'
            if 'built' in arms:
                head += f' {"built":>9} {"took":>4}'
            print(head)
            for n in sorted(runs.get(('tree', per_node, chosen[0]), {})):
                tree, traff = cells('tree', n), cells('traff', n)
                t = statistics.median(us for us, a in tree)
                c = statistics.median(us for us, a in traff)
                ratios = [b[0] / a[0] for a, b in zip(tree, traff)]
                line = (f'{n:9d} {t:9.1f} {c:9.1f} {c / t:10.3f} '
                        f'{min(ratios):5.3f}-{max(ratios):5.3f}')
                if 'tree-measured' in arms:
                    m = statistics.median(us for us, a in cells('tree-measured', n))
                    line += f' {m:9.1f} {m / t:6.3f}'
                if 'built' in arms:
                    built = cells('built', n)
                    b = statistics.median(us for us, a in built)
                    took = ''.join('tc'[a] if a in (0, 1) else '?' for us, a in built)
                    line += f' {b:9.1f} {took:>4}'
                print(line)
    if links:
        print('\nThe links measured (OSOE_TEST_PRINT_LINK):')
        for line in links:
            print(f'  {line}')


if __name__ == '__main__':
    main(sys.argv[1], {int(s) for s in sys.argv[2:]})
