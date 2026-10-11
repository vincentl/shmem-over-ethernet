#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Rabenseifner's reduction over a team's node leads (v0.2 step 3,
docs/prompts/done/v0.2-step3-rabenseifner-prompt.md; the record
docs/decisions/2026-10-06-rabenseifner.md): a reduce-scatter by recursive
halving and an allgather by recursive doubling, with a fold for a node count
that is not a power of two.

Not built. The fold puts up to 3.5 vectors on a fold lead's link (this
model's bytes_sent), and Träff's circulant reduce-scatter - circulant_model.py,
collective-circulant.c - moves N - 1 blocks a half on every lead for every N;
the author chose to model it and build it if the model held it (2026-10-06,
item 6), and it did (docs/decisions/2026-10-07-circulant.md). This file stays as
the evidence of that comparison, which circulant_model.py's check makes on
every run; its C side - collective-rabenseifner.c, its test and digests - was
written on the branch that modelled the fold and not carried over.

Steps, numbered from 0, over N = 2^p + r leads:
  fold       (only when r > 0) the first 2r leads pair as (0, 1), (2, 3), ...:
             the even lead sends the upper half of its vector to the odd, the
             odd its lower half to the even, and each combines what it gets
             into the half it kept.
  fold back  the odd lead sends its combined upper half to the even, which
             then holds the pair's whole combined vector; the odd ones wait.
  halving    p steps over the 2^p leads that remain, renumbered q = 0 ..
             2^p - 1 in order: at the i-th the partner is q ^ 2^(p - i); the
             lower of a pair keeps the lower half of its segment and sends the
             upper, the upper the reverse, and each combines.
  doubling   p steps, the same pairs in reverse order: each sends the segment
             it holds and receives its partner's.
  unfold     each even lead of the fold sends the whole result to its odd one.
A segment is a half-open interval of element indices, split at a + (b - a) / 2,
so the two of a pair compute the same halves.

This file holds the arithmetic, in two forms that must agree: schedule(), every
step's messages, written from the description above; and step_of(), one lead's
part in one step from its rank, the node count and the step alone, as
collective-rabenseifner.c computed it. The check runs the schedule over
symbolic vectors - each element a multiset of the nodes combined into it - for
every N from 2 to 70 at nine lengths: every element of every lead's result is
combined from every node exactly once. Its broken variants must fail: a fold
lead combining the wrong half, and the unfold sent before the allgather's last
step.

The protocol, the design the record's second question proposes, under the
engine's SC interleavings: each lead's lane has R rows of W slots; step i
moves its pieces into row i mod R of the receiver's lane, piece j into slot
j mod W, each a put and then an add on the receiver's arrival counter for the
row, counted cumulatively over the team's life and compared as at least. A
sender puts its first piece of step i only once it holds its partner's grant
for step i and this collective; a receiver grants step i at entry if i < R, and
otherwise once it has combined every piece of step i - R, the row's last use.
Within an exchange step a lead puts its piece q only after combining its
partner's piece q - d, d = W / 2, so that a piece arriving says how far its
sender has combined; within a one-way step the receiver adds a credit at the
sender for every piece it takes, and the sender puts piece q only once it holds
q - W + 1 of them. Each lead runs two collectives in a row. Checked:
  UP     every piece a lead takes from its lane is the one it waits for -
         this collective's, this step's, this index - and an unfold's comes
         from a lead that has finished the allgather;
  LANE   every put lands in a lane lent, at that moment, to the collective
         that puts;
  and, by the engine, no stuck lead and from every state a way to the end.
