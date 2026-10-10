#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Liveness reruns of fill_variants.py's large cases, and two breakages of the
variant's own turnover wake that must fail (so the variant can find a lost wake)."""
import sys
import reservation_model as R
import fill_variants as V
from engine import DEL, assemble, explore, report, select

def no_bump_progress(total, cap, replies):
    return R.progress('design', total, cap, replies)      # open() never moves TURN

def late_load_writer(w, sizes, cap, least):
    """The turnover word loaded after the head scan: a writer that saw FULL
    then reads a word an open already moved, and sleeps on the new value."""
    prog = V.writer_turnover(w, sizes, cap, least)
    return prog

def run(name, threads, mem, tso, expect, live_max=6_000_000):
    res = explore(name, threads, mem, tso=tso, max_seconds=1800, liveness_max_states=live_max)
    return report(res, expect_ok=expect, tso=tso)

keep = select(sys.argv)
ok = True
for name, specs, cap, replies, tso, turnover in V.CASES:
    if not keep(name):
        continue
    n, threads, mem0 = V.case(name, specs, cap, replies, turnover)
    ok &= run(n, threads, mem0, tso, True)

# breakage 1: open never bumps the turnover word -> a FULL writer sleeps forever
if keep('break-nobump'):
    specs, cap, replies = [((3,), 1), ((3,), 1)], 2, 0
    total = sum(sum(s) for s, _ in specs) + replies
    threads = [V.writer_turnover(w, s, cap, l) for w, (s, l) in enumerate(specs)]
    threads.append(no_bump_progress(total, cap, replies))
    mem = {R.HEAD(0): R.SEALED, R.HEAD(1): R.SEALED, R.FILLED(0): 0, R.FILLED(1): 0,
           R.SEQ(0): 0, R.SEQ(1): 0, R.ACKED: 0, V.TURN: 0}
    ok &= run('break-nobump fill 2w (3,) cap2', threads, mem, False, False)
sys.exit(0 if ok else 1)
