#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""H21's analysis (h21.sh): each line's median of three runs, its spread
((max - min) / median) and `tree` over `link`; OSU's reduce at 4 x 1 against
the event simulation of tools/modelcheck/shape_timing.py at the Pi's link -
h 86 us, o 7 us, B 116 bytes/us, the window the chooser gives four nodes -
under both ways for the links to share ('sender', the reference, and
'connection'): measured over simulated, Träff's for `link` and the chooser's
tree for `tree`. With STEP3, step 3's OSU 4 x 1 lines the same way, to check
the method against the record's 11 to 22%.
Usage: analyse-h21.py REPO OUTDIR [STEP3]"""
import os, re, statistics, sys

REPO, OUT = sys.argv[1], sys.argv[2]
STEP3 = sys.argv[3] if len(sys.argv) > 3 else None
sys.path.insert(0, os.path.join(REPO, 'tools', 'modelcheck'))
import shape_timing as st

BENCH = re.compile(r'(\w+) pes=\d+ bytes=(\d+) median_us=([\d.]+) fastest_us=([\d.]+)')


def osu(path):
    rows = {}
    for line in open(path):
        p = line.split()
        if len(p) == 2 and p[0].isdigit():
            rows[int(p[0])] = float(p[1])
    return rows


def bench(path):
    rows = {}
    for line in open(path):
        m = BENCH.match(line)
        if m:
            rows[(m[1], int(m[2]))] = (float(m[3]), float(m[4]))
    return rows


def median(values):
    m = statistics.median(values)
    return m, (max(values) - min(values)) / m


def simulated(n):
    N = 4
    window = 4 * (9 // len(st.circulant_skips(N)))
    _, k, W = st.choose_shape('reduce', N, n, st.PI_LINK, 8, 4, 1024)
    sim = {}
    for links in ('sender', 'connection'):
        sim['link', links] = st.simulate_circulant(N, n, st.PIECE, window, 86, 7, 116, links=links)
        sim['tree', links] = st.simulate('reduce', N, k, W, n, st.PIECE, 86, 7, 116, links=links)
    return sim


def osu_table(title, runs):
    """runs: {side: [ {bytes: us}, ... ]}"""
    print(title)
    print(f'{"bytes":>8} {"link":>9} {"spread":>6} {"tree":>9} {"spread":>6} {"tree/link":>9}'
          f' {"link/sim":>8} {"(conn)":>6} {"tree/sim":>8} {"(conn)":>6}')
    sizes = sorted(set.intersection(*(set(r) for side in runs.values() for r in side)))
    for n in sizes:
        sim = simulated(n)
        (lm, ls), (tm, ts) = (median([r[n] for r in runs[side]]) for side in ('link', 'tree'))
        print(f'{n:>8} {lm:>9.1f} {ls:>6.3f} {tm:>9.1f} {ts:>6.3f} {tm / lm:>9.3f}'
              f' {lm / sim["link", "sender"]:>8.3f} {lm / sim["link", "connection"]:>6.3f}'
              f' {tm / sim["tree", "sender"]:>8.3f} {tm / sim["tree", "connection"]:>6.3f}')
    print()


def bench_table(title, runs):
    print(title)
    print(f'{"line":>18} {"link":>9} {"spread":>6} {"fastest":>8} {"tree":>9} {"spread":>6}'
          f' {"fastest":>8} {"tree/link":>9}')
    for key in sorted(set.intersection(*(set(r) for side in runs.values() for r in side))):
        if key[0] not in ('reduce', 'broadcast'):
            continue
        out = []
        for side in ('link', 'tree'):
            m, s = median([r[key][0] for r in runs[side]])
            out.append((m, s, min(r[key][1] for r in runs[side])))
        (lm, ls, lf), (tm, ts, tf) = out
        print(f'{key[0] + " " + str(key[1]):>18} {lm:>9.1f} {ls:>6.3f} {lf:>8.1f} {tm:>9.1f}'
              f' {ts:>6.3f} {tf:>8.1f} {tm / lm:>9.3f}')
    print()


def path(*parts):
    return os.path.join(*parts)


if STEP3:
    runs = {side: [osu(path(STEP3, f'osu-reduce-4x1-{side}-{i}.txt')) for i in (1, 2, 3)]
            for side in ('link', 'tree')}
    osu_table('step 3 (2026-10-07), OSU reduce 4 x 1, default binding, no helpers', runs)
    for side in ('link', 'tree'):
        big = osu(path(STEP3, f'osu-reduce-big-4x1-{side}.txt'))
        print(f'step 3 once, {side}: ' + ', '.join(
            f'{n} {big[n]:.1f} ({big[n] / simulated(n)[side, "sender"]:.3f} of the simulation)'
            for n in (2097152, 4194304)))
    print()
for bind in ('bound', 'none'):
    for helpers in (0, 2):
        tag = f'{bind}-h{helpers}'
        what = f'{"default binding" if bind == "bound" else "--cpu-bind=none"}, {helpers} helpers'
        if not os.path.exists(path(OUT, f'osu-reduce-4x1-{tag}-link-3.txt')):
            continue
        osu_table(f'OSU reduce 4 x 1, {what}',
                  {side: [osu(path(OUT, f'osu-reduce-4x1-{tag}-{side}-{i}.txt')) for i in (1, 2, 3)]
                   for side in ('link', 'tree')})
        bench_table(f'osoe-bench 4 x 4, {what}',
                    {side: [bench(path(OUT, f'bench-4x4-{tag}-{side}-{i}.txt')) for i in (1, 2, 3)]
                     for side in ('link', 'tree')})