Breakages, each of which must fail: row_early (a step's grant sent as step
i - R starts, not as it ends), no_grant, stale_grant (any grant the step ever
had taken for this collective's), early_unfold, exchange_no_lag (a piece put
without waiting for the partner's piece q - d) and one_way_no_credit.

Usage:
  rabenseifner_model.py [check] [substring ...]   the arithmetic, then the
                                                  protocol's cases matching
  rabenseifner_model.py digest FILE           write the digests its C test read
  rabenseifner_model.py digest-compare FILE   check FILE is what `digest` writes
"""
import sys

NONE = 0xFFFFFFFF


def shape(n):
    """p = floor(log2 N) and r = N - 2^p, the pairs that fold."""
    p = n.bit_length() - 1
    return p, n - (1 << p)


def step_count(n):
    p, r = shape(n)
    return 2 * p + (3 if r else 0)


def original(q, n):
    """The rank of halving rank q."""
    p, r = shape(n)
    return 2 * q if q < r else q + r


def halving_rank(rank, n):
    """The halving rank of an original rank, or None for a fold's odd lead."""
    p, r = shape(n)
    if rank < 2 * r:
        return rank // 2 if rank % 2 == 0 else None
    return rank - r


def split(a, b):
    m = a + (b - a) // 2
    return (a, m), (m, b)


def held_after(q, done, n, count):
    """The interval halving rank q holds after `done` halving steps."""
    p, _ = shape(n)
    a, b = 0, count
    for i in range(1, done + 1):
        lower, upper = split(a, b)
        a, b = upper if (q >> (p - i)) & 1 else lower
    return a, b


def schedule(n, count, broken=None):
    """Every step's messages, in order: (step, kind, sender, receiver, (a, b))."""
    p, r = shape(n)
    out = []
    s = 0
    if r:
        lower, upper = split(0, count)
        for e in range(0, 2 * r, 2):
            if broken == 'fold_half':
                # the even lead keeps its upper half and sends its lower, as
                # the odd one does: both send the same half
                out.append((s, 'fold', e, e + 1, lower))
            else:
                out.append((s, 'fold', e, e + 1, upper))
            out.append((s, 'fold', e + 1, e, lower))
        s += 1
        for e in range(0, 2 * r, 2):
            out.append((s, 'fold back', e + 1, e, upper))
        s += 1
    unfold = []
    for e in range(0, 2 * r, 2):
        unfold.append(('unfold', e, e + 1, (0, count)))
    for i in range(1, p + 1):
        for q in range(1 << p):
            partner = q ^ (1 << (p - i))
            lower, upper = split(*held_after(q, i - 1, n, count))
            out.append((s, 'halving', original(q, n), original(partner, n),
                        lower if (q >> (p - i)) & 1 else upper))
        s += 1
    for i in range(p, 0, -1):
        if broken == 'early_unfold' and i == 1:
            # the unfold before the allgather's last step
            out += [(s, kind, src, dst, seg) for kind, src, dst, seg in unfold]
            unfold = []
            s += 1
        for q in range(1 << p):
            partner = q ^ (1 << (p - i))
            out.append((s, 'doubling', original(q, n), original(partner, n),
                        held_after(q, i, n, count)))
        s += 1
    out += [(s, kind, src, dst, seg) for kind, src, dst, seg in unfold]
    return out


def combine_check(n, count, broken=None):
    """Every element of every lead's result combined from every node exactly
    once: each step reads what each lead held after the step before, then
    combines (fold, halving) or copies (fold back, doubling, unfold)."""
    have = [[(lead,) for _ in range(count)] for lead in range(n)]
    msgs = schedule(n, count, broken)
    for st in sorted(set(m[0] for m in msgs)):
        batch = [m for m in msgs if m[0] == st]
        sent = [(m, [have[m[2]][e] for e in range(*m[4])]) for m in batch]
        for (step, kind, src, dst, (a, b)), values in sent:
            for off, e in enumerate(range(a, b)):
                if kind in ('fold', 'halving'):
                    have[dst][e] = tuple(sorted(have[dst][e] + values[off]))
                else:
                    have[dst][e] = values[off]
    full = tuple(range(n))
    for lead in range(n):
        for e in range(count):
            if have[lead][e] != full:
                return f'N {n}, {count} elements: lead {lead} element {e} holds {have[lead][e]}'
    return None


def step_of(rank, n, step, count):
    """One lead's part in one step: (partner, sends, receives, combines,
    send_first, send_end, receive_first, receive_end) - collective-
    rabenseifner.c's osoe_rabenseifner_step, line for line."""
    p, r = shape(n)
    none = (NONE, 0, 0, 0, 0, 0, 0, 0)
    at = step
    if r:
        if at <= 1:
            if rank >= 2 * r:
                return none
            even = rank % 2 == 0
            partner = rank + 1 if even else rank - 1
            lower, upper = split(0, count)
            if at == 0:
                send, receive = (upper, lower) if even else (lower, upper)
                return (partner, 1, 1, 1, *send, *receive)
            if even:
                return (partner, 0, 1, 0, 0, 0, *upper)
            return (partner, 1, 0, 0, *upper, 0, 0)
        at -= 2
    q = halving_rank(rank, n)
    if at < 2 * p:
        if q is None:
            return none
        i = at + 1 if at < p else 2 * p - at
        partner = q ^ (1 << (p - i))
        if at < p:
            lower, upper = split(*held_after(q, i - 1, n, count))
            keeps_upper = (q >> (p - i)) & 1
            send, receive = (lower, upper) if keeps_upper else (upper, lower)
            return (original(partner, n), 1, 1, 1, *send, *receive)
        return (original(partner, n), 1, 1, 0, *held_after(q, i, n, count),
                *held_after(partner, i, n, count))
    if rank >= 2 * r:
        return none
    if rank % 2 == 0:
        return (rank + 1, 1, 0, 0, 0, count, 0, 0)
    return (rank - 1, 0, 1, 0, 0, 0, 0, count)


def step_of_check(n, count):
    """step_of() says what schedule() does: each message's sender and
    receiver see it, with its interval, and nothing else is sent."""
    msgs = schedule(n, count)
    seen = set()
    for step, kind, src, dst, (a, b) in msgs:
        s = step_of(src, n, step, count)
        d = step_of(dst, n, step, count)
        if s[0] != dst or not s[1] or (s[4], s[5]) != (a, b):
            return f'N {n}, {count} elements, step {step}: lead {src} sends {s}, not {kind} {a}..{b} to {dst}'
        if d[0] != src or not d[2] or (d[6], d[7]) != (a, b):
            return f'N {n}, {count} elements, step {step}: lead {dst} receives {d}, not {kind} {a}..{b} from {src}'
        combines = kind in ('fold', 'halving')
        if d[3] != combines:
            return f'N {n}, {count} elements, step {step}: lead {dst} combines {d[3]} in a {kind}'
        seen.add((step, src, 'send'))
        seen.add((step, dst, 'receive'))
    for step in range(step_count(n)):
        for rank in range(n):
            t = step_of(rank, n, step, count)
            if t[1] and (step, rank, 'send') not in seen:
                return f'N {n}, step {step}: lead {rank} sends where the schedule has nothing'
            if t[2] and (step, rank, 'receive') not in seen:
                return f'N {n}, step {step}: lead {rank} receives where the schedule has nothing'
    return None


def counts_for(n):
    return sorted({c for c in (1, 2, 3, n - 1, n, n + 1, 2 * n + 3, 64, 1000) if c >= 1})


def digest(n, count):
    """FNV-1a, 64 bits, over every step's every lead's step_of(), each value
    as 8 bytes, least significant first: what collective-rabenseifner.c's test,
    not carried over,
    computes from the C."""
    h = 0xcbf29ce484222325
    for step in range(step_count(n)):
        for rank in range(n):
            for v in step_of(rank, n, step, count):
                for i in range(8):
                    h ^= (v >> (8 * i)) & 0xff
                    h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def digest_lines():
    lines = ['# nodes elements steps fnv1a64 (tools/modelcheck/rabenseifner_model.py digest)']
    for n in range(2, 71):
        for count in counts_for(n):
            lines.append(f'{n} {count} {step_count(n)} {digest(n, count):016x}')
    return lines


def bytes_sent(n, count):
    """The most elements any one lead sends, and the most it receives."""
    out, inn = {}, {}
    for step, kind, src, dst, (a, b) in schedule(n, count):
        out[src] = out.get(src, 0) + (b - a)
        inn[dst] = inn.get(dst, 0) + (b - a)
    return max(out.values()), max(inn.values())


# ---- The protocol ------------------------------------------------------------

ARR = lambda n, r: ('arr', n, r)            # lead n's arrival counter for row r
CRED = lambda n: ('cred', n)                # credits lead n holds for its one-way pieces
GRANT = lambda n, i: ('grant', n, i)        # grants lead n has had for its pieces of step i
SLOT = lambda n, r, j: ('slot', n, r, j)    # lead n's lane: row r, slot j
BIND = lambda n: ('$bind', n)               # ghost: the collective lead n's lane is lent to
GATHERED = lambda n: ('$gathered', n)       # ghost: lead n holds the whole result


def tag(c, i, q, complete):
    """A piece: collective c, step i, index q, and whether its sender had
    finished the allgather when it put it."""
    return ((c * 64 + i) * 64 + q) * 2 + complete


def lead_program(design, n, nodes, count, W, R, collectives):
    """Lead n's program: `collectives` reductions of `count` elements, a
    piece an element."""
    from engine import DEL
    S = step_count(nodes)
    p, r = shape(nodes)
    d = W // 2
    order = list(range(S))
    if design == 'early_unfold' and r:
        order = order[:S - 2] + [S - 1, S - 2]      # the unfold before the last doubling
    last_doubling = S - 2 if r else S - 1
    steps = {i: step_of(n, nodes, i, count) for i in range(S)}
    # cumulative bases: what each row's arrival counter and the credits hold
    # before each step of each collective, in this lead's order
    received = [0] * R
    credits = 0
    base = {}
    for c in range(collectives):
        for i in order:
            partner, sends, receives, combines, sf, se, rf, re = steps[i]
            base[(c, i)] = (received[i % R], credits)
            if receives:
                received[i % R] += re - rf
            if sends and not receives:
                credits += se - sf
    B = []
    for c in range(collectives):
        me = c + 1
        pre = f'c{c}'
        B.append((pre + 'E', ('ghost', lambda M, L, me=me: ({BIND(n): me}, {}), f'@{pre}G0')))
        # the grants of the first R steps, as this lead enters
        grants = [i for i in range(min(R, S)) if steps[i][2]]
        if design == 'no_grant':
            grants = []
        for g, i in enumerate(grants):
            B.append((f'{pre}G{g}', ('add', lambda L, i=i: GRANT(steps[i][0], i), lambda L: 1, '_',
                                    f'@{pre}G{g + 1}')))
        B.append((f'{pre}G{len(grants)}', ('br', lambda L: True, f'@{pre}S{order[0]}_A',
                                          f'@{pre}S{order[0]}_A')))
        for at, i in enumerate(order):
            partner, sends, receives, combines, sf, se, rf, re = steps[i]
            P_send, P_recv = se - sf, re - rf
            row = i % R
            arrived, held = base[(c, i)]
            one_way = sends != receives
            unfold = r and i == S - 1
            nxt = f'@{pre}S{order[at + 1]}_A' if at + 1 < len(order) else f'@{pre}X'
            q = f'{pre}S{i}_'
            # the grant of step i + R: as this step ends (the design), or as it starts
            later = i + R if i + R < S and steps[i + R][2] and design != 'no_grant' else None
            if later is not None and design == 'row_early':
                B.append((q + 'A', ('add', lambda L, j=later: GRANT(steps[j][0], j),
                                    lambda L: 1, '_', '@' + q + 'L')))
            else:
                B.append((q + 'A', ('br', lambda L: True, '@' + q + 'L', '@' + q + 'L')))

            def choose(M, L, i=i, c=c, partner=partner, P_send=P_send, P_recv=P_recv, row=row,
                       arrived=arrived, held=held, one_way=one_way):
                qs, qc = L['qs'] or 0, L['qc'] or 0
                alts = []
                if qs < P_send:
                    if design == 'no_grant':
                        granted = True
                    elif design == 'stale_grant':
                        granted = M[GRANT(n, i)] >= 1
                    else:
                        granted = M[GRANT(n, i)] >= c + 1
                    if one_way:
                        flowing = (design == 'one_way_no_credit' or qs < W or
                                   M[CRED(n)] >= held + qs - W + 1)
                    else:
                        flowing = design == 'exchange_no_lag' or qs < d or qc >= qs - d + 1
                    if granted and flowing:
                        alts.append(({}, {'go': 1}, f'put piece {qs} of step {i}'))
                if qc < P_recv and M[ARR(n, row)] >= arrived + qc + 1:
                    alts.append(({}, {'go': 2}, f'take piece {qc} of step {i}'))
                if qs == P_send and qc == P_recv:
                    alts.append(({}, {'go': 3}, f'step {i} done'))
                return alts

            def expected(L, c=c, i=i, unfold=unfold):
                return tag(c, i, L['qc'] or 0, 1 if unfold else 0)

            B += [(q + 'L', ('nd', choose, '@' + q + 'GO')),
                  (q + 'GO', ('br', lambda L: L['go'] == 1, '@' + q + 'PUT', '@' + q + 'GO2')),
                  (q + 'GO2', ('br', lambda L: L['go'] == 2, '@' + q + 'TAKE', '@' + q + 'END')),
                  (q + 'PUT', ('check', lambda M, L, partner=partner, me=me, i=i:
                               None if M[BIND(partner)] == me else
                               f'LANE: lead {n} puts piece {L["qs"] or 0} of step {i} into lead '
                               f'{partner}, whose lane is lent to collective {M[BIND(partner)]}, '
                               f'not {me}', '@' + q + 'PUTG')),
                  (q + 'PUTG', ('gpre', lambda M, L, unfold=unfold:
                                ({}, {'cb': M[GATHERED(n)] if unfold else 0}), '@' + q + 'PUTS')),
                  (q + 'PUTS', ('store', lambda L, partner=partner, row=row:
                                SLOT(partner, row, (L['qs'] or 0) % W),
                                lambda L, c=c, i=i: tag(c, i, L['qs'] or 0, L['cb']), True,
                                '@' + q + 'PUTA')),
                  (q + 'PUTA', ('add', lambda L, partner=partner, row=row: ARR(partner, row),
                                lambda L: 1, '_', '@' + q + 'PUTN')),
                  (q + 'PUTN', ('set', 'qs', lambda L: (L['qs'] or 0) + 1, '@' + q + 'PUTD')),
                  (q + 'PUTD', ('set', 'cb', lambda L: DEL, '@' + q + 'L')),
                  (q + 'TAKE', ('check', lambda M, L, row=row, i=i, expected=expected:
                                None if M[SLOT(n, row, (L['qc'] or 0) % W)] == expected(L) else
                                f'UP: lead {n} takes {M[SLOT(n, row, (L["qc"] or 0) % W)]} from '
                                f'row {row} slot {(L["qc"] or 0) % W} for piece {L["qc"] or 0} of '
                                f'step {i}, waiting for {expected(L)}', '@' + q + 'TAKEN')),
                  (q + 'TAKEN', ('set', 'qc', lambda L: (L['qc'] or 0) + 1,
                                 '@' + q + ('CRED' if one_way and receives else 'L')))]
            if one_way and receives:
                B.append((q + 'CRED', ('add', lambda L, partner=partner: CRED(partner),
                                       lambda L: 1, '_', '@' + q + 'L')))
            # The step's end: its row is free, so the grant of step i + R goes
            # out; the allgather's end is marked; the counts are forgotten.
            end = [(q + 'END', ('set', 'go', lambda L: DEL, '@' + q + 'END1')),
                   (q + 'END1', ('set', 'qs', lambda L: DEL, '@' + q + 'END2')),
                   (q + 'END2', ('set', 'qc', lambda L: DEL, '@' + q + 'END3'))]
            tail = nxt
            if i == last_doubling:
                end.append((q + 'END3', ('ghost', lambda M, L: ({GATHERED(n): 1}, {}),
                                         '@' + q + 'END4')))
                last = q + 'END4'
            else:
                last = q + 'END3'
            if later is not None and design != 'row_early':
                end.append((last, ('add', lambda L, j=later: GRANT(steps[j][0], j),
                                   lambda L: 1, '_', tail)))
            else:
                end.append((last, ('br', lambda L: True, tail, tail)))
            B += end
        # returning: the lane is free; the next collective starts afresh
        B += [(pre + 'X', ('ghost', lambda M, L: ({BIND(n): 0, GATHERED(n): 0}, {}),
                           f'@c{c + 1}E' if c + 1 < collectives else '@DONE'))]
    B.append(('DONE', ('done',)))
    return B


PROTOCOL_CASES = [
    # (name, nodes, elements, window, rows)
    ('two leads, 3 pieces a half', 2, 6, 2, 2),
    ('three leads, the fold', 3, 6, 2, 2),
    ('four leads, a power of two', 4, 4, 2, 2),
    ('four leads, three rows', 4, 4, 2, 3),
    ('five leads, the fold and two halvings', 5, 4, 2, 2),
]
BREAKAGES = ['row_early', 'no_grant', 'stale_grant', 'early_unfold', 'exchange_no_lag',
             'one_way_no_credit']


def protocol(want):
    """The design in every case, then each breakage in the cases in order until
    one fails: a breakage need fail once, and the cases it cannot reach - an
    early unfold where there is no fold - would only cost the time of the
    design's own run."""
    from engine import assemble, explore, report
    ok = True

    def run(design, name, nodes, count, W, R, expect):
        threads = [assemble(lead_program(design, n, nodes, count, W, R, 2)) for n in range(nodes)]
        res = explore(f'{design}: {name}', threads, {}, max_seconds=900,
                      liveness_max_states=3_000_000, progress=60)
        report(res, expect_ok=expect)
        return res

    for name, nodes, count, W, R in PROTOCOL_CASES:
        if want(f'design: {name}'):
            res = run('design', name, nodes, count, W, R, True)
            ok &= res.complete and res.ok
    for design in BREAKAGES:
        cases = [c for c in PROTOCOL_CASES if want(f'{design}: {c[0]}')]
        if not cases:
            continue
        breakage_failed[design] = False
        for name, nodes, count, W, R in cases:
            res = run(design, name, nodes, count, W, R, None)
            if not res.ok:
                breakage_failed[design] = True
                break
    return ok


breakage_failed = {}


def main(argv):
    mode = argv[1] if len(argv) > 1 else 'check'
    if mode == 'digest':
        with open(argv[2], 'w') as f:
            f.write('\n'.join(digest_lines()) + '\n')
        return 0
    if mode == 'digest-compare':
        have = open(argv[2]).read().splitlines()
        if have != digest_lines():
            print(f'{argv[2]} is not what rabenseifner_model.py digest writes now: regenerate it')
            return 1
        print(f'{argv[2]}: {len(have) - 1} digests, as the model writes them')
        return 0
    if mode != 'check':
        print(__doc__)
        return 2
    ok = True
    failures = []
    for n in range(2, 71):
        for count in counts_for(n):
            why = combine_check(n, count) or step_of_check(n, count)
            if why:
                failures.append(why)
    print(f'arithmetic, N 2 to 70 at {len(counts_for(70))} lengths: '
          f'{"every element combined from every node once" if not failures else failures[:3]}')
    ok = ok and not failures
    for broken in ('fold_half', 'early_unfold'):
        caught = [why for n in (3, 5, 6, 7, 12) for count in counts_for(n)
                  for why in [combine_check(n, count, broken)] if why]
        print(f'broken {broken}: {"fails: " + caught[0] if caught else "PASSES - the check cannot see it"}')
        ok = ok and bool(caught)
    for n in (4, 8, 16, 7, 33, 70):
        o, i = bytes_sent(n, 1 << 20)
        print(f'   N {n:2d}: the busiest lead sends {o / (1 << 20):.2f} and receives '
              f'{i / (1 << 20):.2f} vectors')
    pats = argv[2:]
    want = lambda name: not pats or any(x in name for x in pats)
    ok = protocol(want) and ok
    for design in BREAKAGES:
        if design in breakage_failed:
            print(f'breakage {design}: {"fails" if breakage_failed[design] else "NEVER FAILS"}')
            ok = ok and breakage_failed[design]
    print('ok' if ok else 'FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
