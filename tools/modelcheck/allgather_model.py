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

Checked
  DEST     a lead writes a peer's buffer only while the peer is in the same
           collective: never during the peer's members' copy-out of the one
           before.
  FORWARD  a share a lead sends carries this collective's value.
  RESULT   on return every share of the buffer carries this collective's value,
           and it still does when the members finish copying it out.
  PLACE    (the placed cases, below) every byte of the result, [0, total), is
           written once in a collective - by its own lead's copy of its
           members' blocks, or by one put - at its place, with its owner's
           value, and holds it at return.
  and, by the engine: no stuck lead, and every state can still finish.

Designs
  design               as above.
  placed               shares of differing lengths, a zero among them, placed
                       from each node's total (below).
Breakages
  no_ready             a lead writes without waiting for READY (DEST, RESULT).
  no_data_wait         a lead forwards without waiting for DATA (FORWARD).
  short_steps          floor(log2 N) steps rather than the ceiling: on a node
                       count that is not a power of two some shares never
                       arrive (RESULT).
  bases_in_node_order  the placed design with the bases summed in position
                       order, not team-rank order: under a negative stride a
                       node's share lands at another's place (PLACE); under a
                       positive one the two orders are one, and it holds.

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

Usage: allgather_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import assemble, explore, report, select

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
                                 f'lead {i} overwrote lead {to}\'s slot {slot} before lead {to} '
                                 f'copied the share staged there', f'@{q}D')),
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
                                 f'lead {i} copied share {mine} out of its slot {slot} as '
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
                                    f'lead {i} wrote lead {to}\'s buffer in collective {x} while '
                                    f'lead {to} was ' + (f'in collective {M[IN(to)] - 1}' if M[IN(to)]
                                                         else 'outside it: not yet entered, or '
                                                              'its members copying out'),
                                    f'@{q}V{j}')),
                      (f'{q}V{j}', ('check', lambda M, L, k=k, x=x:
                                    None if M[BUF(i, k)] == value(x, k) else
                                    f'lead {i} forwarded share {k} as {M[BUF(i, k)]}, '
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
                                f'lead {i}\'s members copied share {k} as {M[BUF(i, k)]}, not '
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
                    return (f'lead {i} copied byte {j} of its own share to byte {b} of the result, '
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
                        return (f'lead {i} wrote lead {to}\'s buffer in collective {x} while '
                                f'lead {to} was ' +
                                (f'in collective {M[IN(to)] - 1}' if M[IN(to)] else
                                 'outside it: not yet entered, or its members copying out'))
                    for b in range(a, e):
                        if M[BUF(i, b)] != layout.expected(x, b):
                            return (f'lead {i} forwarded byte {b} as {M[BUF(i, b)]}, not '
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
                    return (f'lead {i} returned from collective {x} with byte {b} of the result '
                            f'written {M[WRITES(i, x, b)]} times in it')
                if M[BUF(i, b)] != layout.expected(x, b):
                    return (f'lead {i} returned from collective {x} with byte {b} holding '
                            f'{M[BUF(i, b)]}, not {layout.expected(x, b)}')
            return None

        # Return: the members copy the result out, then the lead may enter the next.
        B += [(p + 'RET', ('ghost', lambda M, L: ({IN(i): 0}, {}), '@' + p + 'PL')),
              (p + 'PL', ('check', placed, f'@{p}C0' if total else nxt))]
        for b in range(total):
            nx = f'@{p}C{b + 1}' if b + 1 < total else nxt
            B += [(f'{p}C{b}', ('check', lambda M, L, b=b, x=x:
                                None if M[BUF(i, b)] == layout.expected(x, b) else
                                f'lead {i}\'s members copied byte {b} as {M[BUF(i, b)]}, not '
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


# (design, nodes, collectives, expect_ok)
CASES = [
    ('design', 2, 2, True),
    ('design', 3, 2, True),
    ('design', 4, 2, True),
    ('design', 5, 2, True),
    ('design', 3, 3, True),
    ('no_ready', 3, 2, False),
    ('no_data_wait', 4, 1, False),
    ('short_steps', 3, 1, False),
    ('staged', 2, 3, True),
    ('staged', 3, 3, True),
    ('staged', 4, 3, True),
    ('staged', 5, 2, True),
    ('staged_one_slot', 3, 2, True),    # N - 1 a power of two: i - 1 feeds i, so one slot holds
    ('staged_one_slot', 4, 2, False),
    ('staged_early_copy', 3, 1, False),
]

# (design, each position's share in bytes, the stride's sign, collectives, expect_ok).
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
    ('bases_in_node_order', (2, 0, 1), -1, 1, False),
]


def main():
    want = select(sys.argv)
    ok = True
    runs = [case(design, nodes, collectives) + (expect,)
            for design, nodes, collectives, expect in CASES]
    runs += [placed_case(design, lengths, stride, collectives) + (expect,)
             for design, lengths, stride, collectives, expect in PLACED_CASES]
    for name, threads, mem, n, expect in runs:
        if not want(name):
            continue
        res = explore(name, threads, mem, max_seconds=900, liveness_max_states=3_000_000,
                      done=lambda M, n=n: M[FIN] == n)
        ok &= report(res, expect_ok=expect)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
