#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""An allgather among node leads, for fcollect's internode phase (design §5.8;
internode.h, osoe_internode_collect).

The author's decision of 2026-09-25: a gather whose steps are log N, moving each
node's blocks once, in place of the reduction's tree running an or. Bruck's
algorithm among the team's N node leads: at step s, lead i sends every share it
holds - its own and those gathered so far, nodes i to i + c - 1 with
c = min(2^s, N - 2^s) - to lead i - 2^s, and receives nodes i + 2^s onwards
from lead i + 2^s; after ceil(log2 N) steps every lead holds every share. The
shares are written straight to their places in the receiver's buffer, the
lead's dest, so nothing is staged and nothing rotated.

Two rules make it safe, one per counter a lead keeps per step:
  READY  internode.h's rule: a peer's buffer is written only once that peer has
         entered this collective, since until then its members may still be
         copying the collective before's result out of it. At entry a lead adds
         one to READY[s] at each lead that will write to it, i + 2^s; the
         writer waits for it before its step-s put.
  DATA   a lead forwards at step s + 1 shares it received at step s, so it waits
         for the step-s writer's add, fenced after the put, before sending on;
         and it returns only after the last step's.
Counters are cumulative over the team's life and compared as at least: each
collective adds exactly one to each, so collective x waits for x + 1.

