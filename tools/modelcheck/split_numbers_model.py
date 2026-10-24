#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""A split's numbers, two splits at once on one PE (step D, proposal §3 D7, as redesigned).

A split agrees its team's number by one AND reduction over the parent of each
parent PE's *free mask* - bit t set when t is free on that PE - and takes the
lowest bit left (D2). Under SHMEM_THREAD_MULTIPLE two threads of one PE can be
in two splits at once, of two parents, and a PE's free mask is one resource:
two reductions reading it at once can agree the same number, and the PE would
hold two teams under it.

The first design (`token`, below) let a thread contribute the PE's mask only
while it held the PE's *mask token*, held across the reduction; a thread that
could not take it contributed zeros and a busy mark, and every parent PE ran
the round again. The adversarial read of 2026-10-03 found the program it
deadlocks: the token held across a reduction that waits for a PE which waits,
in another collective, for a PE that runs the other split's busy rounds forever
(the `crossed` case). Anything held across a blocking reduction can close such
a cycle, so the design keeps nothing across one:

  1. every member contributes its free mask, read without a lock, and the
     reduction agrees the lowest number free on every member;
  2. each member then takes the number *pending* on its PE - a compare-and-swap
     of the number's pending word from 0 to its split's parent number - and,
     having it, looks at the PE's held bits: a number held already is given
     back and the take has failed. A take that finds the word another split's
     waits until it changes if that split's parent number is higher, and fails
     if it is lower. split_2d's two numbers are taken in turn, and a member that
     fails one clears the word of any it took;
  3. a second AND reduction, of each member's "took it": all took it, and each
     marks the number held and then clears its pending word; any did not, and
     each member that took it clears the word, and every parent PE runs the
     round again.

Nothing is held across reduction 1. Between the two reductions a member holds
its pending word, and every member of its split is by then past reduction 1 and
on its way to reduction 2 with no wait but step 2's; a wait in step 2 is for a
split with a higher parent number, whose own waits are for higher numbers
still, so the waits form no cycle (a team's number is one number on every
member, so "higher" means the same on every PE) and every pending word is
cleared in bounded time. The split with the lowest parent number among those
active loses a round only to a number another split has made held, never to a
pending one.

Checked
  UNIQUE   no PE holds two teams under one number.
  ONE      a split's members all take the same numbers.
  LOWEST   split 1, the lowest parent number, runs at most two rounds: it loses
           one only to split 2 making a number of its own held, which happens
           once.
  CAPACITY each split finds on every member the numbers it asks for: the model's
           own sizing of the pools, so that a split never fails for want of one.
  and, by the engine: no stuck thread, and from every reachable state the run can
  still finish (AG EF done). A lower split's wait on a pending word ends once
  the word is cleared and the waiter runs before the higher split takes it
  again; AG EF done is that a schedule exists in which it does.

Cases
  shared     two PEs, two threads each; thread s of each PE splits parent s
             (s = 1, 2), both parents both PEs - every collision, both ways;
             two numbers.
  crossed    the read's program: PE 0's two threads split parent 1 = {0, 1} and
             parent 2 = {0, 2}; PE 1 splits parent 1 and then joins a
             collective with PE 2; PE 2 joins that collective and then splits
             parent 2. Two numbers.
  two_low    as shared, with three numbers, and split 1 agreeing two of them, as
             split_2d does: a member takes them in turn, and may wait for split
             2's pending word on the second holding the first.
  two_high   the same with split 2 agreeing two: a member holding the first
             finds split 1's pending word on the first, and fails.
  second_low, second_high  three PEs, split 1 over {0, 1} and split 2 over
             {0, 2}, and a team of neither already under number 0 on PE 2
             (second_low) or PE 1 (second_high), so that the split agreeing
             two numbers meets the other on its second: in second_low split 1
             holds 0 pending and waits for split 2's 1, in second_high split 2
             holds 0 pending and loses 1 to split 1, clearing 0.
  The reductions and the collective are abstract - each PE posts its
  contribution, and every PE reads all of them once all are in - since they
  are the data collectives the other models check.

Designs
  design         as above.
