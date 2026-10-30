#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""T10's Pi timing on the tree to be tagged (pi-t10.sh): `link`, the default,
against `tree`, step 2's trees alone, and against OSOE_PROGRESS_HELPERS=0 at
the default shape, three rotations each, alternating - after step 3's
analyse-part3.py. Per line: each side's median over its rotations and its
spread (max/min - 1); a comparison's noise is the larger spread of its two
sides. tree/link above 1: the default (Träff's where the chooser takes it)
faster; h0/link above 1: the default helpers faster than none."""
import os
import re
import statistics
import sys

D = sys.argv[1]
SIDES = ('link', 'tree', 'helpers-0')


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


def runs(stem, reader):
    found = {}
    for side in SIDES:
        found[side] = [reader(p) for p in (os.path.join(D, f'{stem}-{side}-{r}.txt') for r in (1, 2, 3))
                       if os.path.exists(p)]
    return found


def line(what, sides, key):
    vals = {s: [run[key] for run in sides[s] if key in run] for s in SIDES}
    if any(len(v) < 2 for v in vals.values()):
        return None
    med = {s: statistics.median(v) for s, v in vals.items()}
    spread = {s: max(v) / min(v) - 1 for s, v in vals.items()}
    tree = med['tree'] / med['link']
    helpers = med['helpers-0'] / med['link']
    print(f"{what:40s} {med['link']:10.1f} {med['tree']:10.1f} {med['helpers-0']:10.1f}  "
          f"{tree:7.3f} {max(spread['link'], spread['tree']):5.3f}  "
          f"{helpers:7.3f} {max(spread['link'], spread['helpers-0']):5.3f}")
    return med


print(f"{'what':40s} {'link':>10s} {'tree':>10s} {'helpers-0':>10s}  tree/link noise  h0/link noise")
for layout in ('4x4', '4x1'):
    sides = runs(f'band-{layout}', band)
    keys = sorted(set().union(*(set(r) for r in sides['link'])) if sides['link'] else [])
    for key in keys:
        if key[1] >= 16384 or key[0] == 'barrier':
            line(f'band {layout} {key[0]} {key[1]}', sides, key)
for stem in ('osu-reduce-4x1', 'osu-reduce-4x4', 'osu-broadcast-4x4'):
    sides = runs(stem, osu)
    for size in (16384, 65536, 262144, 1048576):
        line(f'{stem} {size}', sides, size)
