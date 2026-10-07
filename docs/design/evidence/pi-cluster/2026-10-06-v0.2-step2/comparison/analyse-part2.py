#!/usr/bin/env python3
"""Step 2 Part 2's Pi comparison: link (the chooser) against fixed (k 4, D 2 on
this build) and the tag (0.1.0's build), three rotations each, alternating.
Per line: each side's median over its rotations and its spread (max/min - 1);
the noise of a comparison is the larger spread of its two sides. Flags a line
slower under link than under fixed or the tag by more than the noise."""
import re, statistics, sys, glob, os
D = sys.argv[1]
SIDES = ('link', 'fixed', 'tag')
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
        m = re.match(r'(\w+) (.+?) (bytes=\d+)? ?median_us=([\d.]+)', line)
        if line.startswith('latency') or line.startswith('collective') or 'median_us' in line:
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
    for op in ('reduce', 'broadcast', 'fcollect'):
        compare('osu', layout, osu, op)
compare('bench', '4x4', bench)
flags = []
print(f"{'what':44s} {'link':>9s} {'fixed':>9s} {'tag':>9s}  link/fixed link/tag  noise")
for kind, layout, op, key, med, spread in rows:
    if kind == 'bench' and ('local' in str(key)):
        continue
    lf = med['link'] / med['fixed'] if med['fixed'] else 0
    lt = med['link'] / med['tag'] if med['tag'] else 0
    noise_f = max(spread['link'], spread['fixed'])
    noise_t = max(spread['link'], spread['tag'])
    what = f"{kind} {layout} {op or ''} {key}"[:44]
    mark = ''
    if lf - 1 > noise_f and lf > 1.02:
        mark += ' SLOWER-THAN-FIXED'
        flags.append((what, lf, noise_f))
    if lt - 1 > noise_t and lt > 1.02:
        mark += ' SLOWER-THAN-TAG'
    print(f"{what:44s} {med['link']:9.1f} {med['fixed']:9.1f} {med['tag']:9.1f}  {lf:9.3f} {lt:8.3f}  {noise_f:5.3f}{mark}")
print(f"\nlines slower under link than fixed by more than the noise: {len(flags)}")
for f in flags:
    print('  ', f)
