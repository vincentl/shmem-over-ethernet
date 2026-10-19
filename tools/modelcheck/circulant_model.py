#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Träff's circulant reduce-scatter and allgather over a team's node leads
(v0.2 step 3; the author's decision of 2026-10-06, docs/decisions/2026-10-06.md
item 6: modelled first, built if the model holds it). Träff, "Optimal,
Non-pipelined Reduce-scatter and Allreduce Algorithms", arXiv 2410.14234 (2024),
Algorithms 1 and 2.

Over N leads, the vector cut into N blocks, block j the elements
[j count / N, (j + 1) count / N). Lead r's partial for block (r + i) mod N is
its R[i]. The skips halve N, rounding up, to 1: s_0 = N > s_1 > ... > s_q = 1,
q = ceil(log2 N) rounds.
  reduce-scatter  round k, s' = s_k and s = s_{k+1}: lead r sends its partials
                  R[s .. s' - 1] - blocks (r + s .. r + s' - 1) mod N - to lead
                  (r + s) mod N, and receives as many from lead (r - s) mod N,
                  which it combines into R[0 .. s' - s - 1]. After the last, R[0]
                  is block r of the result.
  allgather       the same skips in reverse: lead r sends R[0 .. s' - s - 1] to
                  lead (r - s) mod N and receives R[s .. s' - 1] from lead
                  (r + s) mod N, copying them.
Every lead sends and receives exactly N - 1 blocks in each half, for every N -
no fold, no lead busier than another. A round sends to one lead and receives
from another, the same one only where 2s = N.

This file holds the arithmetic - the schedule, run over symbolic vectors, each
element the multiset of the nodes combined into it, for every N from 2 to 70
at the fold model's nine lengths: every element of every lead's result holds
every node exactly once - in two forms that must agree: the rounds written out
element by element, and round_intervals(), one lead's part in one round as
collective-circulant.c computes it, whose digests shmem.circulant.arithmetic
holds the C to. Its broken variants must fail: a round sent to the wrong lead,
and the allgather's skips in the reduce-scatter's order. Against the fold the
brief described (rabenseifner_model.py): the busiest lead's link the same at a
power of two and less at every other N.

And the protocol, under the engine's SC interleavings (below, "The
protocol"): each round through a row of the receiver's lane, credited piece by
piece for the pieces its sender's window waits on; the reduce-scatter's grants,
a lane each, all taken at its start; the allgather's, a row's as its
reduce-scatter round ends, all taken before the allgather, its lanes carried by
the reduce-scatter's first pieces; every counter the reduce-scatter's own. On
two to five leads, two reductions in a row, and a small tree reduction before
and after one: every piece taken is the one waited for, every lane read is the
one put for it, every put lands in a lane lent to its collective, nothing
written into a lead's words moves once every lead has returned from its last
collective (QUIET, v0.2 step 3's Part 2), nothing is stuck, and from every
state the run can end - five leads past the liveness check's bound. Eight
breakages, each failing.

Usage:
  circulant_model.py [check] [substring ...]  the arithmetic, then the
                                              protocol's cases matching
  circulant_model.py arithmetic               the arithmetic alone
  circulant_model.py volumes                  the busiest lead's link, against the fold's
  circulant_model.py digest FILE              write the C test's digests
  circulant_model.py digest-compare FILE      check FILE is what `digest` writes
"""
import sys

import engine
from engine import DEL

import rabenseifner_model as fold


def skips(n):
    """The rounds' (s', s) pairs: s halving N, rounding up, to 1."""
    out = []
    s = n
    while s > 1:
        t = (s + 1) // 2
        out.append((s, t))
        s = t
    return out


def block(n, count, j):
    """Block j of N over `count` elements: a half-open interval."""
    return j * count // n, (j + 1) * count // n


def blocks_elements(n, count, first_lead, offsets):
    """The elements of blocks (first_lead + i) mod N for i in `offsets`."""
    out = []
    for i in offsets:
        a, b = block(n, count, (first_lead + i) % n)
        out.extend(range(a, b))
    return out


def rounds(n, broken=None):
    """Every round, in order: (phase, s', s), the allgather's in the reduce-
    scatter's reverse - or, broken, in the same order."""
    rs = skips(n)
    ag = rs if broken == 'allgather_order' else list(reversed(rs))
    return [('reduce', a, b) for a, b in rs] + [('gather', a, b) for a, b in ag]


def round_of(rank, n, count, phase, s_prime, s, broken=None):
    """Lead `rank`'s part in one round: (to, sent elements, from, received
    elements, combines)."""
    if phase == 'reduce':
        to, frm = (rank + s) % n, (rank - s) % n
        if broken == 'wrong_lead':
            to = (rank + s + 1) % n if n > 2 else to
        sent = blocks_elements(n, count, rank, range(s, s_prime))
        received = blocks_elements(n, count, rank, range(0, s_prime - s))
        return to, sent, frm, received, True
    to, frm = (rank - s) % n, (rank + s) % n
    sent = blocks_elements(n, count, rank, range(0, s_prime - s))
    received = blocks_elements(n, count, rank, range(s, s_prime))
    return to, sent, frm, received, False


def combine_check(n, count, broken=None):
    """Run the rounds over symbolic vectors; None, or what went wrong."""
    have = [[{lead: 1} for _ in range(count)] for lead in range(n)]
    for phase, s_prime, s in rounds(n, broken):
        parts = [round_of(r, n, count, phase, s_prime, s, broken) for r in range(n)]
        # what each lead sends, read before anything is delivered
        outgoing = {}
        for r, (to, sent, frm, received, combines) in enumerate(parts):
            outgoing[(r, to)] = [(e, dict(have[r][e])) for e in sent]
        for r, (to, sent, frm, received, combines) in enumerate(parts):
            got = outgoing.get((frm, r))
            if got is None or [e for e, _ in got] != received:
                return (f'ROUND: N {n}, {count} elements, {phase} round s = {s}: lead {r} '
                        f'expects {len(received)} elements from lead {frm}, which sends it '
                        f'{"nothing" if got is None else len(got)}')
            for e, value in got:
                if combines:
                    for node, times in value.items():
                        have[r][e][node] = have[r][e].get(node, 0) + times
                else:
                    have[r][e] = dict(value)
    whole = {lead: 1 for lead in range(n)}
    for r in range(n):
        for e in range(count):
            if have[r][e] != whole:
                return f'RESULT: N {n}, {count} elements: lead {r}\'s element {e} is {have[r][e]}'
    return None


def block_start(n, count, j):
    """collective-circulant.c's block_start: j count / n, without the product."""
    return j * (count // n) + j * (count % n) // n


def blocks_intervals(n, count, rank, a, b):
    """collective-circulant.c's blocks(): blocks (rank + a .. rank + b - 1) mod n
    as two half-open intervals, the second empty unless the run wraps."""
    start, run = (rank + a) % n, b - a
    if start + run <= n:
        return (block_start(n, count, start), block_start(n, count, start + run)), (0, 0)
    return (block_start(n, count, start), count), (0, block_start(n, count, start + run - n))


def round_intervals(rank, n, count, rnd):
    """collective-circulant.c's osoe_circulant_round, line for line: (to, from,
    row, combines, the sent intervals, the received intervals)."""
    rs = skips(n)
    rows = len(rs)
    k = rnd if rnd < rows else 2 * rows - 1 - rnd
    outer, inner = rs[k]
    combines = rnd < rows
    if combines:
        to, frm = (rank + inner) % n, (rank - inner) % n
        sent = blocks_intervals(n, count, rank, inner, outer)
        received = blocks_intervals(n, count, rank, 0, outer - inner)
    else:
        to, frm = (rank - inner) % n, (rank + inner) % n
        sent = blocks_intervals(n, count, rank, 0, outer - inner)
        received = blocks_intervals(n, count, rank, inner, outer)
    return to, frm, k, combines, sent, received


def intervals_check(n, count):
    """The C's form against the rounds written out element by element."""
    for rnd, (phase, s_prime, s) in enumerate(rounds(n)):
        for r in range(n):
            to, frm, k, combines, sent, received = round_intervals(r, n, count, rnd)
            want = round_of(r, n, count, phase, s_prime, s)
            elements = lambda iv: [e for a, b in iv for e in range(a, b)]
            if (to, frm, combines) != (want[0], want[2], want[4]) or \
                    sorted(elements(sent)) != sorted(want[1]) or \
                    sorted(elements(received)) != sorted(want[3]):
                return (f'INTERVALS: N {n}, {count} elements, round {rnd}: lead {r}\'s '
                        f'intervals differ')
    return None


def digest(n, count):
    """FNV-1a, 64 bits, over every round's every lead's round_intervals, each
    value as 8 bytes, least significant first: what test-circulant.c computes
    from the C."""
    h = 0xcbf29ce484222325
    for rnd in range(2 * len(skips(n))):
        for r in range(n):
            to, frm, k, combines, sent, received = round_intervals(r, n, count, rnd)
            values = [to, frm, k, int(combines), sent[0][0], sent[0][1], sent[1][0], sent[1][1],
                      received[0][0], received[0][1], received[1][0], received[1][1]]
            for v in values:
                for i in range(8):
                    h ^= (v >> (8 * i)) & 0xff
                    h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def digest_lines():
    lines = ['# nodes elements rounds fnv1a64 (tools/modelcheck/circulant_model.py digest)']
    for n in range(2, 71):
        for count in fold.counts_for(n):
            lines.append(f'{n} {count} {2 * len(skips(n))} {digest(n, count):016x}')
    return lines


def volumes(n, count):
    """Elements each lead sends, and receives, over both halves."""
    sent = [0] * n
    received = [0] * n
    for phase, s_prime, s in rounds(n):
        for r in range(n):
            to, out, frm, inc, combines = round_of(r, n, count, phase, s_prime, s)
            sent[r] += len(out)
            received[r] += len(inc)
    return sent, received


def check():
    failures = []
    cases = 0
    for n in range(2, 71):
        for count in fold.counts_for(n):
            cases += 1
            why = combine_check(n, count) or intervals_check(n, count)
            if why:
                failures.append(why)
            sent, received = volumes(n, count)
            # every lead the same N - 1 blocks each half: 2(N - 1)/N of the
            # vector each way, to within the blocks' rounding
            for r in range(n):
                most = 2 * ((n - 1) * ((count + n - 1) // n))
                if sent[r] > most or received[r] > most:
                    failures.append(f'VOLUME: N {n}, {count} elements: lead {r} sends {sent[r]} '
                                    f'and receives {received[r]}, more than {most}')
            if len(rounds(n)) != 2 * (n - 1).bit_length():
                failures.append(f'ROUND_COUNT: N {n}: {len(rounds(n))} rounds, not 2 ceil(log2 N)')
            # against the fold the brief described (rabenseifner_model.py): the
            # same at a power of two, less on the busiest link at every other N
            if count >= 64 * n:
                f_out, f_in = fold.bytes_sent(n, count)
                c_out = max(sent)
                if (n & (n - 1)) == 0 and c_out > f_out or (n & (n - 1)) and c_out >= f_out:
                    failures.append(f'FOLD: N {n}, {count} elements: the busiest lead sends '
                                    f'{c_out}, the fold\'s {f_out}')
    print(f'arithmetic: {cases} node counts and lengths, {len(failures)} failures')
    for f in failures[:10]:
        print('  ' + f)
    broken_ok = True
    for broken, expect in (('wrong_lead', 'ROUND'), ('allgather_order', 'RESULT')):
        why = next(filter(None, (combine_check(n, count, broken) for n in range(3, 20)
                                 for count in fold.counts_for(n))), None)
        wrong = engine.judge(why is None, engine.named(why), expect)
        print(f'broken {broken}: ' + ('PASSES - the check is blind' if why is None else
                                      f'fails {engine.named(why)}') +
              (', as it must' if wrong is None else '   <-- UNEXPECTED' + wrong))
        broken_ok &= wrong is None
    return not failures and broken_ok


def volume_table():
    """The busiest lead's link, in vectors sent / received, against the fold's."""
    print(' N   circulant      fold (rabenseifner_model)')
    for n in (2, 3, 4, 5, 6, 7, 8, 12, 16, 17, 31, 33, 64, 70):
        count = 1000 * n
        sent, received = volumes(n, count)
        c = (max(sent) / count, max(received) / count)
        fs = fold.bytes_sent(n, count)
        print(f'{n:3d}   {c[0]:.2f} / {c[1]:.2f}    {fs[0] / count:.2f} / {fs[1] / count:.2f}')


# ---- The protocol ------------------------------------------------------------
#
# Each lead's lane has a row per skip: round k of either half moves its pieces
# into row k of the receiver's lane, piece j into slot j mod W, each a put and
# then an add on the receiver's arrival counter for the row. (The C gives each
# round a block of as many of the lane's rows as the rounds leave it, W its
# slots; the model's row is that block.) A round sends to
# one lead and receives from another, so there is no swap to clock a round by:
# the receiver adds a credit at the sender for each piece it takes, and the
# sender puts piece j only once it holds j - W + 1 of the round's credits.
#
# **Only for the pieces the sender waits on**: piece j's credit is what lets
# piece j + W go, so a round of P pieces credits its first P - W and no more.
# A credit for one of the last W would be a write into the sender's words that
# the sender never waits for - it may have returned, and left the team, by the
# time it lands - and a team number is zeroed for its next team on the word
# that nothing of the last is still to arrive (internal-region.h; the team
# redesign's "Why giving a number back is safe"). With them left out, the
# sender's last put of a round waits for every credit of the round, so none
# outlives it. The observer checks it (QUIET): once every lead has returned
# from its last collective, no counter or field of any lead's words has moved
# since that lead returned. The breakage credit_every_piece, the design as
# first written, fails it.
#
# Grants. Lead r's reduce-scatter receivers r + s_k are as many leads as there
# are rounds, and each lends its own lane, so each grant carries a lane of its
# own - into field k of r's grant line, with the collective it names, which
# grant_tag folds in and r checks field by field; the C's field holds the
# epoch, identity and bytes beside the lane, each receiver's alone (as built
# since the review of v0.2 step 4's merge point, Q2-4, where the C had put one
# head every receiver wrote), and no writer or round as the tag does: the
# arithmetic gives each field one writer, the receiver r reads it as from
# (shmem.circulant.arithmetic, to 300 leads), and r clears each field once
# read, so that one not written again reads as none (shmem.circulant-grants.*;
# T40) - and r cannot tell from one counter
# which of them has come: it waits for all of them, at the reduce-scatter's
# start, every receiver granting as it enters. The allgather's round k comes
# from r + s_k into r's row k, which r's reduce-scatter round k used, so r
# grants that sender as its own round k ends - a count, nothing more - and a
# sender waits for all of its allgather grants before its first allgather
# round. Where the allgather's pieces go, the sender learned in the
# reduce-scatter: its allgather round k sends to r - s_k, its reduce-scatter
# round k's sender, which put its own lane into the field for row k of its
# senders' lanes line before its first piece's add, and which the sender read
# as it took that piece - the row's counter says which field is written. Every
# put lands outside the counters a lead watches: the grant's fields past
# its counter, the senders' lanes on a line of their own.
#
# Counters of their own. The allgather ends with no step that every lead
# waits for: a lead whose last pieces have come leaves, and can start the
# team's next collective - a small reduction, whose child adds to its parent's
# arrival counter as it enters - while another still waits for its own last
# pieces. So every counter the reduce-scatter waits on is one no other
# collective adds to: the row counters, the credits and the two grant
# counters are the reduce-scatter's alone. The breakage shared_counters gives
# the tree's arrival counters to the rows and fails; sender_field_late puts a
# sender's lane after its first piece's add, and fails.

ARR = lambda n, k: ('arr', n, k)            # lead n's arrival counter for row k
CRED = lambda n: ('cred', n)                # credits lead n holds for its pieces
RS_GRANT = lambda n: ('rsg', n)             # reduce-scatter grants lead n has had
RS_FIELD = lambda n, k: ('rsf', n, k)       # field k of lead n's reduce-scatter grant line
AG_GRANT = lambda n: ('agg', n)             # allgather grants lead n has had
SENDER_FIELD = lambda n, k: ('snd', n, k)   # field k of lead n's senders' lanes line
SLOT = lambda n, k, j: ('slot', n, k, j)    # lead n's lane: row k, slot j
TREE_ARR = lambda n, p: ('tarr', n, p)      # a tree's arrival counter at lead n, position p
TREE_REL = lambda n: ('trel', n)            # a tree's release counter at lead n
SMALL = lambda n, p: ('small', n, p)        # a tree's small slot at lead n, position p
BIND = lambda n: ('$bind', n)               # ghost: the collective lead n's lane is lent to

REDUCE, GATHER = 0, 1


def piece_tag(c, phase, k, j):
    return ((c * 2 + phase) * 32 + k) * 64 + j + 1


def grant_tag(c, k, sender):
    return ((c * 32 + k) * 128 + sender) + 1


def tree_tag(c, sender):
    return 1 << 20 | c * 128 + sender


def lead_rounds(rank, n, count):
    """Lead `rank`'s rounds in order: (phase, k, to, from, pieces sent,
    pieces received), a piece an element."""
    rs = skips(n)
    out = []
    for phase, s_prime, s in rounds(n):
        k = rs.index((s_prime, s))
        to, sent, frm, received, _ = round_of(rank, n, count, 'reduce' if phase == 'reduce'
                                              else 'gather', s_prime, s)
        out.append((REDUCE if phase == 'reduce' else GATHER, k, to, frm, len(sent),
                    len(received)))
    return out


def lead_program(design, rank, n, count, W, kinds):
    """Lead `rank`'s program: one collective of each kind in `kinds`, in order -
    'circulant', the reduce-scatter and allgather of `count` elements, or
    'tree', a small reduction over a star rooted at lead 0."""
    q = len(skips(n))
    plan = lead_rounds(rank, n, count)
    shared = design == 'shared_counters'
    arrival_of_tree = (lambda m, p: ARR(m, p)) if shared else TREE_ARR
    # cumulative bases, in this lead's order
    arrived = [0] * max(q, n)
    credits = 0
    tree_arrived = [0] * n
    tree_released = 0
    circulants = 0
    base = {}
    for c, kind in enumerate(kinds):
        if kind == 'circulant':
            base[(c, 'grants')] = circulants * q
            circulants += 1
            for phase, k, to, frm, sends, receives in plan:
                base[(c, phase, k)] = (arrived[k], credits)
                arrived[k] += receives
                credits += sends if design == 'credit_every_piece' else max(0, sends - W)
        else:
            if rank == 0:
                base[(c, 'tree')] = list(arrived) if shared else list(tree_arrived)
                for p in range(n - 1):
                    if shared:
                        arrived[p] += 1
                    else:
                        tree_arrived[p] += 1
            else:
                base[(c, 'tree')] = tree_released
                tree_released += 1
    B = []
    for c, kind in enumerate(kinds):
        me = c + 1
        pre = f'c{c}'
        nxt_collective = f'@c{c + 1}E' if c + 1 < len(kinds) else '@RET'
        if kind == 'tree':
            B.append((pre + 'E', ('ghost', lambda M, L, me=me: ({BIND(rank): me}, {}),
                                  f'@{pre}T')))
            if rank != 0:
                p = rank - 1
                B += [(pre + 'T', ('store', lambda L, p=p: SMALL(0, p),
                                   lambda L, c=c: tree_tag(c, rank), True, f'@{pre}TA')),
                      (pre + 'TA', ('add', lambda L, p=p: arrival_of_tree(0, p), lambda L: 1, '_',
                                    f'@{pre}TW')),
                      (pre + 'TW', ('await', lambda M, L, b=base[(c, 'tree')]:
                                    M[TREE_REL(rank)] >= b + 1, f'@{pre}X'))]
            else:
                b = base[(c, 'tree')]
                labels = [f'{pre}T'] + [f'{pre}T{p}' for p in range(1, n - 1)] + [f'{pre}R0']
                for p in range(n - 1):
                    B += [(labels[p], ('await', lambda M, L, p=p, b=b:
                                       M[arrival_of_tree(0, p)] >= b[p] + 1, f'@{pre}TC{p}')),
                          (f'{pre}TC{p}', ('check', lambda M, L, p=p, c=c:
                                           None if M[SMALL(0, p)] == tree_tag(c, p + 1) else
                                           f'UP: lead 0 takes {M[SMALL(0, p)]} from the small '
                                           f'slot of position {p}, waiting for lead {p + 1}\'s '
                                           f'piece of collective {c}', '@' + labels[p + 1]))]
                for p in range(n - 1):
                    B.append((f'{pre}R{p}', ('add', lambda L, p=p: TREE_REL(p + 1), lambda L: 1,
                                             '_', f'@{pre}R{p + 1}' if p + 2 < n else
                                             f'@{pre}X')))
            B.append((pre + 'X', ('ghost', lambda M, L: ({BIND(rank): 0}, {}), nxt_collective)))
            continue

        # ---- a circulant reduction ----
        B.append((pre + 'E', ('ghost', lambda M, L, me=me: ({BIND(rank): me}, {}), f'@{pre}G0')))
        # The reduce-scatter's grants, as this lead enters: to the sender of each
        # of its rounds, into that round's field, then the count.
        rs = [r for r in plan if r[0] == REDUCE]
        for g, (phase, k, to, frm, sends, receives) in enumerate(rs):
            B += [(f'{pre}G{g}', ('store', lambda L, frm=frm, k=k: RS_FIELD(frm, k),
                                  lambda L, c=c, k=k: grant_tag(c, k, rank), True,
                                  f'@{pre}G{g}A')),
                  (f'{pre}G{g}A', ('add', lambda L, frm=frm: RS_GRANT(frm), lambda L: 1, '_',
                                   f'@{pre}G{g + 1}'))]
        gb = base[(c, 'grants')]
        if design == 'no_rs_grant':
            B.append((f'{pre}G{len(rs)}', ('br', lambda L: True, f'@{pre}RS', f'@{pre}RS')))
        else:
            want = (lambda M: M[RS_GRANT(rank)] >= 1) if design == 'stale_rs_grant' else \
                   (lambda M, gb=gb: M[RS_GRANT(rank)] >= gb + q)
            B.append((f'{pre}G{len(rs)}', ('await', lambda M, L, want=want: want(M),
                                           f'@{pre}GC')))

            def fields(M, L, c=c):
                for phase, k, to, frm, sends, receives in rs:
                    if M[RS_FIELD(rank, k)] != grant_tag(c, k, to):
                        return (f'GRANT: lead {rank} reads {M[RS_FIELD(rank, k)]} in its '
                                f'reduce-scatter grant field {k}, waiting for lead {to}\'s '
                                f'grant of collective {c}')
                return None
            B.append((f'{pre}GC', ('check', fields if design != 'stale_rs_grant' else
                                   (lambda M, L: None), f'@{pre}RS')))
        B.append((f'{pre}RS', ('br', lambda L: True, f'@{pre}S0_A', f'@{pre}S0_A')))
        first_gather = next(i for i, r in enumerate(plan) if r[0] == GATHER)
        for at, (phase, k, to, frm, P_send, P_recv) in enumerate(plan):
            arrived_base, held = base[(c, phase, k)]
            q_ = f'{pre}S{at}_'
            nxt = f'@{pre}S{at + 1}_A' if at + 1 < len(plan) else f'@{pre}X'
            if at == first_gather:
                # every allgather grant, before the first allgather round
                ab = base[(c, 'grants')]
                if design == 'one_ag_grant_at_a_time':
                    B.append((q_ + 'A', ('br', lambda L: True, '@' + q_ + 'L', '@' + q_ + 'L')))
                else:
                    B.append((q_ + 'A', ('await', lambda M, L, ab=ab: M[AG_GRANT(rank)] >= ab + q,
                                         '@' + q_ + 'L')))
            elif design == 'early_ag_grant' and phase == REDUCE:
                # the breakage: row k's allgather grant as the round starts,
                # before its pieces are taken, not as it ends
                B.append((q_ + 'A', ('add', lambda L, to=to: AG_GRANT(to), lambda L: 1, '_',
                                     '@' + q_ + 'L')))
            else:
                B.append((q_ + 'A', ('br', lambda L: True, '@' + q_ + 'L', '@' + q_ + 'L')))
            # one allgather grant at a time, as a count: round k's wait is for
            # the count to pass the rounds so far - satisfied by any of them
            ag_rounds_before = sum(1 for r in plan[:at] if r[0] == GATHER)

            def choose(M, L, phase=phase, k=k, to=to, P_send=P_send, P_recv=P_recv,
                       arrived_base=arrived_base, held=held, ag_rounds_before=ag_rounds_before,
                       c=c):
                qs, qc = L['qs'] or 0, L['qc'] or 0
                alts = []
                if qs < P_send:
                    flowing = design == 'no_credit' or qs < W or M[CRED(rank)] >= held + qs - W + 1
                    if design == 'one_ag_grant_at_a_time' and phase == GATHER:
                        flowing = flowing and M[AG_GRANT(rank)] >= base[(c, 'grants')] + \
                            ag_rounds_before + 1
                    if flowing:
                        alts.append(({}, {'go': 1}, f'put piece {qs} of round {k}'))
                if qc < P_recv and M[ARR(rank, k)] >= arrived_base + qc + 1:
                    alts.append(({}, {'go': 2}, f'take piece {qc} of round {k}'))
                if qs == P_send and qc == P_recv:
                    alts.append(({}, {'go': 3}, f'round {k} done'))
                return alts

            B += [(q_ + 'L', ('nd', choose, '@' + q_ + 'GO')),
                  (q_ + 'GO', ('br', lambda L: L['go'] == 1, '@' + q_ + 'PUT', '@' + q_ + 'GO2')),
                  (q_ + 'GO2', ('br', lambda L: L['go'] == 2, '@' + q_ + 'TAKE', '@' + q_ + 'END')),
                  (q_ + 'PUT', ('check', lambda M, L, to=to, me=me, k=k:
                                None if M[BIND(to)] == me else
                                f'LANE: lead {rank} puts piece {L["qs"] or 0} of round {k} into '
                                f'lead {to}, whose lane is lent to collective {M[BIND(to)]}, not '
                                f'{me}', '@' + q_ + 'PUTS')),
                  (q_ + 'PUTS', ('store', lambda L, to=to, k=k: SLOT(to, k, (L['qs'] or 0) % W),
                                 lambda L, c=c, phase=phase, k=k:
                                 piece_tag(c, phase, k, L['qs'] or 0), True,
                                 '@' + q_ + ('PUTF' if phase == REDUCE and
                                             design != 'sender_field_late' else 'PUTA'))),
                  (q_ + 'PUTA', ('add', lambda L, to=to, k=k: ARR(to, k), lambda L: 1, '_',
                                 '@' + q_ + ('PUTF' if phase == REDUCE and
                                             design == 'sender_field_late' else 'PUTN'))),
                  (q_ + 'PUTN', ('set', 'qs', lambda L: (L['qs'] or 0) + 1, '@' + q_ + 'L')),
                  (q_ + 'TAKE', ('check', lambda M, L, c=c, phase=phase, k=k:
                                 None if M[SLOT(rank, k, (L['qc'] or 0) % W)] ==
                                 piece_tag(c, phase, k, L['qc'] or 0) else
                                 f'UP: lead {rank} takes {M[SLOT(rank, k, (L["qc"] or 0) % W)]} '
                                 f'from row {k} slot {(L["qc"] or 0) % W} for piece '
                                 f'{L["qc"] or 0} of {"reduce-scatter" if phase == REDUCE else "allgather"} '
                                 f'round {k}, waiting for {piece_tag(c, phase, k, L["qc"] or 0)}',
                                 '@' + q_ + 'TAKEN')),
                  (q_ + 'TAKEN', ('set', 'qc', lambda L: (L['qc'] or 0) + 1,
                                  '@' + q_ + ('TAKEF' if phase == REDUCE else 'CREDQ'))),
                  # a credit only for a piece the sender waits on: the taken
                  # piece's index, qc - 1, below P - W (or, broken, for every one)
                  (q_ + 'CREDQ', ('br', lambda L, P_recv=P_recv:
                                  design == 'credit_every_piece' or (L['qc'] or 0) <= P_recv - W,
                                  '@' + q_ + 'CRED', '@' + q_ + 'L')),
                  (q_ + 'CRED', ('add', lambda L, frm=frm: CRED(frm), lambda L: 1, '_',
                                 '@' + q_ + 'L'))]
            if phase == REDUCE:
                # The first piece of the round carries the sender's lane: its
                # field at the receiver, put before the first add (or, broken,
                # after it), which the receiver reads as it takes that piece
                # and uses for the allgather's round k back to the sender.
                after_field = 'PUTA' if design != 'sender_field_late' else 'PUTN'
                B += [(q_ + 'PUTF', ('br', lambda L: (L['qs'] or 0) == 0, '@' + q_ + 'PUTF1',
                                     '@' + q_ + after_field)),
                      (q_ + 'PUTF1', ('store', lambda L, to=to, k=k: SENDER_FIELD(to, k),
                                      lambda L, c=c, k=k: grant_tag(c, k, rank), True,
                                      '@' + q_ + after_field)),
                      (q_ + 'TAKEF', ('br', lambda L: (L['qc'] or 0) == 1, '@' + q_ + 'TAKEF1',
                                      '@' + q_ + 'CREDQ')),
                      (q_ + 'TAKEF1', ('check', lambda M, L, c=c, k=k, frm=frm:
                                       None if M[SENDER_FIELD(rank, k)] == grant_tag(c, k, frm)
                                       else f'SENDER: lead {rank} reads '
                                       f'{M[SENDER_FIELD(rank, k)]} in its senders\' lane field '
                                       f'{k} with the first piece of round {k}, waiting for lead '
                                       f'{frm}\'s of collective {c}', '@' + q_ + 'CREDQ'))]
            end = [(q_ + 'END', ('set', 'go', lambda L: DEL, '@' + q_ + 'END1')),
                   (q_ + 'END1', ('set', 'qs', lambda L: DEL, '@' + q_ + 'END2')),
                   (q_ + 'END2', ('set', 'qc', lambda L: DEL, '@' + q_ + 'END3'))]
            if phase == REDUCE and design != 'early_ag_grant':
                # Row k is free: grant the lead that fills it in the allgather,
                # lead + s_k - this round's receiver.
                sender = to
                end.append((q_ + 'END3', ('add', lambda L, sender=sender: AG_GRANT(sender),
                                          lambda L: 1, '_', nxt)))
            else:
                end.append((q_ + 'END3', ('br', lambda L: True, nxt, nxt)))
            B += end
        B.append((pre + 'X', ('ghost', lambda M, L: ({BIND(rank): 0}, {}), nxt_collective)))
    # Returned from its last collective: what its words hold now, for the
    # observer to compare once every lead has returned (QUIET).
    B.append(('RET', ('ghost', lambda M, L: ({('$ret', rank): 1, **{('$q', rank, i): M[key]
                                              for i, key in enumerate(own_words(rank, n, q))}},
                                             {}), '@DONE')))
    B.append(('DONE', ('done',)))
    return B


def own_words(rank, n, q):
    """Every counter and field of lead `rank`'s words that another lead writes."""
    return ([CRED(rank), RS_GRANT(rank), AG_GRANT(rank), TREE_REL(rank)] +
            [ARR(rank, k) for k in range(max(q, n))] +
            [TREE_ARR(rank, p) for p in range(n)] +
            [RS_FIELD(rank, k) for k in range(q)] + [SENDER_FIELD(rank, k) for k in range(q)])


def observer(n):
    """Once every lead has returned from its last collective, nothing written
    into any lead's words has moved since that lead returned: every write into
    them is one their owner waited for (QUIET) - the word a team number given
    back is zeroed on for its next team."""
    q = len(skips(n))

    def quiet(M, L):
        for r in range(n):
            for i, key in enumerate(own_words(r, n, q)):
                if M[key] != M[('$q', r, i)]:
                    return (f'QUIET: lead {r}\'s {key} moved from {M[("$q", r, i)]} to {M[key]} '
                            f'after lead {r} returned from its last collective')
        return None
    return [('W', ('await', lambda M, L: all(M[('$ret', r)] for r in range(n)), '@C')),
            ('C', ('check', quiet, '@D')),
            ('D', ('done',))]


PROTOCOL_CASES = [
    # (name, leads, elements, window, collectives)
    ('two leads, a reduction twice', 2, 4, 1, ('circulant', 'circulant')),
    ('three leads, a reduction twice', 3, 6, 1, ('circulant', 'circulant')),
    ('three leads, a reduction then a small one', 3, 6, 1, ('circulant', 'tree')),
    ('three leads, a small reduction then a reduction', 3, 6, 1, ('tree', 'circulant')),
    ('four leads, a reduction twice', 4, 4, 1, ('circulant', 'circulant')),
    ('five leads, a reduction twice', 5, 5, 1, ('circulant', 'circulant')),
]
# each breakage, and the property it must fail
BREAKAGES = {'no_credit': 'UP', 'no_rs_grant': 'LANE', 'stale_rs_grant': 'LANE',
             'early_ag_grant': 'UP', 'one_ag_grant_at_a_time': 'UP', 'shared_counters': 'UP',
             'sender_field_late': 'SENDER', 'credit_every_piece': 'QUIET'}


def protocol(want):
    """The design in every case, then each breakage in the cases in order until
    one fails."""
    from engine import assemble, explore, report
    ok = True

    def run(design, name, n, count, W, kinds, expect):
        threads = [assemble(lead_program(design, r, n, count, W, kinds)) for r in range(n)]
        threads.append(assemble(observer(n)))
        res = explore(f'{design}: {name}', threads, {}, max_seconds=1800,
                      liveness_max_states=3_000_000, progress=60)
        report(res, expect=expect)
        return res

    for name, n, count, W, kinds in PROTOCOL_CASES:
        if want(f'design: {name}'):
            res = run('design', name, n, count, W, kinds, True)
            ok &= res.complete and res.ok
    for design in BREAKAGES:
        cases = [c for c in PROTOCOL_CASES if want(f'{design}: {c[0]}')]
        if not cases:
            continue
        breakage_failed[design] = None
        for name, n, count, W, kinds in cases:
            res = run(design, name, n, count, W, kinds, None)
            if not res.ok:
                # A limit is recorded by its verdict, which names no property: unexpected.
                breakage_failed[design] = res.failed or res.verdict
                break
    return ok


breakage_failed = {}        # breakage -> what its first failing case fails, or None


def main(argv):
    what = argv[1] if len(argv) > 1 else 'check'
    if what == 'volumes':
        volume_table()
        return 0
    if what == 'digest':
        with open(argv[2], 'w') as f:
            f.write('\n'.join(digest_lines()) + '\n')
        return 0
    if what == 'digest-compare':
        have = open(argv[2]).read().splitlines()
        if have != digest_lines():
            print(f'{argv[2]} is not what circulant_model.py digest writes now: regenerate it')
            return 1
        print(f'{argv[2]}: {len(have) - 1} digests, as the model writes them')
        return 0
    if what not in ('check', 'arithmetic'):
        print(__doc__)
        return 2
    ok = check()
    if what == 'arithmetic':
        return 0 if ok else 1
    subs = argv[2:]
    ok &= protocol(lambda name: not subs or any(s in name for s in subs))
    for design, failed in breakage_failed.items():
        wrong = engine.judge(failed is None, failed, BREAKAGES[design])
        if wrong is None:
            print(f'breakage {design}: fails {failed}, as it must')
        else:
            print(f'breakage {design}: {"fails" if failed else "PASSES - the model is blind to it"}'
                  f'   <-- UNEXPECTED{wrong}')
        ok &= wrong is None
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
