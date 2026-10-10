#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Step 3 Part 3's Pi comparison: link (Träff's where the chooser takes it)
against tree (step 2's trees alone), three rotations each, alternating. Per
line: each side's median over its rotations and its spread (max/min - 1); the
noise of a comparison is the larger spread of its two sides. Flags a line
slower under link than under tree by more than the noise, and lists the
reductions' ratios tree/link (above 1: Träff's faster)."""
import re, statistics, sys, os
D = sys.argv[1]
SIDES = ('link', 'tree')


def band(path):
    out = {}
    for line in open(path):
        m = re.match(r'e-band pes=(\d+) (\w+) bytes=(\d+) slowest_pe_median_us=([\d.]+)', line)
        if m:
            out[(m.group(2), int(m.group(3)))] = float(m.group(4))
    return out


def osu(path):
    out = {}
    for line in open(path):
        m = re.match(r'^(\d+)\s+([\d.]+)\s*$', line)
        if m:
            out[int(m.group(1))] = float(m.group(2))
    return out


def bench(path):
    out = {}
    for line in open(path):
        if 'median_us' in line:
            key = re.sub(r' (median|fastest)_us=[\d.]+', '', line.strip())
            v = re.search(r'median_us=([\d.]+)', line)
            if v:
                out[key] = float(v.group(1))
    return out


rows = []


def compare(kind, layout, reader, op=None):
    sides = {}
    for side in SIDES:
        runs = []
        for r in (1, 2, 3):
            name = f'{kind}-{op}-{layout}-{side}-{r}.txt' if op else f'{kind}-{layout}-{side}-{r}.txt'
            p = os.path.join(D, name)
            if os.path.exists(p):
                runs.append(reader(p))
        sides[side] = runs
    keys = set(sides['link'][0]) if sides['link'] else set()
    for key in sorted(keys, key=str):
        vals = {s: [run[key] for run in sides[s] if key in run] for s in SIDES}
        if any(len(v) < 2 for v in vals.values()):
            continue
        med = {s: statistics.median(v) for s, v in vals.items()}
        spread = {s: max(v) / min(v) - 1 if min(v) > 0 else 0 for s, v in vals.items()}
        rows.append((kind, layout, op, key, med, spread))


for layout in ('4x4', '4x1'):
    compare('band', layout, band)
    compare('osu', layout, osu, 'reduce')
compare('bench', '4x4', bench)
flags = []
print(f"{'what':48s} {'link':>10s} {'tree':>10s}  tree/link  noise")
for kind, layout, op, key, med, spread in rows:
    if kind == 'bench' and 'local' in str(key):
        continue
    ratio = med['tree'] / med['link'] if med['link'] else 0
    noise = max(spread['link'], spread['tree'])
    what = f"{kind} {layout} {op or ''} {key}"[:48]
    mark = ''
    if 1 / ratio - 1 > noise and 1 / ratio > 1.02 if ratio else False:
        mark = ' SLOWER-UNDER-LINK'
        flags.append((what, 1 / ratio, noise))
    print(f"{what:48s} {med['link']:10.1f} {med['tree']:10.1f}  {ratio:9.3f}  {noise:5.3f}{mark}")
print(f"\nlines slower under link than tree by more than the noise: {len(flags)}")
for f in flags:
    print('  ', f)
for side in SIDES:
    p = os.path.join(D, f'osu-reduce-big-4x1-{side}.txt')
    if os.path.exists(p):
        print(f'big {side}:', osu(p))
