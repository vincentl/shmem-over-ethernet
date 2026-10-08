#!/usr/bin/env python3
"""Reviewer's variants of reservation_model.py's `fill` cases (not part of the tree).

1. turnover: the FULL/NONE wait is the C's - the writer loads the turnover word
   before it tries, and on failure sleeps until the word moves; only open()
   bumps the word (after its head store), as osoe_connection_open does. The
   tree's model wakes the writer on any change of the head instead.
2. mixed: writers with different floors on one connection - a filling put
   beside a writer whose every message is reserved whole (a get, an rmw).
"""
import sys

import reservation_model as R
from engine import DEL, assemble, explore, report, select

TURN = ('turn',)


def part_blocks_turnover(p, w, cap, least, done):
    B = [(p + 'S0', ('load', lambda L: TURN, 'tw', '@' + p + 'S0a')),
         (p + 'S0a', ('load', lambda L: R.HEAD(0), 'v', '@' + p + 'B0')),
         (p + 'B0', ('br', lambda L: not (L['v'] & R.SEALED), '@' + p + 'K0', '@' + p + 'S1')),
         (p + 'K0', ('set', 'k', lambda L: 0, '@' + p + 'KH')),
         (p + 'S1', ('load', lambda L: R.HEAD(1), 'v', '@' + p + 'B1')),
         (p + 'B1', ('br', lambda L: not (L['v'] & R.SEALED), '@' + p + 'K1', '@' + p + 'NW')),
         (p + 'K1', ('set', 'k', lambda L: 1, '@' + p + 'KH')),
         (p + 'KH', ('set', 'h', lambda L: L['v'], '@' + p + 'KV')),
         (p + 'KV', ('set', 'v', lambda L: DEL, '@' + p + 'T')),
         (p + 'T', ('set', 't', lambda L, c=cap: max(0, min(L['rem'], R.H + c - L['h'])),
                    '@' + p + 'CHK')),
         (p + 'CHK', ('br', lambda L, f=least: L['t'] < min(L['rem'], f), '@' + p + 'FW',
                      '@' + p + 'CAS')),
         (p + 'CAS', ('cas', lambda L: R.HEAD(L['k']), lambda L: L['h'], lambda L: L['h'] + L['t'],
                      'ok', '@' + p + 'OK')),
         (p + 'OK', ('br', lambda L: L['ok'], '@' + p + 'OV', '@' + p + 'RT')),
         # a lost CAS looks again at once, inside osoe_connection_reserve_up_to
         (p + 'RT', ('set', 'ok', lambda L: DEL, '@' + p + 'RT2')),
         (p + 'RT2', ('set', 'h', lambda L: DEL, '@' + p + 'RT3')),
         (p + 'RT3', ('set', 'k', lambda L: DEL, '@' + p + 'RT4')),
         (p + 'RT4', ('set', 't', lambda L: DEL, '@' + p + 'S0a')),
         (p + 'OV', ('check', lambda M, L, c=cap, w=w: None if (
             not (L['h'] & R.SEALED) and L['h'] + L['t'] <= R.H + c) else
             f"OVERFLOW: writer {w} reserved {L['t']} units at {L['h']}", '@' + p + 'OK2')),
         (p + 'OK2', ('set', 'ok', lambda L: DEL, '@' + p + 'RS')),
         (p + 'RS', ('load', lambda L: R.SEQ(L['k']), 's', '@' + p + 'CP')),
         (p + 'CP', ('ghost', lambda M, L, w=w: (
             {('$slot', L['k'], L['h'] + j): 1 + w * R.PER_WRITER + L['u'] + j
              for j in range(L['t'])}, {}), '@' + p + 'CM')),
         (p + 'CM', ('add', lambda L: R.FILLED(L['k']), lambda L: L['t'], '_', '@' + p + 'CG')),
         (p + 'CG', ('gpost', lambda M, L, w=w: (
             {('$rseq', 1 + w * R.PER_WRITER + L['u'] + j): L['s'] for j in range(L['t'])}, {}),
             '@' + p + 'AD')),
         (p + 'AD', ('set', 'u', lambda L: L['u'] + L['t'], '@' + p + 'AR')),
         (p + 'AR', ('set', 'rem', lambda L: L['rem'] - L['t'], '@' + p + 'CL')),
         (p + 'CL', ('set', 'h', lambda L: DEL, '@' + p + 'CL1')),
         (p + 'CL1', ('set', 't', lambda L: DEL, '@' + p + 'CL2')),
         (p + 'CL2', ('set', 'k', lambda L: DEL, '@' + p + 'CL3')),
         (p + 'CL3', ('set', 'tw', lambda L: DEL, '@' + p + 'NX')),
         (p + 'NX', ('br', lambda L: L['rem'] > 0, '@' + p + 'S0', done)),
         # FULL or NONE: sleep until open() moves the turnover word, then look again
         (p + 'NW', ('set', 'v', lambda L: DEL, '@' + p + 'FW')),
         (p + 'FW', ('await', lambda M, L: M[TURN] != L['tw'], '@' + p + 'FW2')),
         (p + 'FW2', ('set', 'h', lambda L: DEL, '@' + p + 'FW3')),
         (p + 'FW3', ('set', 'k', lambda L: DEL, '@' + p + 'FW4')),
         (p + 'FW4', ('set', 't', lambda L: DEL, '@' + p + 'S0'))]
    return B