Checked - each check's message begins with the name of the property it holds,
and each breakage below names the one it must fail, which report compares
(engine.py's Properties; the arithmetic's by judge, as the engine's results are):
  DEST     a lead writes a peer's buffer only while the peer is in the same
           collective: never during the peer's members' copy-out of the one
           before.
  FORWARD  a share a lead sends, or copies out of a staging slot, carries this
           collective's value.
  RESULT   on return every share of the buffer carries this collective's value,
           and it still does when the members finish copying it out.
  PLACE    (the placed cases, below) every byte of the result, [0, total), is
           written once in a collective - by its own lead's copy of its
           members' blocks, or by one put - at its place, with its owner's
           value, and holds it at return; in the ranged cases none past the
           result's end, and the lead holds the whole result as ranges.
  RANGE    (the ranged cases, below) the ranges a lead sends at a step are the
           bytes of the shares it sends there in this collective, the last
           step's cut at its receiver's own share; every record it takes is
           this collective's, the ranges its writer sent, and continues what
           it holds; and the share its last-step receiver put at entry is that
           lead's own in this collective.
  SCAN     (ranged_scan) the base each lead takes from the leads' scan, in
           team-rank order, and the total are this collective's; and, as
           scan_model.py has them, SLOT - no small slot written while it holds
           a value not yet taken - and VALUE - a value taken is this
           collective's.
  and, by the engine: no stuck lead, and every state can still finish.

Designs
  design               as above.
  placed               shares of differing lengths, a zero among them, placed
                       from each node's total (below).
  ranged               the leads' scan abstracted - a lead enters knowing its
                       own share's range and the total - and each step carrying
                       its ranges (below).
  ranged_scan          the same, the leads' scan itself first, through the team
                       number's small slots and words.
Breakages
  no_ready             a lead writes without waiting for READY (DEST, before
                       RESULT would see it).
  no_data_wait         a lead forwards without waiting for DATA (FORWARD).
  short_steps          floor(log2 N) steps rather than the ceiling: on a node
                       count that is not a power of two some shares never
                       arrive (RESULT).
  bases_in_node_order  the placed design with the bases summed in position
                       order, not team-rank order: under a negative stride a
                       node's share lands at another's place (PLACE); under a
                       positive one the two orders are one, and it holds.
  range_before_data    a lead takes a step's record before DATA says it landed
                       (RANGE: the collective before's record, or none).
  range_before_ready   a writer puts its record ahead of its READY wait, the
                       record being the library's own word and not the
                       receiver's buffer: on four nodes lead 1 finishes
                       collective 0 while lead 0 has yet to take its step-0
                       record, enters collective 1 and puts that one's over it
                       (RANGE). With the wait the case cannot arise - a
                       record of collective x + 1 waits for its receiver's
                       entry into it, after that lead took every record of x.
  wrap_as_one          a run of shares wrapping past position N - 1 sent as one
                       range of its whole length from where it begins (RANGE).
  no_cut               the last step sends all a lead holds, not cut at its
                       receiver's own share: on three nodes a share the
                       receiver holds already goes again (RANGE).

Placed from each node's total (collect's places, P29 of
docs/prompts/v0.2-open-items.md; the author's A11, 2026-10-11): a share is a
node's members' blocks, of any length, and the buffer is bytes [0, total). A
team whose every node holds one run of consecutive team ranks - one strided in
world ranks over a block distribution - takes each node's share's place from
the nodes' totals alone: its base is the sum of the totals of the nodes before
it in team-rank order, which is the positions' order under a positive stride
and its reverse under a negative one, the node set being sorted by node and the
ranks walked in their own order. A step's shares are their byte ranges in
team-rank order, merged where they touch, as the C code's put_step merges them:
two ranges where a run of shares wraps past position N - 1, and none for a share
of no bytes. Each put writes its range in one step, the bytes the writer holds
there.

The leads' scan, each step carrying its ranges (P30 of
docs/prompts/v0.2-open-items.md, the author's A11; stage 1, designed and
modelled, not built - docs/design/collect-scan-proposal.md). Where the placed
cases give every lead every node's base, here a lead knows only its own share's
range, [base, base + its total), and the total, from a scan of the nodes'
totals among the leads, and each step's puts carry a record of their ranges, so
that no lead needs another node's places. A lead's shares are consecutive
positions, whose bytes - the positions in team-rank order being the node set's
or its reverse - are one run of the result, up from its base under a positive
stride and down from its end under a negative one, wrapping past position
N - 1 at most once. So a lead keeps what it holds as two ranges, `first` up to
the wrap and `second` past it, each (start, end) and, while empty, kept at the
byte where its bytes will begin. At step s it sends both - at the last step,
which sends N - 2^(S-1) shares, fewer than it holds unless N is a power of two,
cut where its receiver's own share begins, which that receiver put past this
lead's READY[S - 1] counter at entry, fenced before the add - then puts the
record, the collective's epoch and the two ranges, past the receiver's DATA[s]
counter, and then the fenced add. The receiver takes it once DATA[s] says it
landed: while i + 2^s is below N, the writer's `first` continues the receiver's
`first` and its `second` the receiver's `second`; past that, every position the
writer sends lies past the receiver's wrap, and its `first` continues the
receiver's `second`. A record is put only after its writer's READY wait, which
the receiver satisfies for collective x + 1 only once it has entered it, after
taking every record of x: so none is overwritten before it is taken.

The leads' scan (ranged_scan): a pre-order tree over the positions rooted at
position 0, as the member scans' is over ranks (scan_model.py), every subtree a
contiguous range of positions; up, a subtree's total into the parent's small
slot for its position and a fenced add of that position's arrival counter;
down, (the totals before the subtree, the total) into each child's small
release slot and a fenced add of its release counter. A lead's base is the
totals before it under a positive stride, and the total less those and its
own under a negative one. The model's tree is the same every collective, so a
counter's collective x waits for x + 1; the code keeps the reductions' bases, a
child position's each.

The arithmetic, held whole before any interleaving (ranged_arithmetic): every
node count from 2 to 70, seven layouts - every share alike, every other one
empty, one holding every byte at either end and in the middle, a spread with
zeros, none - under both strides: the scan at every fan-out from 2 to 8 gives
every lead its base in team-rank order and the total, every step's ranges are
its shares' bytes, every record continues what its receiver holds, and every
lead ends holding the whole result. Seven nodes is the first count whose last
step cuts inside a range a lead received. Its breakages must fail, each the
property it names: the scan on the reductions' tree, whose subtrees are not
contiguous (heap_tree, SCAN), no_cut and wrap_as_one (RANGE).

Staged (proposed to the author, 2026-09-26, from the review of step 5 slice 6):
step 0's one share goes into one of two slots at the receiver, chosen by the
collective's parity, with no READY wait, and the receiver copies it into its
buffer once DATA[0] says it landed; READY[0] is not sent; steps 1 on are as
above. It takes READY's hop off a small gather's path. Safe because a lead
writing in collective x has finished x - 1, which no lead finishes before every
lead has entered it - so every lead has finished x - 2, the slot's last user.
  staged              the design (SLOT: a staged share is never overwritten
                      before its receiver copied it).
  staged_one_slot     one slot, not two: at four nodes a lead finishes x and
                      overwrites the slot its receiver has not copied (SLOT).
                      At N with N - 1 a power of two, lead i - 1 feeds lead i
                      directly at the last step, and one slot happens to hold.
  staged_early_copy   the receiver copies before DATA[0] (FORWARD).

Sending the last step's full 2^s shares, past N - 2^s, is not a breakage: the
extra shares wrap onto ones the receiver already holds, with the same values.
The model found that before this line said it: the bound saves bytes, and
nothing depends on it. (PLACE counts writes, so it would name them in the
placed cases; the C code sends c.)

Every counter add is fenced after the data it covers, and waits are abstract
(the region wait's wake is wait_word_model.py's); runs are SC.

Usage: allgather_model.py [substring ...]   runs the cases whose names match;
       "ranged arithmetic" names the arithmetic's lines
"""
import sys

from engine import DEL, assemble, explore, judge, named, report, select

BUF = lambda n, k: ('buf', n, k)         # lead n's buffer: share k's value, or byte k's when placed
READY = lambda n, s: ('ready', n, s)     # at lead n: step s's receiver has entered
DATA = lambda n, s: ('data', n, s)       # at lead n: step s's shares have landed
IN = lambda n: ('$in', n)                # ghost: the collective lead n is in, + 1; 0 between
SLOT = lambda n, p: ('slot', n, p)       # at lead n: step 0's share, staged, slot p of two
STAGED = lambda n, p: ('$staged', n, p)  # ghost: collective + 1 whose share slot p holds uncopied
WRITES = lambda n, x, b: ('$writes', n, x, b)  # ghost: writes of lead n's byte b in collective x
FIN = ('$fin',)


def steps_of(nodes):
    s = 0
    while (1 << s) < nodes:
        s += 1
    return s


def value(x, k):
    """Share k's value in collective x: distinct across collectives and shares."""
    return 100 * (x + 1) + k


def lead(design, i, nodes, collectives):
    S = steps_of(nodes)
    if design == 'short_steps':
        S = max(1, (nodes.bit_length() - 1))
    B = []
    for x in range(collectives):
        p = f'l{i}x{x}_'
        nxt = f'@l{i}x{x + 1}_E' if x + 1 < collectives else '@DONE'
        # Enter: the members' blocks are in (this node's share), then tell every
        # lead that will write here, one per step.
        B += [(p + 'E', ('ghost', lambda M, L, x=x: ({IN(i): x + 1}, {}), '@' + p + 'E0')),
              (p + 'E0', ('store', lambda L: BUF(i, i), lambda L, x=x: value(x, i), True,
                          '@' + p + 'R0'))]
        staged = design.startswith('staged')
        first_ready = 1 if staged else 0
        if first_ready >= S:
            B += [(f'{p}R{first_ready}', ('br', lambda L: True, f'@{p}S0', f'@{p}S0'))]
        for s in range(first_ready, S):
            writer = (i + (1 << s)) % nodes
            nx = f'@{p}R{s + 1}' if s + 1 < S else f'@{p}S0'
            B += [(f'{p}R{s}', ('add', lambda L, w=writer, s=s: READY(w, s), lambda L: 1, '_', nx))]
        if first_ready != 0:
            B += [(p + 'R0', ('br', lambda L: True, f'@{p}R{first_ready}', f'@{p}R{first_ready}'))]
        # The steps.
        for s in range(S):
            to = (i - (1 << s)) % nodes
            c = min(1 << s, nodes - (1 << s))
            shares = [(i + j) % nodes for j in range(c)]
            q = f'{p}S{s}'
            if staged and s == 0:
                # Step 0's one share, i's own, into the receiver's slot for this
                # collective's parity, with no wait: the slot's last user was
                # collective x - 2, which every lead has finished once this lead
                # has finished x - 1 (a lead cannot finish x - 1 before every
                # lead has entered it).
                parity = (lambda x: 0) if design == 'staged_one_slot' else (lambda x: x % 2)
                slot = parity(x)
                def put_slot(M, L, to=to, slot=slot, x=x):
                    if M[STAGED(to, slot)]:
                        return [({('$overwrite',): 1}, {}, 'overwrite')]
                    return [({SLOT(to, slot): value(x, i), STAGED(to, slot): x + 1}, {},
                             f'stage share {i} in lead {to}\'s slot {slot}')]
                mine = (i + 1) % nodes
                B += [(q, ('nd', put_slot, f'@{q}O')),
                      (f'{q}O', ('check', lambda M, L, to=to, slot=slot:
                                 None if not M[('$overwrite',)] else
                                 f'SLOT: lead {i} overwrote lead {to}\'s slot {slot} before '
                                 f'lead {to} copied the share staged there', f'@{q}D')),
                      (f'{q}D', ('add', lambda L, to=to: DATA(to, 0), lambda L: 1, '_', f'@{q}A'))]
                # This lead's own staged share, from lead i + 1: once DATA says it
                # landed, copied into place; then on to step 1.
                nx = f'@{p}S1' if S > 1 else f'@{p}RET'
                if design == 'staged_early_copy':
                    B += [(f'{q}A', ('br', lambda L: True, f'@{q}C', f'@{q}C'))]
                else:
                    B += [(f'{q}A', ('await', lambda M, L, x=x: M[DATA(i, 0)] >= x + 1, f'@{q}C'))]
                B += [(f'{q}C', ('check', lambda M, L, slot=slot, x=x, mine=mine:
                                 None if M[SLOT(i, slot)] == value(x, mine) else
                                 f'FORWARD: lead {i} copied share {mine} out of its slot {slot} as '
                                 f'{M[SLOT(i, slot)]}, not collective {x}\'s {value(x, mine)}',
                                 f'@{q}K')),
                      (f'{q}K', ('nd', lambda M, L, slot=slot, x=x, mine=mine: [(
                          {BUF(i, mine): M[SLOT(i, slot)], STAGED(i, slot): 0}, {},
                          f'copy share {mine} out of slot {slot}')], nx))]
                continue
            # Wait for the receiver's entry, then write, then say so.
            if design == 'no_ready':
                B += [(q, ('br', lambda L: True, f'@{q}W0', f'@{q}W0'))]
            else:
                B += [(q, ('await', lambda M, L, s=s, x=x: M[READY(i, s)] >= x + 1, f'@{q}W0'))]
            for j, k in enumerate(shares):
                nx = f'@{q}W{j + 1}' if j + 1 < len(shares) else f'@{q}D'
                B += [(f'{q}W{j}', ('check', lambda M, L, to=to, x=x:
                                    None if M[IN(to)] == x + 1 else
                                    f'DEST: lead {i} wrote lead {to}\'s buffer in collective {x} '
                                    f'while lead {to} was ' +
                                    (f'in collective {M[IN(to)] - 1}' if M[IN(to)]
                                     else 'outside it: not yet entered, or its members copying '
                                          'out'),
                                    f'@{q}V{j}')),
                      (f'{q}V{j}', ('check', lambda M, L, k=k, x=x:
                                    None if M[BUF(i, k)] == value(x, k) else
                                    f'FORWARD: lead {i} forwarded share {k} as {M[BUF(i, k)]}, '
                                    f'not collective {x}\'s {value(x, k)}', f'@{q}P{j}')),
                      (f'{q}P{j}', ('store', lambda L, to=to, k=k: BUF(to, k),
                                    lambda L, k=k, x=x: value(x, k), True, nx))]
            B += [(f'{q}D', ('add', lambda L, to=to, s=s: DATA(to, s), lambda L: 1, '_', f'@{q}A'))]
            # Before the next step or the return: this step's shares are in.
            if design == 'no_data_wait' and s + 1 < S:
                B += [(f'{q}A', ('br', lambda L: True, f'@{p}S{s + 1}', f'@{p}S{s + 1}'))]
            else:
                nx = f'@{p}S{s + 1}' if s + 1 < S else f'@{p}RET'
                B += [(f'{q}A', ('await', lambda M, L, s=s, x=x: M[DATA(i, s)] >= x + 1, nx))]
        # Return: the members copy the result out, then the lead may enter the next.
        B += [(p + 'RET', ('ghost', lambda M, L: ({IN(i): 0}, {}), '@' + p + 'C0'))]
        for k in range(nodes):
            nx = f'@{p}C{k + 1}' if k + 1 < nodes else nxt
            B += [(f'{p}C{k}', ('check', lambda M, L, k=k, x=x:
                                None if M[BUF(i, k)] == value(x, k) else
                                f'RESULT: lead {i}\'s members copied share {k} as {M[BUF(i, k)]}, '
                                f'not '
                                f'collective {x}\'s {value(x, k)}', nx))]
    B += [('DONE', ('gpost', lambda M, L: ({FIN: M[FIN] + 1}, {}), '@X')),
          ('X', ('done',))]
    return assemble(B)


def case(design, nodes, collectives):
    threads = [lead(design, i, nodes, collectives) for i in range(nodes)]
    mem = {FIN: 0}
    for n in range(nodes):
        mem[IN(n)] = 0
        for s in range(steps_of(nodes)):
            mem[READY(n, s)] = mem[DATA(n, s)] = 0
        for k in range(nodes):
            mem[BUF(n, k)] = 0
        for q in range(2):
            mem[SLOT(n, q)] = 0
            mem[STAGED(n, q)] = 0
    mem[('$overwrite',)] = 0
    return f'allgather/{design} nodes={nodes} collectives={collectives}', threads, mem, nodes


# ---- placed from each node's total (the docstring's section of that name) -------


def placed_value(x, k, j):
    """Byte j of node k's share in collective x: distinct across collectives,
    shares and bytes, and never 0, which a buffer holds before any write."""
    return 100 * (x + 1) + 10 * k + j


def team_order(nodes, stride):
    """The positions in team-rank order, the order in which the team's ranks meet
    the nodes' runs: the positions' own under a positive stride, and their
    reverse under a negative one."""
    return list(range(nodes)) if stride > 0 else list(range(nodes - 1, -1, -1))


def bases(lengths, order):
    """Each position's base - the lengths of the shares before it in `order` -
    and the total."""
    base, at = {}, 0
    for k in order:
        base[k] = at
        at += lengths[k]
    return base, at


class Layout:
    """A placed case's shares: their true places, the bases summed in team-rank
    order, to which every check holds the buffers; and the places a lead of the
    design computes, which it copies and puts at."""

    def __init__(self, design, lengths, stride):
        self.lengths = lengths
        self.nodes = len(lengths)
        self.order = team_order(self.nodes, stride)
        self.rank = {k: r for r, k in enumerate(self.order)}
        self.true_base, self.total = bases(lengths, self.order)
        summed = range(self.nodes) if design == 'bases_in_node_order' else self.order
        self.base, _ = bases(lengths, summed)
        self.owner = {self.true_base[k] + j: (k, j)
                      for k in range(self.nodes) for j in range(lengths[k])}

    def expected(self, x, b):
        """What byte b of the result holds in collective x."""
        k, j = self.owner[b]
        return placed_value(x, k, j)

    def ranges(self, shares):
        """The bytes of `shares` at the lead's places, in team-rank order, merged
        where they touch and none for a share of no bytes: put_step's puts."""
        out = []
        for k in sorted(shares, key=lambda k: self.rank[k]):
            a, e = self.base[k], self.base[k] + self.lengths[k]
            if a == e:
                continue
            if out and out[-1][1] == a:
                out[-1] = (out[-1][0], e)
            else:
                out.append((a, e))
        return out


def placed_lead(design, i, layout, collectives):
    """Lead i of a placed case: the design's program over bytes, its own share
    copied in at entry and each put a range of bytes in one step."""
    nodes, total = layout.nodes, layout.total
    S = steps_of(nodes)
    mine = [(layout.base[i] + j, j) for j in range(layout.lengths[i])]
    B = []
    for x in range(collectives):
        p = f'l{i}x{x}_'
        nxt = f'@l{i}x{x + 1}_E' if x + 1 < collectives else '@DONE'

        def misplaced(M, L, x=x):
            """PLACE at entry: the members' blocks, this node's share, at its place."""
            for b, j in mine:
                if b >= total or layout.expected(x, b) != placed_value(x, i, j):
                    k, at = layout.owner.get(b, (None, None))
                    return (f'PLACE: lead {i} copied byte {j} of its own share to byte {b} of the '
                            f'result, '
                            + (f'past its {total} bytes' if k is None else
                               f'which is share {k}\'s byte {at}')
                            + f': its place is byte {layout.true_base[i] + j}')
            return None

        def copy_own(M, L, x=x):
            upd = {}
            for b, j in mine:
                upd[BUF(i, b)] = placed_value(x, i, j)
                upd[WRITES(i, x, b)] = M[WRITES(i, x, b)] + 1
            return [(upd, {}, f'lead {i} copies its members\' blocks to bytes {mine[0][0]} on')]

        # Enter: this node's share, its members' blocks, into its own buffer -
        # nothing for a share of no bytes - then tell every lead that will write
        # here, one per step.
        B += [(p + 'E', ('ghost', lambda M, L, x=x: ({IN(i): x + 1}, {}), '@' + p + 'E0'))]
        if mine:
            B += [(p + 'E0', ('check', misplaced, '@' + p + 'E1')),
                  (p + 'E1', ('nd', copy_own, '@' + p + 'R0'))]
        else:
            B += [(p + 'E0', ('br', lambda L: True, '@' + p + 'R0', '@' + p + 'R0'))]
        for s in range(S):
            writer = (i + (1 << s)) % nodes
            nx = f'@{p}R{s + 1}' if s + 1 < S else f'@{p}S0'
            B += [(f'{p}R{s}', ('add', lambda L, w=writer, s=s: READY(w, s), lambda L: 1, '_', nx))]
        # The steps: each the shares' ranges, once the receiver has entered.
        for s in range(S):
            to = (i - (1 << s)) % nodes
            c = min(1 << s, nodes - (1 << s))
            puts = layout.ranges([(i + j) % nodes for j in range(c)])
            q = f'{p}S{s}'
            B += [(q, ('await', lambda M, L, s=s, x=x: M[READY(i, s)] >= x + 1,
                       f'@{q}W0' if puts else f'@{q}D'))]
            for j, (a, e) in enumerate(puts):
                nx = f'@{q}W{j + 1}' if j + 1 < len(puts) else f'@{q}D'

                def sendable(M, L, to=to, a=a, e=e, x=x):
                    """DEST and FORWARD, for a range."""
                    if M[IN(to)] != x + 1:
                        return (f'DEST: lead {i} wrote lead {to}\'s buffer in collective {x} while '
                                f'lead {to} was ' +
                                (f'in collective {M[IN(to)] - 1}' if M[IN(to)] else
                                 'outside it: not yet entered, or its members copying out'))
                    for b in range(a, e):
                        if M[BUF(i, b)] != layout.expected(x, b):
                            return (f'FORWARD: lead {i} forwarded byte {b} as {M[BUF(i, b)]}, not '
                                    f'collective {x}\'s {layout.expected(x, b)}')
                    return None

                def put(M, L, to=to, a=a, e=e, x=x):
                    upd = {}
                    for b in range(a, e):
                        upd[BUF(to, b)] = M[BUF(i, b)]
                        upd[WRITES(to, x, b)] = M[WRITES(to, x, b)] + 1
                    return [(upd, {}, f'lead {i} puts bytes {a} to {e - 1} into lead {to}\'s')]

                B += [(f'{q}W{j}', ('check', sendable, f'@{q}P{j}')),
                      (f'{q}P{j}', ('nd', put, nx))]
            nx = f'@{p}S{s + 1}' if s + 1 < S else f'@{p}RET'
            B += [(f'{q}D', ('add', lambda L, to=to, s=s: DATA(to, s), lambda L: 1, '_', f'@{q}A')),
                  (f'{q}A', ('await', lambda M, L, s=s, x=x: M[DATA(i, s)] >= x + 1, nx))]

        def placed(M, L, x=x):
            """PLACE at return: every byte written once in the collective, and
            holding its owner's value."""
            for b in range(total):
                if M[WRITES(i, x, b)] != 1:
                    return (f'PLACE: lead {i} returned from collective {x} with byte {b} of the '
                            f'result '
                            f'written {M[WRITES(i, x, b)]} times in it')
                if M[BUF(i, b)] != layout.expected(x, b):
                    return (f'PLACE: lead {i} returned from collective {x} with byte {b} holding '
                            f'{M[BUF(i, b)]}, not {layout.expected(x, b)}')
            return None

        # Return: the members copy the result out, then the lead may enter the next.
        B += [(p + 'RET', ('ghost', lambda M, L: ({IN(i): 0}, {}), '@' + p + 'PL')),
              (p + 'PL', ('check', placed, f'@{p}C0' if total else nxt))]
        for b in range(total):
            nx = f'@{p}C{b + 1}' if b + 1 < total else nxt
            B += [(f'{p}C{b}', ('check', lambda M, L, b=b, x=x:
                                None if M[BUF(i, b)] == layout.expected(x, b) else
                                f'RESULT: lead {i}\'s members copied byte {b} as {M[BUF(i, b)]}, '
                                f'not '
                                f'collective {x}\'s {layout.expected(x, b)}', nx))]
    B += [('DONE', ('gpost', lambda M, L: ({FIN: M[FIN] + 1}, {}), '@X')),
          ('X', ('done',))]
    return assemble(B)


def placed_case(design, lengths, stride, collectives):
    layout = Layout(design, lengths, stride)
    nodes = layout.nodes
    threads = [placed_lead(design, i, layout, collectives) for i in range(nodes)]
    mem = {FIN: 0}
    for n in range(nodes):
        mem[IN(n)] = 0
        for s in range(steps_of(nodes)):
            mem[READY(n, s)] = mem[DATA(n, s)] = 0
        for b in range(layout.total):
            mem[BUF(n, b)] = 0
            for x in range(collectives):
                mem[WRITES(n, x, b)] = 0
    name = (f'allgather/{design} lengths={",".join(map(str, lengths))} '
            f'stride={"+" if stride > 0 else "-"}1 collectives={collectives}')
    return name, threads, mem, nodes


# ---- ranged: the leads' scan, each step carrying its ranges (the docstring's section) ----


RECORD = lambda n, s: ('record', n, s)      # at lead n, past DATA[s]'s counter: step s's record
SHARE = lambda n: ('share', n)              # at lead n, past READY[S - 1]'s counter: the range
                                            # of its last-step receiver's own share
ARRIVALS = lambda n, j: ('arrivals', n, j)  # at lead n: the leads' scan's arrivals, child position j
RELEASES = lambda n: ('releases', n)        # at lead n: the leads' scan's downs
UP = lambda n, j: ('up', n, j)              # at lead n: child position j's small slot, its total
DOWN = lambda n: ('down', n)                # at lead n: its small release slot, (before, total)
TAG = lambda key: ('$tag',) + key           # ghost: collective + 1 whose value a slot holds
UNREAD = lambda key: ('$unread',) + key     # ghost: a slot holds a value not yet read
SCAN_OVERWRITE = ('$scan_overwrite',)


def scan_tree(nodes, fanout, shape='pre_order'):
    """The leads' scan's tree over positions 0 ... nodes - 1: each position's
    parent (None at the root, position 0) and its children in order. Pre-order,
    as the member scans' tree is over ranks (internode-generic.c, scan_place): a
    place covering positions [a, b) is position a, and its children split
    [a + 1, b) into at most `fanout` consecutive blocks, the first ones a
    position longer, so that every subtree is a contiguous range of positions.
    `heap` is the reductions' tree instead (collective-tree.h: position i's
    children k i + 1 ... k i + k), whose subtrees are not - the breakage
    heap_tree."""
    parent, kids = {0: None}, {p: [] for p in range(nodes)}
    if shape == 'heap':
        for p in range(1, nodes):
            parent[p] = (p - 1) // fanout
            kids[parent[p]].append(p)
        return parent, kids

    def walk(a, b):
        rest = b - a - 1
        m = min(fanout, rest)
        start = a + 1
        for j in range(m):
            size = rest // m + (1 if j < rest % m else 0)
            parent[start] = a
            kids[a].append(start)
            walk(start, start + size)
            start += size
    walk(0, nodes)
    return parent, kids


def leads_scan(totals, fanout, shape='pre_order'):
    """The leads' scan of each position's total, as its sweeps compute it: up, a
    subtree's total; down, the sum before the subtree and the total - child j's
    `before` its parent's, plus the parent's own total and the subtrees of
    children 0 ... j - 1. Returns each position's `before` - the totals at the
    positions before it, on the pre-order tree - and the total."""
    parent, kids = scan_tree(len(totals), fanout, shape)
    below = {}

    def up(p):
        below[p] = totals[p] + sum(up(c) for c in kids[p])
        return below[p]
    total = up(0)
    before = {0: 0}

    def down(p):
        at = before[p] + totals[p]
        for c in kids[p]:
            before[c] = at
            at += below[c]
            down(c)
    down(0)
    return before, total


def team_base(before, total, own, stride):
    """A node's base in team-rank order from the scan in position order: the
    totals before it under a positive stride, and under a negative one, which
    reverses the positions, the totals after it."""
    return before if stride > 0 else total - before - own


def step_ranges(lengths, base, total, stride, writer, count):
    """The bytes of positions writer ... writer + count - 1 (mod N) as two
    ranges, (start, end) each: `first` those up to position N - 1, `second`
    those past it, wrapped to position 0. An empty one keeps its place, where
    its bytes would begin: under a positive stride, whose bytes run up with the
    positions, `first` from the writer's base and `second` from 0; under a
    negative one, whose bytes run down, `first` down from the writer's end and
    `second` down from the total."""
    n = len(lengths)
    pre = sum(lengths[p] for p in range(writer, min(writer + count, n)))
    post = sum(lengths[p - n] for p in range(n, writer + count))
    if stride > 0:
        return (base[writer], base[writer] + pre), (0, post)
    end = base[writer] + lengths[writer]
    return (end - pre, end), (total - post, total)


def joined(held, more, stride):
    """A range held, and the range that continues it, as one - up from its end
    under a positive stride, down from its start under a negative one - or None
    where `more` does not continue `held`."""
    if more[0] > more[1]:
        return None
    if stride > 0:
        return (held[0], more[1]) if more[0] == held[1] else None
    return (more[0], held[1]) if more[1] == held[0] else None


def taken(held, record, lead, step, nodes, stride):
    """What a lead holds once it has taken step `step`'s record. Where
    lead + 2^step is below N, the writer's positions start before the lead's
    wrap: the record's `first` continues the lead's `first`, and its `second`
    the lead's `second`, empty until then. Otherwise all of the writer's
    positions are past the lead's wrap and short of its own: the record's
    `first` continues the lead's `second`, and its `second` holds nothing. None
    where a range does not continue what the lead holds."""
    (first, second), (more_first, more_second) = held, record
    if lead + (1 << step) < nodes:
        a, b = joined(first, more_first, stride), joined(second, more_second, stride)
        return None if a is None or b is None else (a, b)
    if more_second[0] != more_second[1]:
        return None
    b = joined(second, more_first, stride)
    return None if b is None else (first, b)


def cut(held, lead, count, nodes, stride, receiver_share, total):
    """The last step's shares, positions lead ... lead + count - 1, out of what
    the lead holds, which runs on into its receiver's own share - position
    lead + count (mod N): cut where that share begins, its start under a
    positive stride and its end under a negative one - in `first` where the
    positions stop short of N - 1's wrap, `second` then empty; else in
    `second`, `first` whole."""
    first, second = held
    at = receiver_share[0] if stride > 0 else receiver_share[1]
    if lead + count < nodes:
        return (((first[0], at) if stride > 0 else (at, first[1])),
                ((0, 0) if stride > 0 else (total, total)))
    return first, ((second[0], at) if stride > 0 else (at, second[1]))


def as_one(sent, stride):
    """The breakage wrap_as_one: a step's bytes as one range of their whole
    length from where they begin, a run that wraps past position N - 1 sent as
    though it did not."""
    first, second = sent
    length = first[1] - first[0] + second[1] - second[0]
    if stride > 0:
        return (first[0], first[0] + length), (0, 0)
    return (first[1] - length, first[1]), (second[1], second[1])


def length_patterns(nodes):
    """Each position's share several ways: all alike; every other one empty;
    one share holding every byte, at either end and in the middle; a spread
    with zeros among it; and nothing at all."""
    yield tuple([1] * nodes)
    yield tuple(2 * (p % 2) for p in range(nodes))
    for at in sorted({0, nodes // 2, nodes - 1}):
        yield tuple(3 if p == at else 0 for p in range(nodes))
    yield tuple((7 * p * p + 3 * p + 1) % 4 for p in range(nodes))
    yield tuple([0] * nodes)


def ranged_check(lengths, stride, broken=None):
    """P30's arithmetic on one layout, or why it fails: the leads' scan at every
    fan-out from 2 to 8 gives each node its base in team-rank order and the
    total; every step's ranges, the last one's cut at its receiver's own share,
    are the bytes of the shares it sends; every record continues what its
    receiver holds; and every lead ends holding the whole result."""
    n = len(lengths)
    base, total = bases(lengths, team_order(n, stride))
    for fanout in range(2, 9):
        before, scanned = leads_scan(lengths, fanout, 'heap' if broken == 'heap_tree' else
                                     'pre_order')
        if scanned != total:
            return f'SCAN: fan-out {fanout}: the scan\'s total is {scanned}, not {total}'
        for p in range(n):
            got = team_base(before[p], scanned, lengths[p], stride)
            if got != base[p]:
                return (f'SCAN: fan-out {fanout}: the scan puts position {p} at byte {got}, not '
                        f'{base[p]}')
    S = steps_of(n)
    held = {i: step_ranges(lengths, base, total, stride, i, 1) for i in range(n)}
    for s in range(S):
        c = min(1 << s, n - (1 << s))
        records = {}
        for i in range(n):
            to = (i - (1 << s)) % n
            sent = held[i]
            if s + 1 == S and broken != 'no_cut':
                sent = cut(sent, i, c, n, stride, (base[to], base[to] + lengths[to]), total)
            if broken == 'wrap_as_one':
                sent = as_one(sent, stride)
            want = step_ranges(lengths, base, total, stride, i, c)
            if sent != want:
                return (f'RANGE: step {s}: lead {i} sends {sent}, where positions {i} to '
                        f'{(i + c - 1) % n} are {want}')
            records[to] = sent
        for to, record in records.items():
            got = taken(held[to], record, to, s, n, stride)
            if got is None:
                return f'RANGE: step {s}: lead {to}\'s record {record} does not continue {held[to]}'
            held[to] = got
    for i in range(n):
        end = base[i] + lengths[i]
        whole = ((base[i], total), (0, base[i])) if stride > 0 else ((0, end), (end, total))
        if held[i] != whole:
            return f'PLACE: lead {i} ends holding {held[i]}, not the whole result, {whole}'
    return None


RANGED_ARITHMETIC_NODES = range(2, 71)
# Each breakage of the arithmetic, and the property it must fail.
RANGED_BREAKAGES = {'heap_tree': 'SCAN', 'no_cut': 'RANGE', 'wrap_as_one': 'RANGE'}


def ranged_arithmetic(broken=None):
    """(layouts, failures) over every node count of RANGED_ARITHMETIC_NODES,
    each of length_patterns, both strides."""
    layouts, failures = 0, []
    for n in RANGED_ARITHMETIC_NODES:
        for lengths in length_patterns(n):
            for stride in (1, -1):
                layouts += 1
                why = ranged_check(lengths, stride, broken)
                if why:
                    failures.append(f'N {n}, lengths {",".join(map(str, lengths))}, '
                                    f'stride {stride:+d}: {why}')
    return layouts, failures


def record_word(epoch, first, second):
    """A step's record as one word of the model's memory: its epoch - the
    collective + 1 - and its two ranges, every byte below 100."""
    return (((epoch * 100 + first[0]) * 100 + first[1]) * 100 + second[0]) * 100 + second[1]


def record_of(word):
    """(epoch, first, second) of a record word."""
    word, second_end = divmod(word, 100)
    word, second_start = divmod(word, 100)
    word, first_end = divmod(word, 100)
    epoch, first_start = divmod(word, 100)
    return epoch, (first_start, first_end), (second_start, second_end)


def share_word(epoch, start, end):
    """A lead's own share's range, put into its last-step writer's READY line."""
    return (epoch * 100 + start) * 100 + end


def share_of(word):
    word, end = divmod(word, 100)
    epoch, start = divmod(word, 100)
    return epoch, (start, end)


class Ranged:
    """A ranged case's collectives: each one's lengths by position - differing
    from one collective to the next, so that a record or a byte left from the
    collective before is never this one's - their bases in team-rank order and
    the total, which every check holds the leads to."""

    def __init__(self, lengths, stride):
        self.lengths = lengths
        self.nodes = len(lengths[0])
        self.stride = stride
        self.order = team_order(self.nodes, stride)
        self.base, self.total, self.owner = [], [], []
        for x, by_position in enumerate(lengths):
            b, t = bases(by_position, self.order)
            self.base.append(b)
            self.total.append(t)
            self.owner.append({b[k] + j: (k, j) for k in range(self.nodes)
                               for j in range(by_position[k])})
        self.room = max(self.total)

    def end(self, x, k):
        return self.base[x][k] + self.lengths[x][k]

    def expected(self, x, b):
        k, j = self.owner[x][b]
        return placed_value(x, k, j)

    def ranges(self, x, writer, count):
        return step_ranges(self.lengths[x], self.base[x], self.total[x], self.stride, writer,
                           count)


def chain(steps, first, last):
    """Instructions as (label, instruction) with no successors yet, each made to
    go on to the next, the first labelled `first` and the last going to
    `last`."""
    out = []
    for k, (label, ins) in enumerate(steps):
        ins = list(ins)
        target = '@' + steps[k + 1][0] if k + 1 < len(steps) else last
        if ins[0] == 'br':
            ins[2] = ins[3] = target
        else:
            ins[-1] = target
        out.append((first if k == 0 else label, tuple(ins)))
    return out


def scan_steps(p, i, x, ranged, fanout):
    """Lead i's part in collective x's scan of the nodes' totals, through the
    team number's small slots and words (the docstring's section): up, each
    child's subtree total once its arrival is counted, then its own subtree's
    to its parent; down, (before, total) once its own down is counted - the
    root's its own - then each child's. SLOT and VALUE as scan_model.py has
    them, and SCAN: the base it takes in team-rank order, and the total, are
    this collective's."""
    n, stride = ranged.nodes, ranged.stride
    own = ranged.lengths[x][i]
    parent, kids = scan_tree(n, fanout)
    children = kids[i]
    steps = []
    for j in range(len(children)):
        key = UP(i, j)
        steps += [(f'{p}U{j}', ('await', lambda M, L, j=j, x=x: M[ARRIVALS(i, j)] >= x + 1, None)),
                  (f'{p}V{j}', ('check', lambda M, L, key=key, x=x, j=j:
                                None if M[TAG(key)] == x + 1 else
                                f'VALUE: lead {i} took child position {j}\'s total, collective '
                                f'{M[TAG(key)] - 1}\'s, in collective {x}', None)),
                  (f'{p}T{j}', ('nd', lambda M, L, key=key, j=j:
                                [({UNREAD(key): 0}, {f'child{j}': M[key]},
                                  f'lead {i} takes child position {j}\'s total')], None))]
    below = lambda L, upto: sum(L[f'child{q}'] for q in range(upto))
    if parent[i] is not None:
        position = kids[parent[i]].index(i)
        key = UP(parent[i], position)

        def put_up(M, L, key=key, x=x):
            if M[UNREAD(key)]:
                return [({SCAN_OVERWRITE: 1}, {}, 'overwrite')]
            return [({key: own + below(L, len(children)), UNREAD(key): 1, TAG(key): x + 1}, {},
                     f'lead {i} puts its subtree\'s total up')]
        steps += [(f'{p}UP', ('nd', put_up, None)),
                  (f'{p}UO', ('check', lambda M, L: None if not M[SCAN_OVERWRITE] else
                              f'SLOT: lead {i} overwrote a slot its parent had not read', None)),
                  (f'{p}UA', ('add', lambda L, position=position: ARRIVALS(parent[i], position),
                              lambda L: 1, '_', None)),
                  (f'{p}DW', ('await', lambda M, L, x=x: M[RELEASES(i)] >= x + 1, None)),
                  (f'{p}DV', ('check', lambda M, L, x=x:
                              None if M[TAG(DOWN(i))] == x + 1 else
                              f'VALUE: lead {i} took its down, collective '
                              f'{M[TAG(DOWN(i))] - 1}\'s, in '
                              f'collective {x}', None)),
                  (f'{p}DT', ('nd', lambda M, L: [({UNREAD(DOWN(i)): 0},
                                                   {'before': M[DOWN(i)] // 100,
                                                    'total': M[DOWN(i)] % 100},
                                                   f'lead {i} takes its down')], None))]
    else:
        steps += [(f'{p}DW', ('set', 'before', lambda L: 0, None)),
                  (f'{p}DT', ('set', 'total', lambda L: own + below(L, len(children)), None))]
    for j, child in enumerate(children):
        key = DOWN(child)

        def put_down(M, L, key=key, x=x, j=j, child=child):
            if M[UNREAD(key)]:
                return [({SCAN_OVERWRITE: 1}, {}, 'overwrite')]
            value = (L['before'] + own + below(L, j)) * 100 + L['total']
            return [({key: value, UNREAD(key): 1, TAG(key): x + 1}, {},
                     f'lead {i} puts lead {child}\'s down')]
        steps += [(f'{p}Q{j}', ('nd', put_down, None)),
                  (f'{p}QO{j}', ('check', lambda M, L: None if not M[SCAN_OVERWRITE] else
                                 f'SLOT: lead {i} overwrote a down its child had not read', None)),
                  (f'{p}QA{j}', ('add', lambda L, child=child: RELEASES(child), lambda L: 1, '_',
                                 None))]
    base, total = ranged.base[x][i], ranged.total[x]
    steps += [(f'{p}SC', ('check', lambda M, L, x=x, base=base, total=total:
                          None if (team_base(L['before'], L['total'], own, stride) == base and
                                   L['total'] == total) else
                          f'SCAN: lead {i}\'s scan gave base '
                          f'{team_base(L["before"], L["total"], own, stride)} and total '
                          f'{L["total"]} in collective {x}, not {base} and {total}', None))]
    steps += [(f'{p}SD{name}', ('set', name, lambda L: DEL, None))
              for name in [f'child{j}' for j in range(len(children))] + ['before', 'total']]
    return steps


def ranged_lead(design, i, ranged, collectives, fanout):
    """Lead i of a ranged case: at entry its base, its own share's end and the
    total - from the layout, the scan abstracted, or from the leads' scan
    (`ranged_scan`) - then the gather, each step's ranges in its record. What
    it holds is one local, `held`: its two ranges, as `taken` keeps them."""
    n, stride = ranged.nodes, ranged.stride
    S = steps_of(n)
    last_writer = (i + (1 << (S - 1))) % n
    B = []
    for x in range(collectives):
        p = f'l{i}x{x}_'
        nxt = f'@l{i}x{x + 1}_E' if x + 1 < collectives else '@DONE'
        own, total = ranged.lengths[x][i], ranged.total[x]
        base, end = ranged.base[x][i], ranged.end(x, i)
        steps = []
        if design == 'ranged_scan':
            steps += scan_steps(p, i, x, ranged, fanout)
        # This node's share, its members' blocks, into its own buffer; what it
        # holds, its own share's range.
        steps += [(f'{p}H', ('set', 'held', lambda L, h=ranged.ranges(x, i, 1): h, None))]
        if own:
            def copy_own(M, L, x=x, base=base, own=own):
                upd = {}
                for j in range(own):
                    upd[BUF(i, base + j)] = placed_value(x, i, j)
                    upd[WRITES(i, x, base + j)] = M[WRITES(i, x, base + j)] + 1
                return [(upd, {}, f'lead {i} copies its members\' blocks to bytes {base} on')]
            steps += [(f'{p}G', ('nd', copy_own, None))]
        # Entry: its own share's range past the READY[S - 1] counter of the lead
        # that writes here at the last step, then the READY adds, that one's
        # right after it.
        steps += [(f'{p}O', ('store', lambda L: SHARE(last_writer),
                             lambda L, word=share_word(x + 1, base, end): word, True, None))]
        steps += [(f'{p}R{s}', ('add', lambda L, w=(i + (1 << s)) % n, s=s: READY(w, s),
                                lambda L: 1, '_', None)) for s in range(S - 1, -1, -1)]
        for s in range(S):
            to = (i - (1 << s)) % n
            c = min(1 << s, n - (1 << s))
            last = s + 1 == S
            q = f'{p}S{s}'
            want = ranged.ranges(x, i, c)

            def sending(L, last=last, c=c, total=total):
                """The step's ranges: all it holds, the last step's cut at its
                receiver's own share."""
                out = L['held']
                if last and design != 'no_cut':
                    out = cut(out, i, c, n, stride, L['share'], total)
                if design == 'wrap_as_one':
                    out = as_one(out, stride)
                return out
            to_send = [(f'{q}X', ('set', 'send', lambda L, sending=sending: sending(L), None))]
            put_record = [(f'{q}K', ('store', lambda L, to=to, s=s: RECORD(to, s),
                                     lambda L, x=x: record_word(x + 1, *L['send']), True,
                                     None))]
            early = design == 'range_before_ready' and not last
            if early:
                # The breakage: the record is the library's own word, not the
                # receiver's buffer, so it goes ahead of the READY wait.
                steps += to_send + put_record
            steps += [(f'{q}W', ('await', lambda M, L, s=s, x=x: M[READY(i, s)] >= x + 1, None))]
            if last:
                # Its receiver's own share, which that lead put here at entry.
                share = (ranged.base[x][to], ranged.end(x, to))
                steps += [(f'{q}J', ('load', lambda L: SHARE(i), 'word', None)),
                          (f'{q}JC', ('check', lambda M, L, x=x, share=share, to=to:
                                      None if share_of(L['word']) == (x + 1, share) else
                                      f'RANGE: lead {i} took lead {to}\'s share as '
                                      f'{share_of(L["word"])}, not collective {x}\'s '
                                      f'{(x + 1, share)}', None)),
                          (f'{q}Js', ('set', 'share', lambda L: share_of(L['word'])[1], None)),
                          (f'{q}Jw', ('set', 'word', lambda L: DEL, None))]
            if not early:
                steps += to_send

            def sendable(M, L, to=to, x=x, s=s, want=want):
                """RANGE, then DEST and FORWARD, for the step's ranges."""
                if L['send'] != want:
                    return (f'RANGE: lead {i} sends {L["send"]} at step {s} of collective {x}, '
                            f'where '
                            f'the shares it sends are {want}')
                if M[IN(to)] != x + 1:
                    return (f'DEST: lead {i} wrote lead {to}\'s buffer in collective {x} while '
                            f'lead {to} was ' +
                            (f'in collective {M[IN(to)] - 1}' if M[IN(to)] else
                             'outside it: not yet entered, or its members copying out'))
                for a, e in want:
                    for b in range(a, e):
                        if M[BUF(i, b)] != ranged.expected(x, b):
                            return (f'FORWARD: lead {i} forwarded byte {b} as {M[BUF(i, b)]}, not '
                                    f'collective {x}\'s {ranged.expected(x, b)}')
                return None
            steps += [(f'{q}C', ('check', sendable, None))]
            # A put a range, none for an empty one, past the check the step's own;
            # one for both where they touch.
            puts = sorted(r for r in want if r[0] < r[1])
            if len(puts) == 2 and puts[0][1] == puts[1][0]:
                puts = [(puts[0][0], puts[1][1])]
            for which, (a, e) in enumerate(puts):
                def put(M, L, a=a, e=e, to=to, x=x):
                    upd = {}
                    for b in range(a, e):
                        upd[BUF(to, b)] = M[BUF(i, b)]
                        upd[WRITES(to, x, b)] = M[WRITES(to, x, b)] + 1
                    return [(upd, {}, f'lead {i} puts bytes {a} to {e - 1} into lead {to}\'s')]
                steps += [(f'{q}P{which}', ('nd', put, None))]
            if not early:
                steps += put_record
            steps += [(f'{q}D', ('add', lambda L, to=to, s=s: DATA(to, s), lambda L: 1, '_',
                                 None)),
                      (f'{q}Xd', ('set', 'send', lambda L: DEL, None)),
                      (f'{q}Jd', ('set', 'share', lambda L: DEL, None))]
            # This step's own record, from the lead 2^s on: once DATA says it
            # landed, read, held to RANGE, and taken.
            writer = (i + (1 << s)) % n
            record = (x + 1,) + ranged.ranges(x, writer, c)
            take_record = [(f'{q}L', ('load', lambda L, s=s: RECORD(i, s), 'word', None))]
            if design == 'range_before_data':
                steps += take_record
            steps += [(f'{q}A', ('await', lambda M, L, s=s, x=x: M[DATA(i, s)] >= x + 1, None))]
            if design != 'range_before_data':
                steps += take_record

            def take(L, s=s):
                _, first, second = record_of(L['word'])
                return taken(L['held'], (first, second), i, s, n, stride)
            steps += [(f'{q}RC', ('check', lambda M, L, x=x, s=s, record=record, writer=writer,
                                  take=take:
                                  f'RANGE: lead {i} took step {s}\'s record from lead {writer} as '
                                  f'{record_of(L["word"])}, not collective {x}\'s {record}'
                                  if record_of(L['word']) != record else
                                  None if take(L) is not None else
                                  f'RANGE: lead {i}\'s record {record_of(L["word"])} from lead '
                                  f'{writer} does not continue what it holds, {L["held"]}',
                                  None)),
                      (f'{q}M', ('set', 'held', lambda L, take=take: take(L), None)),
                      (f'{q}Md', ('set', 'word', lambda L: DEL, None))]
        whole = (((base, total), (0, base)) if stride > 0 else ((0, end), (end, total)))

        def placed(M, L, x=x, total=total, whole=whole):
            """PLACE at return: every byte of the result written once in the
            collective, holding its owner's value, and none past it; and the
            lead holding the whole result."""
            for b in range(ranged.room):
                writes = M[WRITES(i, x, b)]
                if b >= total and writes:
                    return (f'PLACE: lead {i}\'s byte {b}, past collective {x}\'s {total}, was '
                            f'written')
                if b < total and writes != 1:
                    return (f'PLACE: lead {i} returned from collective {x} with byte {b} of the '
                            f'result '
                            f'written {writes} times in it')
                if b < total and M[BUF(i, b)] != ranged.expected(x, b):
                    return (f'PLACE: lead {i} returned from collective {x} with byte {b} holding '
                            f'{M[BUF(i, b)]}, not {ranged.expected(x, b)}')
            if L['held'] != whole:
                return (f'PLACE: lead {i} returned from collective {x} holding {L["held"]}, not '
                        f'{whole}')
            return None
        # Return: the members copy the result out, then the lead may enter the next.
        steps += [(f'{p}RET', ('ghost', lambda M, L: ({IN(i): 0}, {}), None)),
                  (f'{p}PL', ('check', placed, None)),
                  (f'{p}Hd', ('set', 'held', lambda L: DEL, None))]
        steps += [(f'{p}C{b}', ('check', lambda M, L, b=b, x=x:
                                None if M[BUF(i, b)] == ranged.expected(x, b) else
                                f'RESULT: lead {i}\'s members copied byte {b} as {M[BUF(i, b)]}, '
                                f'not '
                                f'collective {x}\'s {ranged.expected(x, b)}', None))
                  for b in range(total)]
        B += [(p + 'E', ('ghost', lambda M, L, x=x: ({IN(i): x + 1}, {}), f'@{p}E1'))]
        B += chain(steps, f'{p}E1', nxt)
    B += [('DONE', ('gpost', lambda M, L: ({FIN: M[FIN] + 1}, {}), '@X')),
          ('X', ('done',))]
    return assemble(B)


def ranged_case(design, lengths, stride, fanout):
    ranged = Ranged(lengths, stride)
    nodes, collectives = ranged.nodes, len(lengths)
    threads = [ranged_lead(design, i, ranged, collectives, fanout) for i in range(nodes)]
    mem = {FIN: 0, SCAN_OVERWRITE: 0}
    for n in range(nodes):
        mem[IN(n)] = mem[SHARE(n)] = mem[RELEASES(n)] = mem[DOWN(n)] = 0
        mem[TAG(DOWN(n))] = mem[UNREAD(DOWN(n))] = 0
        for j in range(fanout):
            mem[ARRIVALS(n, j)] = mem[UP(n, j)] = mem[TAG(UP(n, j))] = mem[UNREAD(UP(n, j))] = 0
        for s in range(steps_of(nodes)):
            mem[READY(n, s)] = mem[DATA(n, s)] = mem[RECORD(n, s)] = 0
        for b in range(ranged.room):
            mem[BUF(n, b)] = 0
            for x in range(collectives):
                mem[WRITES(n, x, b)] = 0
    shown = '/'.join(','.join(map(str, by_position)) for by_position in lengths)
    name = (f'allgather/{design} lengths={shown} stride={"+" if stride > 0 else "-"}1'
            + (f' fanout={fanout}' if design == 'ranged_scan' else ''))
    return name, threads, mem, nodes


# (design, nodes, collectives, expect): True, or the property it must fail
CASES = [
    ('design', 2, 2, True),
    ('design', 3, 2, True),
    ('design', 4, 2, True),
    ('design', 5, 2, True),
    ('design', 3, 3, True),
    ('no_ready', 3, 2, 'DEST'),
    ('no_data_wait', 4, 1, 'FORWARD'),
    ('short_steps', 3, 1, 'RESULT'),
    ('staged', 2, 3, True),
    ('staged', 3, 3, True),
    ('staged', 4, 3, True),
    ('staged', 5, 2, True),
    ('staged_one_slot', 3, 2, True),    # N - 1 a power of two: i - 1 feeds i, so one slot holds
    ('staged_one_slot', 4, 2, 'SLOT'),
    ('staged_early_copy', 3, 1, 'FORWARD'),
]

# (design, each position's share in bytes, the stride's sign, collectives, expect: True, or
# the property it must fail).
# A share of no bytes on two nodes and in every case of more. On four nodes lead
# 3's step 1 sends positions 3 and 0, a run that wraps past position N - 1: in two
# puts under either stride, or in one where the shares between them in team-rank
# order have no bytes, so that the two ranges touch.
PLACED_CASES = [
    ('placed', (2, 1), 1, 2, True),
    ('placed', (2, 1), -1, 2, True),
    ('placed', (0, 2), -1, 2, True),
    ('placed', (2, 0, 1), 1, 2, True),
    ('placed', (2, 0, 1), -1, 2, True),
    ('placed', (1, 0, 2, 1), 1, 2, True),
    ('placed', (1, 0, 2, 1), -1, 2, True),
    ('placed', (1, 0, 0, 2), 1, 2, True),
    ('placed', (2, 0, 0, 1), -1, 2, True),
    ('bases_in_node_order', (2, 0, 1), 1, 1, True),     # one order: the sum is the same
    ('bases_in_node_order', (2, 0, 1), -1, 1, 'PLACE'),
]

# (design, each collective's shares by position, the stride's sign, the leads'
# scan's fan-out, expect: True, or the property it must fail). The lengths differ from one
# collective to the next,
# a zero among them. On four nodes lead 3's step 1 sends positions 3 and 0 under
# either stride, a run wrapping past position N - 1 in two ranges - touching, so
# one put's bytes, where the shares between them in team-rank order are empty; on
# three and five every lead's last step is a cut of what it holds down to its own
# share, of two shares and of four. The scan's fan-out 2 gives four nodes a tree
# two deep, position 1 interior.
RANGED_CASES = [
    ('ranged', ((2, 1), (0, 2)), 1, 2, True),
    ('ranged', ((2, 1), (0, 2)), -1, 2, True),
    ('ranged', ((2, 0, 1), (1, 2, 0)), 1, 2, True),
    ('ranged', ((2, 0, 1), (1, 2, 0)), -1, 2, True),
    ('ranged', ((1, 0, 2, 1), (2, 1, 0, 1)), 1, 2, True),
    ('ranged', ((1, 0, 2, 1), (2, 1, 0, 1)), -1, 2, True),
    ('ranged', ((1, 0, 0, 2), (0, 1, 2, 0)), 1, 2, True),
    ('ranged', ((1, 0, 1, 0, 1), (0, 1, 0, 1, 1)), -1, 2, True),
    ('ranged_scan', ((2, 0, 1), (1, 2, 0)), 1, 2, True),
    ('ranged_scan', ((2, 0, 1), (1, 2, 0)), -1, 2, True),
    ('ranged_scan', ((1, 0, 2, 1), (2, 1, 0, 1)), 1, 2, True),
    ('ranged_scan', ((1, 0, 2, 1), (2, 1, 0, 1)), -1, 2, True),
    ('range_before_data', ((2, 0, 1),), 1, 2, 'RANGE'),
    ('range_before_ready', ((1, 0, 2, 1), (2, 1, 0, 1)), 1, 2, 'RANGE'),
    ('wrap_as_one', ((1, 0, 2, 1),), 1, 2, 'RANGE'),
    ('no_cut', ((2, 0, 1),), 1, 2, 'RANGE'),
]


def arithmetic(want):
    """The ranged arithmetic's lines (ranged_arithmetic): the design without a
    failure, and each of its breakages failing somewhere. True if all as
    expected."""
    name = 'allgather/ranged arithmetic'
    if not want(name):
        return True
    layouts, failures = ranged_arithmetic()
    print(f'{name}: N {RANGED_ARITHMETIC_NODES.start} to {RANGED_ARITHMETIC_NODES.stop - 1}, '
          f'{layouts} layouts, the leads\' scan at fan-outs 2 to 8 and every step\'s ranges: '
          f'{len(failures)} failures', flush=True)
    for why in failures[:5]:
        print(f'    {why}')
    ok = not failures
    for broken, expect in RANGED_BREAKAGES.items():
        _, failures = ranged_arithmetic(broken)
        # Its first failure names the property it fails, as a check's message
        # does (engine.py's Properties).
        first = failures[0].split(': ', 1)[1] if failures else None
        wrong = judge(not failures, named(first), expect)
        print(f'{name}, broken {broken}: ' +
              ('PASSES - the check is blind to it' if not failures else
               f'fails {named(first)}' + (', as it must' if wrong is None else '') +
               f' - first at {failures[0]}') +
              ('' if wrong is None else '   <-- UNEXPECTED' + wrong), flush=True)
        ok &= wrong is None
    return ok


def main():
    want = select(sys.argv)
    ok = True
    runs = [case(design, nodes, collectives) + (expect,)
            for design, nodes, collectives, expect in CASES]
    runs += [placed_case(design, lengths, stride, collectives) + (expect,)
             for design, lengths, stride, collectives, expect in PLACED_CASES]
    ranged = [ranged_case(design, lengths, stride, fanout) + (expect,)
              for design, lengths, stride, fanout, expect in RANGED_CASES]
    for name, threads, mem, n, expect in runs:
        if not want(name):
            continue
        res = explore(name, threads, mem, max_seconds=900, liveness_max_states=3_000_000,
                      done=lambda M, n=n: M[FIN] == n)
        ok &= report(res, expect=expect)
    ok &= arithmetic(want)
    for name, threads, mem, n, expect in ranged:
        if not want(name):
            continue
        res = explore(name, threads, mem, max_seconds=900, liveness_max_states=3_000_000,
                      done=lambda M, n=n: M[FIN] == n)
        ok &= report(res, expect=expect)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