Breakages
  token          the first design, the mask token held across the reduction:
                 in `crossed` PE 0's thread of parent 2 holds the token waiting
                 for PE 2, and parent 1's rounds stay busy forever (TRAPPED).
  no_pending     each member takes the agreed number at once: two splits take
                 one number on one PE (UNIQUE).
  release_first  a member clears its pending word before marking the number
                 held: the other split takes it pending in between, finds it not
                 held, and both keep it (UNIQUE).
  higher_waits   a take waits for any other split's pending word, a lower one's
                 too: each split waits on the other's (STUCK), in shared and in
                 two_low.
  lower_fails    a take fails on any other split's pending word, a higher one's
                 too: the lowest split can lose round after round (LOWEST).

Memory orders (T29; the step D review's portability Q4). Every case runs under
SC and under x86-TSO, and the two agree state for state: every store on the
design's path is a locked instruction or a `seq_cst` store, an `xchg` on x86,
so no store is ever left in a buffer and the TSO runs reach the SC runs'
states. The orders are team.c's:
  load held (a round's start)
                   free_mask: atomic_load_explicit(&held[w], acquire), read without a lock
  cas pending      take_pending: atomic_compare_exchange_strong_explicit(&pending[n], ...,
                   seq_cst, seq_cst)
  load held (a take)
                   take_pending: atomic_load_explicit(&held[w], seq_cst), after the swap
  store pending 0  give_pending: atomic_store_explicit(&pending[n], 0, seq_cst), after a
                   take lost (`Tu`, and the words taken so far), a round lost elsewhere
                   and a number kept (`clear_pending`)
  or held          split: atomic_fetch_or_explicit(&held[w], bit, seq_cst), before the clear
  reductions       osoe_team_split_and: each member's arrival a locked add; abstract here
`token`, the first design, is not in the tree: its stores are kept serializing as
written, and its verdict is a cycle of waits that no order of stores moves.

Usage: split_numbers_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

HELD = lambda pe: ('held', pe)                    # pe's numbers held, a bit each
PENDING = lambda pe, n: ('pending', pe, n)        # the parent of the split holding n pending
TOKEN = lambda pe: ('token', pe)                  # token: the split holding pe's token
WANT = lambda pe, s: ('want', pe, s)              # token: split s is active on pe
POSTED = lambda s, r, k: ('$posted', s, r, k)     # contributions in to reduction k of round r
READ = lambda s, r, k: ('$read', s, r, k)         # members that have read it
CONTRIBUTION = lambda s, r, k, pe: ('$contribution', s, r, k, pe)
COLLECTIVE = ('$collective',)                     # crossed: PEs in the other collective
HOLDS = lambda pe, n: ('$holds', pe, n)           # the split whose team pe holds under n
TOOK = lambda s, pe: ('$took', s, pe)             # the numbers pe took for split s, plus one
ROUNDS = lambda s, pe: ('$rounds', s, pe)
DONE = ('$done',)
LOCALS = ('r', 'h', 'rm', 'n', 'c', 'o', 'ok', 'all', 'want', 'taken', 'release',
          't', 'may', 'took')

# Each case: the numbers a PE has, each split's parent, how many numbers each
# split agrees - split_strided's one, split_2d's two - and the numbers a PE
# holds already, for a team of neither split.
SHAPES = {
    'shared': (2, {1: (0, 1), 2: (0, 1)}, {1: 1, 2: 1}, {}),
    'crossed': (2, {1: (0, 1), 2: (0, 2)}, {1: 1, 2: 1}, {}),
    'two_low': (3, {1: (0, 1), 2: (0, 1)}, {1: 2, 2: 1}, {}),
    'two_high': (3, {1: (0, 1), 2: (0, 1)}, {1: 1, 2: 2}, {}),
    'second_low': (3, {1: (0, 1), 2: (0, 2)}, {1: 2, 2: 1}, {2: 1}),
    'second_high': (3, {1: (0, 1), 2: (0, 2)}, {1: 1, 2: 2}, {1: 1}),
}
OTHER = 9                                         # the team a PE holds already


def lowest(w):
    return (w & -w).bit_length() - 1 if w else None


def lowest_bits(w, count):
    out = 0
    for _ in range(count):
        if not w:
            return None
        out |= w & -w
        w &= w - 1
    return out


def bits(w):
    while w:
        yield (w & -w).bit_length() - 1
        w &= w - 1


def reduction(s, members, k, value, var, p, then):
    """Post this PE's word to reduction k of split s's round, wait for every
    member's, and read their AND into `var`; the last reader frees the slots,
    which the round after next uses again."""
    def post(M, L):
        r = L['r']
        return {CONTRIBUTION(s, r, k, L['pe']): value(L), POSTED(s, r, k): M[POSTED(s, r, k)] + 1}, {}

    def result(M, L):
        r = L['r']
        word = -1
        for q in members:
            word &= M[CONTRIBUTION(s, r, k, q)]
        upd = {READ(s, r, k): M[READ(s, r, k)] + 1}
        if M[READ(s, r, k)] + 1 == len(members):
            upd = {READ(s, r, k): 0, POSTED(s, r, k): 0}
            upd.update({CONTRIBUTION(s, r, k, q): 0 for q in members})
        return upd, {var: word}

    q = f'{p}{k}'
    return [(q + 'p', ('ghost', post, '@' + q + 'a')),
            (q + 'a', ('await', lambda M, L: M[POSTED(s, L['r'], k)] == len(members), '@' + q + 'r')),
            (q + 'r', ('ghost', result, then))]


def take_checks(s, members, pe, var):
    """UNIQUE and ONE for the numbers in local `var`, and the ghost record of
    their taking."""
    def unique(M, L):
        for n in bits(L[var]):
            if M[HOLDS(pe, n)]:
                return (f'UNIQUE: PE {pe} holds the teams of splits {M[HOLDS(pe, n)]} and {s} '
                        f'under number {n}')
        return None

    def one(M, L):
        for q in members:
            other = M[TOOK(s, q)]
            if q != pe and other and other - 1 != L[var]:
                return (f'ONE: split {s} gave PE {q} numbers {sorted(bits(other - 1))} and '
                        f'PE {pe} numbers {sorted(bits(L[var]))}')
        return None

    def record(M, L):
        upd = {HOLDS(pe, n): s for n in bits(L[var])}
        upd[TOOK(s, pe)] = L[var] + 1
        return upd, {}
    return unique, one, record


def clear_pending(pe, p, then):
    """Clear the pending word of every number in local `release`, one store each."""
    return [(p, ('br', lambda L: L['release'] == 0, then, '@' + p + 'n')),
            (p + 'n', ('set', 'n', lambda L: lowest(L['release']), '@' + p + 's')),
            # give_pending's seq_cst store (team.c): an xchg, so serial.
            (p + 's', ('store', lambda L: PENDING(pe, L['n']), lambda L: 0, True, '@' + p + 'x')),
            (p + 'x', ('set', 'release', lambda L: L['release'] & ~(1 << L['n']), '@' + p))]


def split_pending(design, case, pe, s, p, then):
    """The design and its breakages but `token`: a split of parent s on PE pe,
    labels prefixed p, going to `then` once it has its numbers."""
    numbers, parents, takes, _ = SHAPES[case]
    members, count = parents[s], takes[s]
    every = (1 << numbers) - 1

    def waits(L):
        """Step 2: wait for the split holding the word, or fail."""
        if design == 'higher_waits':
            return True
        if design == 'lower_fails':
            return False
        return L['o'] > s

    def lowest_rounds(M, L):
        if design == 'no_pending' or s != min(parents) or M[ROUNDS(s, pe)] <= 2:
            return None
        return (f'LOWEST: split {s}, the lowest parent number, began round {M[ROUNDS(s, pe)]} '
                f'on PE {pe}: it lost a round to a pending number')

    # Rounds are counted for split 1 alone, which LOWEST bounds; split 2's
    # count would make every round a new state.
    step = 1 if s == min(parents) else 0
    B = [(p + 'R', ('gpre', lambda M, L: ({ROUNDS(s, pe): M[ROUNDS(s, pe)] + step}, {}),
                    '@' + p + 'Rl')),
         (p + 'Rl', ('load', lambda L: HELD(pe), 'h', '@' + p + 'Rk')),
         (p + 'Rk', ('check', lowest_rounds, '@' + p + '1p'))]
    B += reduction(s, members, 1, lambda L: ~L['h'] & every, 'rm', p, '@' + p + 'N')
    B += [(p + 'N', ('check', lambda M, L: None if lowest_bits(L['rm'], count) is not None else
                     f'CAPACITY: split {s} found fewer than {count} numbers free on every member, '
                     f'which these splits of {numbers} numbers cannot meet', '@' + p + 'Nn')),
          (p + 'Nn', ('set', 'want', lambda L: lowest_bits(L['rm'], count), '@' + p + 'Nt'))]
    if design == 'no_pending':
        # Taken at once: held, as the single-threaded library does.
        unique, one, record = take_checks(s, members, pe, 'want')
        B += [(p + 'Nt', ('check', unique, '@' + p + 'To')),
              (p + 'To', ('check', one, '@' + p + 'Tt')),
              (p + 'Tt', ('or', lambda L: HELD(pe), lambda L: L['want'], '_', '@' + p + 'Tg')),
              (p + 'Tg', ('gpost', record, then))]
        return B
    unique, one, record = take_checks(s, members, pe, 'taken')
    B += [
        (p + 'Nt', ('set', 'taken', lambda L: 0, '@' + p + 'T0')),
        # Step 2: each number pending on this PE, then not held.
        (p + 'T0', ('br', lambda L: L['want'] == 0, '@' + p + 'Tk', '@' + p + 'T1')),
        (p + 'T1', ('set', 'n', lambda L: lowest(L['want']), '@' + p + 'T')),
        (p + 'T', ('cas', lambda L: PENDING(pe, L['n']), lambda L: 0, lambda L: s, 'c', '@' + p + 'Tc')),
        (p + 'Tc', ('br', lambda L: L['c'], '@' + p + 'Th', '@' + p + 'Tw')),
        (p + 'Th', ('load', lambda L: HELD(pe), 'h', '@' + p + 'Tb')),
        (p + 'Tb', ('br', lambda L: (L['h'] >> L['n']) & 1, '@' + p + 'Tu', '@' + p + 'Tg')),
        # give_pending(numbers, i + 1) of a take that found the number held (team.c):
        # the same seq_cst store.
        (p + 'Tu', ('store', lambda L: PENDING(pe, L['n']), lambda L: 0, True, '@' + p + 'Tf')),
        (p + 'Tg', ('set', 'taken', lambda L: L['taken'] | 1 << L['n'], '@' + p + 'Ti')),
        (p + 'Ti', ('set', 'want', lambda L: L['want'] & ~(1 << L['n']), '@' + p + 'T0')),
        (p + 'Tk', ('set', 'ok', lambda L: 1, '@' + p + '2p')),
        (p + 'Tw', ('load', lambda L: PENDING(pe, L['n']), 'o', '@' + p + 'To')),
        (p + 'To', ('br', lambda L: L['o'] == 0, '@' + p + 'T', '@' + p + 'Ts')),
        (p + 'Ts', ('br', waits, '@' + p + 'Ta', '@' + p + 'Tf')),
        (p + 'Ta', ('await', lambda M, L: M[PENDING(pe, L['n'])] != L['o'], '@' + p + 'T')),
        # Lost: the words this member took so far cleared at once.
        (p + 'Tf', ('set', 'release', lambda L: L['taken'], '@' + p + 'Tq'))]
    B += clear_pending(pe, p + 'Tq', '@' + p + 'Tz')
    B += [(p + 'Tz', ('set', 'taken', lambda L: 0, '@' + p + 'Tn')),
          (p + 'Tn', ('set', 'ok', lambda L: 0, '@' + p + '2p'))]
    # Step 3: did every member take them?
    B += reduction(s, members, 2, lambda L: L['ok'], 'all', p, '@' + p + 'K')
    B += [(p + 'K', ('br', lambda L: L['all'], '@' + p + 'Kc', '@' + p + 'U'))]
    keep = [(p + 'Kc', ('check', unique, '@' + p + 'Ko')),
            (p + 'Ko', ('check', one, '@' + p + 'Kh')),
            (p + 'Kh', ('or', lambda L: HELD(pe), lambda L: L['taken'], '_', '@' + p + 'Kg'))]
    if design == 'release_first':
        B += [(p + 'Kr', ('set', 'release', lambda L: L['taken'], '@' + p + 'Kq'))]
        B += clear_pending(pe, p + 'Kq', '@' + p + 'Kc')
        B = [(lab, ins) if lab != p + 'K' else
             (lab, ('br', lambda L: L['all'], '@' + p + 'Kr', '@' + p + 'U')) for lab, ins in B]
        B += keep + [(p + 'Kg', ('gpost', record, then))]
    else:
        B += keep + [(p + 'Kg', ('gpost', record, '@' + p + 'Kr')),
                     (p + 'Kr', ('set', 'release', lambda L: L['taken'], '@' + p + 'Kq'))]
        B += clear_pending(pe, p + 'Kq', then)
    # Lost elsewhere: the words this member took cleared, and the round again.
    B += [(p + 'U', ('set', 'release', lambda L: L['taken'], '@' + p + 'Uq'))]
    B += clear_pending(pe, p + 'Uq', '@' + p + 'Un')
    B += [(p + 'Un', ('set', 'r', lambda L: 1 - L['r'], '@' + p + 'Ud')),
          (p + 'Ud', ('gpost', lambda M, L: ({}, {v: DEL for v in LOCALS if v != 'r'}),
                      '@' + p + 'R'))]
    return B


def split_token(case, pe, s, p, then):
    """The first design: the PE's mask contributed only under its token, held
    across the reduction; a busy round run again."""
    numbers, parents, _, _ = SHAPES[case]
    members, every = parents[s], (1 << numbers) - 1
    unique, one, record = take_checks(s, members, pe, 'want')
    lower = [o for o, m in parents.items() if o < s and pe in m]

    def may_take(M, L):
        return L['t'] == 0 and all(not M[WANT(pe, o)] for o in lower)

    # The first design's stores (WANT and TOKEN here and below) have no C code: serializing.
    B = [(p + 'W', ('store', lambda L: WANT(pe, s), lambda L: 1, True, '@' + p + 'R')),
         (p + 'R', ('load', lambda L: TOKEN(pe), 't', '@' + p + 'Rt')),
         (p + 'Rt', ('nd', lambda M, L: [({}, {'may': may_take(M, L)}, 'look')], '@' + p + 'Rk')),
         (p + 'Rk', ('br', lambda L: L['may'], '@' + p + 'Rc', '@' + p + 'Rb')),
         (p + 'Rc', ('cas', lambda L: TOKEN(pe), lambda L: 0, lambda L: s, 'took', '@' + p + 'Rm')),
         (p + 'Rb', ('set', 'took', lambda L: False, '@' + p + 'Rm')),
         (p + 'Rm', ('br', lambda L: L['took'], '@' + p + 'Rl', '@' + p + 'Rz')),
         (p + 'Rl', ('load', lambda L: HELD(pe), 'h', '@' + p + '1p')),
         (p + 'Rz', ('set', 'h', lambda L: every, '@' + p + '1p'))]
    # The mask shifted left past a "not busy" bit, so that one AND carries both:
    # a PE that took its token sends its mask << 1 | 1, any other 0.
    B += reduction(s, members, 1,
                   lambda L: ((~L['h'] & every) << 1 | 1) if L['took'] else 0, 'rm', p, '@' + p + 'D')
    B += [(p + 'D', ('br', lambda L: L['rm'] & 1, '@' + p + 'N', '@' + p + 'B')),
          (p + 'B', ('br', lambda L: L['took'], '@' + p + 'Bg', '@' + p + 'Bx')),
          (p + 'Bg', ('store', lambda L: TOKEN(pe), lambda L: 0, True, '@' + p + 'Bx')),
          (p + 'Bx', ('set', 'r', lambda L: 1 - L['r'], '@' + p + 'By')),
          (p + 'By', ('gpost', lambda M, L: ({}, {v: DEL for v in ('t', 'may', 'took', 'h', 'rm')}),
                      '@' + p + 'R')),
          (p + 'N', ('check', lambda M, L: None if L['rm'] >> 1 else
                     f'CAPACITY: split {s} found no number free on every member', '@' + p + 'Nn')),
          (p + 'Nn', ('set', 'want', lambda L: 1 << lowest(L['rm'] >> 1), '@' + p + 'Tu')),
          (p + 'Tu', ('check', unique, '@' + p + 'To')),
          (p + 'To', ('check', one, '@' + p + 'Ta')),
          (p + 'Ta', ('or', lambda L: HELD(pe), lambda L: L['want'], '_', '@' + p + 'Tg')),
          (p + 'Tg', ('gpost', record, '@' + p + 'Tt')),
          (p + 'Tt', ('store', lambda L: TOKEN(pe), lambda L: 0, True, '@' + p + 'Tw')),
          (p + 'Tw', ('store', lambda L: WANT(pe, s), lambda L: 0, True, then))]
    return B


def collective(p, then):
    """The crossed case's collective over PEs 1 and 2: in, and wait for both."""
    return [(p, ('ghost', lambda M, L: ({COLLECTIVE: M[COLLECTIVE] + 1}, {}), '@' + p + 'a')),
            (p + 'a', ('await', lambda M, L: M[COLLECTIVE] == 2, then))]


def thread(design, case, pe, steps):
    """A thread of PE pe running `steps` in order: ('split', s) or ('collective',)."""
    B = [('S', ('set', 'pe', lambda L: pe, '@s0'))]
    for i, step in enumerate(steps):
        p, then = f's{i}', f'@s{i + 1}' if i + 1 < len(steps) else '@E'
        if step[0] == 'collective':
            B += collective(p, then)
        else:
            B += [(p, ('set', 'r', lambda L: 0, '@' + p + ('W' if design == 'token' else 'R')))]
            B += (split_token(case, pe, step[1], p, then) if design == 'token' else
                  split_pending(design, case, pe, step[1], p, then))
    B += [('E', ('ghost', lambda M, L: ({DONE: M[DONE] + 1}, {v: DEL for v in LOCALS}), '@END')),
          ('END', ('done',))]
    return assemble(B)


def case(design, which):
    numbers, parents, _, initial = SHAPES[which]
    if which == 'crossed':
        threads = [thread(design, which, 0, [('split', 1)]),
                   thread(design, which, 0, [('split', 2)]),
                   thread(design, which, 1, [('split', 1), ('collective',)]),
                   thread(design, which, 2, [('collective',), ('split', 2)])]
    else:
        threads = [thread(design, which, pe, [('split', s)])
                   for s, members in parents.items() for pe in members]
    pes = sorted({pe for members in parents.values() for pe in members})
    mem = {DONE: 0, COLLECTIVE: 0}
    for pe in pes:
        mem[HELD(pe)] = initial.get(pe, 0)
        mem[TOKEN(pe)] = 0
        for n in range(numbers):
            mem[PENDING(pe, n)] = 0
            mem[HOLDS(pe, n)] = OTHER if initial.get(pe, 0) >> n & 1 else 0
        for s in parents:
            mem[WANT(pe, s)] = mem[TOOK(s, pe)] = mem[ROUNDS(s, pe)] = 0
    for s, members in parents.items():
        for r in (0, 1):
            for k in (1, 2):
                mem[POSTED(s, r, k)] = mem[READ(s, r, k)] = 0
                for pe in members:
                    mem[CONTRIBUTION(s, r, k, pe)] = 0
    name = f'split_numbers/{which}/{design} numbers={numbers}'
    return name, threads, mem, len(threads)


# (case, design, expect): True, or the property it must fail. Each runs under SC and then
# under x86-TSO, with the same verdict.
CASES = [
    ('shared', 'design', True),
    ('crossed', 'design', True),
    ('two_low', 'design', True),
    ('two_high', 'design', True),
    ('second_low', 'design', True),
    ('second_high', 'design', True),
    ('crossed', 'token', 'TRAPPED'),
    ('shared', 'no_pending', 'UNIQUE'),
    ('shared', 'release_first', 'UNIQUE'),
    ('shared', 'higher_waits', 'STUCK'),
    ('shared', 'lower_fails', 'LOWEST'),
    ('two_low', 'higher_waits', 'STUCK'),
]


def main():
    want = select(sys.argv)
    ok = True
    for which, design, expect in CASES:
        name, threads, mem, count = case(design, which)
        if not want(name):
            continue
        for tso in (False, True):
            res = explore(name, threads, mem, tso=tso, max_seconds=1800,
                          liveness_max_states=6_000_000,
                          done=lambda M, count=count: M[DONE] == count)
            ok &= report(res, expect=expect, tso=tso)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