def writer_turnover(w, sizes, cap, least, quiet=True):
    B = [('U0', ('set', 'u', lambda L: 0, '@m0_M'))]
    for m, size in enumerate(sizes):
        p = f'm{m}_'
        nxt = f'@m{m + 1}_M' if m + 1 < len(sizes) else '@Q0'
        B += [(p + 'M', ('set', 'rem', lambda L, n=size: n, '@' + p + 'S0'))]
        B += part_blocks_turnover(p, w, cap, least, nxt)
    units = sum(sizes)
    if quiet:
        B += [('Q0', ('set', 'rem', lambda L: DEL, '@Q1')),
              ('Q1', ('await', lambda M, L: M[R.ACKED] >= L['s'], '@Q2')),
              ('Q2', ('check', lambda M, L, w=w, n=units: None if all(
                  M[('$applied', 1 + w * R.PER_WRITER + u)] == 1 for u in range(n)) else
                  f"QUIET: writer {w}", '@WEND'))]
    else:
        B += [('Q0', ('set', 's', lambda L: DEL, '@WEND'))]
    B += [('WEND', ('done',))]
    return assemble(B)


def progress_with_turnover(total, cap, replies):
    """reservation_model.progress('design') with open() bumping the turnover word
    after its head store, as osoe_connection_open's wake_all does."""
    prog = R.progress('design', total, cap, replies)
    # Find F4d (the head store of open) by its shape: the instruction after the seq
    # store F4c. Rebuild by relabelling is not possible on an assembled program, so
    # rebuild the blocks: easier to patch the assembled tuple list.
    out = list(prog)
    # locate: ('store', HEAD(k), head_at, False, nxt) whose nxt is the 'set nseq'
    hits = [n for n, ins in enumerate(out)
            if ins[0] == 'store' and out[ins[4]][0] == 'set' and out[ins[4]][1] == 'nseq']
    assert len(hits) == 1, hits
    idx = hits[0]
    store = out[idx]
    bump_at = len(out)
    out.append(('add', lambda L: TURN, lambda L: 1, '_', store[4]))
    out[idx] = (store[0], store[1], store[2], store[3], bump_at)
    return out


def case(name, specs, cap, replies, turnover=True):
    """specs: [(sizes, least)] per writer."""
    total = sum(sum(s) for s, _ in specs) + replies
    if turnover:
        threads = [writer_turnover(w, sizes, cap, least) for w, (sizes, least) in enumerate(specs)]
        threads.append(progress_with_turnover(total, cap, replies))
    else:
        threads = [R.filling_writer(w, sizes, cap, least, True, 'fill')
                   for w, (sizes, least) in enumerate(specs)]
        threads.append(R.progress('design', total, cap, replies))
    mem = {R.HEAD(0): R.SEALED, R.HEAD(1): R.SEALED, R.FILLED(0): 0, R.FILLED(1): 0,
           R.SEQ(0): 0, R.SEQ(1): 0, R.ACKED: 0, TURN: 0}
    return name, threads, mem


CASES = [
    # (name, specs, cap, replies, tso, turnover)
    ('turnover fill 1w (2,2) cap3', [((2, 2), 1)], 3, 0, False, True),
    ('turnover fill 1w (3,1) cap2', [((3, 1), 1)], 2, 0, False, True),
    ('turnover fill 2w (3,) cap2', [((3,), 1), ((3,), 1)], 2, 0, False, True),
    ('turnover fill 2w (2,1) cap2 least2', [((2, 1), 2), ((2, 1), 2)], 2, 0, False, True),
    ('turnover fill 1w (3,) cap2 replies1', [((3,), 1)], 2, 1, False, True),
    ('turnover fill 2w (2,) cap2 replies1', [((2,), 1), ((2,), 1)], 2, 1, False, True),
    ('turnover fill 2w (3,1) cap3 replies1', [((3, 1), 1), ((3, 1), 1)], 3, 1, False, True),
    ('turnover fill 2w (2,) cap3 TSO', [((2,), 1), ((2,), 1)], 3, 0, True, True),
    ('turnover fill 2w (3,) cap2 TSO', [((3,), 1), ((3,), 1)], 2, 0, True, True),
    ('turnover mixed fill(3,1)+whole(1,1) cap2', [((3, 1), 1), ((1, 1), 99)], 2, 0, False, True),
    ('turnover mixed fill(3,1)+whole(2,) cap3 replies1', [((3, 1), 1), ((2,), 99)], 3, 1, False, True),
    ('turnover mixed fill(2,2)+whole(1,) cap3 TSO', [((2, 2), 1), ((1,), 99)], 3, 0, True, True),
    ('mixed fill(3,1)+whole(1,1) cap2', [((3, 1), 1), ((1, 1), 99)], 2, 0, False, False),
    ('mixed fill(2,2)+whole(2,) cap3 replies1', [((2, 2), 1), ((2,), 99)], 3, 1, False, False),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for name, specs, cap, replies, tso, turnover in CASES:
        if not keep(name):
            continue
        n, threads, mem0 = case(name, specs, cap, replies, turnover)
        res = explore(n, threads, mem0, tso=tso, max_seconds=900)
        ok &= report(res, expect_ok=True, tso=tso)
    sys.exit(0 if ok else 1)
