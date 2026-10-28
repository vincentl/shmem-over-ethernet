# SPDX-License-Identifier: LGPL-3.0-or-later
"""T38's runs summarized: for each directory of measure.sh's output, each
tree (as built, the spin) and helper count, node 0's CPU a wake, the other
nodes' CPU a put received, every node's owners' idle CPU a second, and node
0's passes a wake - the median, the range and the count over the runs.
DIR,DIR... reads several directories of one configuration as one series.
usage: summarize.py SERIES..."""
import glob
import os
import re
import statistics
import sys

NODE = re.compile(r'wake-cost (idle|trickle) node (\d+): ([\d.]+) us of CPU in ([\d.]+) s '
                  r'\(([\d.]+) ms a second\), (\d+) wakes, (\d+) passes, (\d+) puts; '
                  r'([\d.]+) us a wake, ([\d.]+) us a put, ([\d.]+) passes a wake')


def spread(values):
    if not values:
        return 'none'
    return '%.1f (%.1f to %.1f, %d)' % (statistics.median(values), min(values), max(values),
                                        len(values))


for series in sys.argv[1:]:
    directories = series.split(',')
    print('==', ' and '.join(os.path.basename(d.rstrip('/')) for d in directories))
    medians = {}
    for helpers in (0, 2):
        for tree in ('built', 'spin'):
            wake, put, idle, passes, failed = [], [], [], [], 0
            for f in sorted(f for d in directories
                            for f in glob.glob('%s/%s-h%d-*.txt' % (d, tree, helpers))):
                text = open(f).read()
                if not text.rstrip().endswith('exit 0'):
                    failed += 1
                    continue
                for m in NODE.finditer(text):
                    phase, node = m.group(1), int(m.group(2))
                    if phase == 'idle':
                        idle.append(float(m.group(5)))
                    elif node == 0:
                        wake.append(float(m.group(9)))
                        passes.append(float(m.group(11)))
                    else:
                        put.append(float(m.group(10)))
            medians[(tree, helpers)] = (wake, put, idle)
            print('%-5s h%d: node 0 us a wake %s; other nodes us a put %s; idle ms a second '
                  '(a node) %s; passes a wake %s%s'
                  % (tree, helpers, spread(wake), spread(put), spread(idle), spread(passes),
                     '; %d runs failed' % failed if failed else ''))
    for helpers in (0, 2):
        b, s = medians[('built', helpers)], medians[('spin', helpers)]
        if b[0] and s[0]:
            print('h%d: the spin over as built - node 0 a wake %.1f times (worst pair %.1f), '
                  'other nodes a put %.1f times, idle %.1f times'
                  % (helpers, statistics.median(s[0]) / statistics.median(b[0]),
                     min(s[0]) / max(b[0]), statistics.median(s[1]) / statistics.median(b[1]),
                     statistics.median(s[2]) / statistics.median(b[2])))
