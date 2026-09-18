#!/usr/bin/env python3
"""Does changing the *coverage predicate* of the §4.9 watch slot change the
protocol?  (docs/design/watch-notification-options.md)

`docs/design/evidence/watch/range_watch_model.py` checks the §4.9 protocol with
the power-of-two block predicate.  Its watched ranges are a few bytes wide, so
every element of every case falls inside one 16-byte grain, and a predicate
that covered the *ends* of a range but not its middle would pass it.  This
model exists to say so out loud and to check the replacement predicates the
memo considers.

Same protocol as §4.9, unchanged:

  waiter   publish the watch word (seq_cst) BEFORE the first predicate check;
           loop { load seq; load the variables; if satisfied, stop;
                  futex_wait(seq) }; publish 0.
  writer   update the variable with a locked rmw; load the watch word; if it
           covers the bytes written, add 1 to seq and futex_wake.

Only `pack` and `covers` change between filters.  The elements of the array
case are 64 bytes apart, so they lie in different 16-byte and 4-byte grains and
a grain-level miss is visible.

Filters
  block     §4.9 today: the smallest aligned power-of-two block containing the
            range, as ((off >> k) << 6) | (k + 1)
  exact     the memo's proposal: region | offset in 4-byte grains | length in
            4-byte grains, saturating to "the rest of the region"
  sig       a 64-bit Bloom signature over 16-byte grains, one hash function

Broken variants, each of which must lose a wake
  endpoints the range's first and last grain only - the mistake that a range
            "is" its two ends, which is what packet classification's
            range-to-prefix expansion exists to avoid
  satzero   `exact` whose saturated length encodes 0 ("no watch") instead of
            "to the end of the region"
  sigfirst  `sig` whose writer tests only the first grain of a multi-grain
            write instead of every grain it touched
  checkfirst   the waiter checks before it publishes (§4.9, invariant 13)
  notifyfirst  the writer reads the word before it updates the variable (§4.9)
  plainpub     the waiter publishes with a plain store, under TSO (§3.16)

Usage: coverage_model.py [substring ...]
"""
import sys

from engine import DEL, assemble, explore, report, select

VAR = lambda o: ('var', o)
W = ('watch',)
SEQ = ('seq',)

MIX = 0x9E3779B97F4A7C15
M64 = (1 << 64) - 1

# Deliberately small so the saturation path is reachable in a small model.
GRAIN = 4
LEN_BITS, OFF_BITS = 3, 20
LEN_MAX = (1 << LEN_BITS) - 1                 # 7 grains = 28 bytes


# ---- block: §4.9 today ------------------------------------------------------

def block_pack(lo, n):
    k = 0
    while (lo >> k) != ((lo + n - 1) >> k):
        k += 1
    return ((lo >> k) << 6) | (k + 1)


def block_covers(w, o, n):
    if w == 0:
        return False
    k = (w & 63) - 1
    want = w >> 6
    return (o >> k) <= want <= ((o + n - 1) >> k)


# ---- exact: the memo's packed interval --------------------------------------

def exact_pack(lo, n, saturate_to_zero=False):
    g0, g1 = lo // GRAIN, (lo + n - 1) // GRAIN
    cnt = g1 - g0 + 1
    if cnt > LEN_MAX:
        cnt = 0 if saturate_to_zero else LEN_MAX      # LEN_MAX means "and on"
    return (g0 << LEN_BITS) | cnt


def exact_covers(w, o, n):
    cnt = w & LEN_MAX
    if cnt == 0:
        return False
    g0 = w >> LEN_BITS
    a, b = o // GRAIN, (o + n - 1) // GRAIN
    if cnt == LEN_MAX:
        return b >= g0                                 # saturated: to the end
    return a < g0 + cnt and g0 <= b


# ---- endpoints: the broken "a range is its two ends" predicate --------------

