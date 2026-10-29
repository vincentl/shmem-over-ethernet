# SPDX-License-Identifier: LGPL-3.0-or-later
"""turnover-herd's lines (bench.sh's herd.txt) as tables: for each block size, writer
count and wake, every series' figure - the median, and the range beside it.
   summarize.py HERD.TXT [HERD.TXT...]"""
import re
import statistics
import sys

FIGURES = [('mib_per_s', 'MiB/s', '{:,.0f}'), ('kputs_per_s', 'thousand puts a second', '{:,.0f}'),
           ('sleeps_per_64kib', 'sleeps a 64 KiB', '{:.2f}'),
           ('agent_cpu_s_per_gib', 'agent CPU s a GiB', '{:.3f}'),
           ('waits_per_64kib', 'waits a 64 KiB', '{:.2f}'),
           ('hand_ons_per_64kib', 'hand-ons a 64 KiB', '{:.2f}'),
           ('passes_per_64kib', 'passes a 64 KiB', '{:.3f}'),
           ('slowest_share', 'slowest share', '{:.2f}'), ('fastest_share', 'fastest share', '{:.2f}'),
           ('longest_put_ms', 'longest put ms', '{:.1f}'),
           ('median_longest_put_ms', 'median longest put ms', '{:.1f}'),
           ('segment_kib', 'KiB a segment', '{:.0f}')]

runs = {}
bad = []
for path in sys.argv[1:]:
    for line in open(path):
        if not line.startswith('series='):
            continue
        f = dict(re.findall(r'(\w+)=([^\s]+)', line))
        if f.get('exit') != '0' or 'mib_per_s' not in f:
            bad.append(line.strip())
            continue
        f['kputs_per_s'] = str(float(f['mib_per_s']) * 1048576 / int(f['bytes']) / 1000)
        key = (int(f['bytes']), int(f['writers']), f.get('variant', f['wake']))
        runs.setdefault(key, []).append(f)

ORDER = ['all', 'hand-on', 'room', 'wake-4', 'wake-4-hand-on']
wakes = sorted({k[2] for k in runs}, key=lambda v: ORDER.index(v) if v in ORDER else len(ORDER))
for b in sorted({k[0] for k in runs}, reverse=True):
    writers = sorted({k[1] for k in runs if k[0] == b})
    print(f'\n## {b // 1024} KiB blocks\n' if b >= 1024 else f'\n## {b} B blocks\n')
    print('| figure | wake | ' + ' | '.join(str(w) for w in writers) + ' |')
    print('|---|---|' + '---|' * len(writers))
    for name, label, fmt in FIGURES:
        for wake in wakes:
            cells = []
            for w in writers:
                vals = [float(f[name]) for f in runs.get((b, w, wake), []) if name in f]
                if not vals:
                    cells.append('-')
                    continue
                m = statistics.median(vals)
                cells.append(fmt.format(m) + (f' ({fmt.format(min(vals))}-{fmt.format(max(vals))})'
                                              if len(vals) > 1 else ''))
            print(f'| {label} | {wake} | ' + ' | '.join(cells) + ' |')
    for wake in wakes[1:]:
        print(f'| {wake} over all, MiB/s | | ' + ' | '.join(
            '{:.2f}'.format(statistics.median([float(f['mib_per_s']) for f in runs[(b, w, wake)]]) /
                            statistics.median([float(f['mib_per_s']) for f in runs[(b, w, 'all')]]))
            if (b, w, wake) in runs and (b, w, 'all') in runs else '-' for w in writers) + ' |')
if bad:
    print('\nruns without a figure:')
    for line in bad:
        print('  ' + line)
