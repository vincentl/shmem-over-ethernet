#!/usr/bin/env python3
"""Two more breakages of the fill writer, to see the model's OVERFLOW and
accounting checks fire on a pure length error (no seal involved)."""
import sys
import reservation_model as R
from engine import explore, report

orig = R.part_blocks

def patched(kind):
    def pb(p, w, cap, least, done, variant):
        B = orig(p, w, cap, least, done, 'fill')
        out = []
        for lab, ins in B:
            if lab == p + 'T' and kind == 'room_plus_one':
                ins = ('set', 't', lambda L, c=cap: max(0, min(L['rem'], R.H + c - L['h'] + 1)),
                       ins[3])
            if lab == p + 'CM' and kind == 'commit_most':
                # commit the whole remainder rather than the part taken
                ins = ('add', lambda L: R.FILLED(L['k']), lambda L: L['rem'], '_', ins[4])
            out.append((lab, ins))
        return out
    return pb

ok = True
for kind, writers, sizes, cap in (('room_plus_one', 1, (2, 2), 3), ('commit_most', 1, (3,), 2)):
    R.part_blocks = patched(kind)
    name, threads, mem = R.fill_case('fill', writers, sizes, cap, 1)
    res = explore(f'{kind} ' + name, threads, mem, max_seconds=300)
    ok &= report(res, expect_ok=False)
R.part_blocks = orig
sys.exit(0 if ok else 1)