def ends_pack(lo, n):
    return (1 << (lo // GRAIN)) | (1 << ((lo + n - 1) // GRAIN))


def ends_covers(w, o, n):
    for g in range(o // GRAIN, (o + n - 1) // GRAIN + 1):
        if w & (1 << g):
            return True
    return False


# ---- sig: a 64-bit signature over 16-byte grains, one hash ------------------

SIG_GRAIN = 16


def sig_bit(g):
    x = (g * MIX) & M64
    x ^= x >> 29
    return 1 << (x % 64)


def sig_pack(lo, n):
    s = 0
    for g in range(lo // SIG_GRAIN, (lo + n - 1) // SIG_GRAIN + 1):
        s |= sig_bit(g)
    return s


def sig_covers(w, o, n):
    for g in range(o // SIG_GRAIN, (o + n - 1) // SIG_GRAIN + 1):
        if w & sig_bit(g):
            return True
    return False


def sig_covers_first(w, o, n):
    return bool(w & sig_bit(o // SIG_GRAIN))           # broken: first grain only


FILTERS = {
    'block':     (block_pack, block_covers),
    'exact':     (exact_pack, exact_covers),
    'satzero':   (lambda lo, n: exact_pack(lo, n, True), exact_covers),
    'endpoints': (ends_pack, ends_covers),
    'sig':       (sig_pack, sig_covers),
    'sigfirst':  (sig_pack, sig_covers_first),
}


def waiter(elems, lo, n, pack, variant):
    """Waits until every element of `elems` is at least 1."""
    pred = lambda L: all(L[f'v{e}'] >= 1 for e in range(len(elems)))
    forget = {f'v{e}': DEL for e in range(len(elems))}

    def loads(tag, then):
        return [(f'{tag}{i}', ('load', lambda L, o=o: VAR(o), f'v{i}',
                               '@' + (f'{tag}{i + 1}' if i + 1 < len(elems) else then)))
                for i, o in enumerate(elems)]

    B = []
    after = 'L0'
    if variant == 'checkfirst':
        B += [('A', ('br', lambda L: True, '@F0', '@F0'))]
        B += loads('F', 'FC')
        B += [('FC', ('br', pred, '@FX', '@FK')),
              ('FX', ('gpost', lambda M, L: ({}, forget), '@END')),
              ('FK', ('gpost', lambda M, L: ({}, forget), '@P')),
              ('Z', ('load', lambda L: SEQ, 'q', '@S'))]
        after = 'Z'
    else:
        B += [('A', ('br', lambda L: True, '@P', '@P'))]

    B += [('P', ('store', lambda L: W, lambda L: pack(lo, n),
                 variant != 'plainpub', '@' + after)),
          ('L0', ('load', lambda L: SEQ, 'q', '@V0'))]
    B += loads('V', 'C')
    B += [('C', ('br', pred, '@U', '@K')),
          ('K', ('gpost', lambda M, L: ({}, forget), '@S')),
          ('S', ('fwait', lambda L: SEQ, lambda L: L['q'], '@L0')),
          ('U', ('gpost', lambda M, L: ({}, {'q': DEL, **forget}), '@U1')),
          ('U1', ('store', lambda L: W, lambda L: 0, True, '@END')),
          ('END', ('done',))]
    return assemble(B)


def writer(elem, o, width, covers, variant):
    """Applies a write that lands on `elem` and spans [o, o + width), then
    notifies with the range it wrote."""
    update = [('X0', ('add', lambda L: VAR(elem), lambda L: 1, '_', '@X1'))]
    scan = [('X1', ('load', lambda L: W, 'x', '@X2')),
            ('X2', ('br', lambda L: covers(L['x'], o, width), '@X3', '@END')),
            ('X3', ('add', lambda L: SEQ, lambda L: 1, '_', '@X4')),
            ('X4', ('fwake', lambda L: SEQ, '@END'))]
    if variant == 'notifyfirst':
        scan = [(lab, tuple('@X0' if x == '@END' else x for x in ins)) for lab, ins in scan]
        update = [(lab, tuple('@DONE' if x == '@X1' else x for x in ins)) for lab, ins in update]
        B = [('START', ('br', lambda L: True, '@X1', '@X1'))] + scan + update + [('DONE', ('done',))]
    else:
        B = update + scan + [('END', ('done',))]
    return assemble(B)


def run(name, filt, elems, lo, n, writes, tso=False, variant='', expect_ok=None):
    pack, covers = FILTERS[filt]
    threads = [waiter(elems, lo, n, pack, variant)]
    threads += [writer(e, o, w, covers, variant) for e, o, w in writes]
    res = explore(name, threads, {W: 0}, tso=tso)
    return report(res, expect_ok=expect_ok, tso=tso)


# A scalar wait: one 8-byte ivar.
SCALAR = ([0], 0, 8, [(0, 0, 8)])
# An array wait whose elements are 64 bytes apart, so they are in different
# 16-byte grains and different 4-byte grains.  [0, 136) is 34 4-byte grains,
# which is past LEN_MAX, so `exact` publishes its saturated form here.
SPREAD = ([0, 64, 128], 0, 136, [(0, 0, 8), (64, 64, 8), (128, 128, 8)])
# Two elements one grain apart: within LEN_MAX, so `exact` is exact.
NEAR = ([0, 8], 0, 16, [(0, 0, 8), (8, 8, 8)])
# A multi-grain write: the writer lands on the watched element at 32 but the
# range it notifies is [16, 48), which spans 16-byte grains 1 and 2.  A filter
# whose writer tests only the first grain of its own write misses it.
BULK = ([32], 32, 8, [(32, 16, 32)])

CASES = [
    # name,                                  filter,      shape,   tso, variant, expect
    ('design block:  scalar',                'block',     SCALAR,  False, '', True),
    ('design block:  spread array',          'block',     SPREAD,  False, '', True),
    ('design exact:  scalar',                'exact',     SCALAR,  False, '', True),
    ('design exact:  scalar',                'exact',     SCALAR,  True,  '', True),
    ('design exact:  two near elements',     'exact',     NEAR,    False, '', True),
    ('design exact:  two near elements',     'exact',     NEAR,    True,  '', True),
    ('design exact:  spread array (sat)',    'exact',     SPREAD,  False, '', True),
    ('design exact:  spread array (sat)',    'exact',     SPREAD,  True,  '', True),
    ('design exact:  multi-grain write',     'exact',     BULK,    False, '', True),
    ('design sig:    scalar',                'sig',       SCALAR,  False, '', True),
    ('design sig:    spread array',          'sig',       SPREAD,  False, '', True),
    ('design sig:    spread array',          'sig',       SPREAD,  True,  '', True),
    ('design sig:    multi-grain write',     'sig',       BULK,    False, '', True),

    ('endpoints: spread array',              'endpoints', SPREAD,  False, '', False),
    ('endpoints: scalar (passes)',           'endpoints', SCALAR,  False, '', True),
    ('endpoints: two near elements (passes)', 'endpoints', NEAR,   False, '', True),
    ('satzero:   spread array',              'satzero',   SPREAD,  False, '', False),
    ('satzero:   two near elements (passes)', 'satzero',  NEAR,    False, '', True),
    ('sigfirst:  multi-grain write',         'sigfirst',  BULK,    False, '', False),
    ('sigfirst:  scalar (passes)',           'sigfirst',  SCALAR,  False, '', True),

    ('exact + checkfirst',                   'exact',     NEAR,    False, 'checkfirst', False),
    ('exact + notifyfirst',                  'exact',     SCALAR,  False, 'notifyfirst', False),
    ('exact + plainpub',                     'exact',     SCALAR,  True,  'plainpub', False),
    ('sig + checkfirst',                     'sig',       NEAR,    False, 'checkfirst', False),
    ('sig + notifyfirst',                    'sig',       SCALAR,  False, 'notifyfirst', False),
    ('sig + plainpub',                       'sig',       SCALAR,  True,  'plainpub', False),
]

if __name__ == '__main__':
    want = select(sys.argv)
    good = True
    print('== does the coverage predicate change the §4.9 protocol?', flush=True)
    for name, filt, (elems, lo, n, writes), tso, variant, expect in CASES:
        if want(name):
            good &= run(name, filt, elems, lo, n, writes, tso, variant, expect)
    print()
    print('ALL AS EXPECTED' if good else 'SOME RESULT UNEXPECTED OR INCOMPLETE')
