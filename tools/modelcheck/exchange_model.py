#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""collect's node totals by a direct exchange among the leads (P33 of
docs/prompts/v0.2-open-items.md; the author's 1(b) of 2026-10-13, from P30's
stage 1, docs/design/collect-scan-proposal.md, "Does it pay?").

Where every node of a team holds one run of its ranks, P29 gives every lead every
node's total through a reduction's tree - up and down, two hops at eight nodes.
The exchange gives the same in one: each lead puts its own node's total into a
slot of every other lead, the slot of its own position, and adds one to that
lead's counter, fenced after the put; a lead whose counter shows the other N - 1
reads its slots, and has every node's total. N - 1 messages out of every lead
and as many in, where the tree's root alone takes N - 1 - so for small node
counts, the tree beyond.

The slots are reused from one collect to the next, and nothing waits before
writing them: a lead writes collect x's total as it enters x, while a slower
lead may not yet have read x - 1's. Two rules make that safe, each a breakage:
  PARITY   two sets of slots, and a counter each, chosen by the exchange's
           count's parity, as the staged allgather's step 0 is
           (allgather_model.py, staged). A lead writing x's total has finished
           x - 1, whose exchange it finished only with every lead's x - 1
           total, which each lead put as it entered x - 1, after finishing
           x - 2 - after reading x - 2's slots, the ones x's reuse.
  COUNTER  a counter a parity, not one: within a parity a lead's adds for
           x + 2 come only after it finished x + 1, so after every lead entered
           x + 1 and so finished its wait for x; but an add for x + 1 into a
           counter x also counts could arrive before a slower lead's add for x,
           and the wait for x would read a slot not yet written.
Counters are cumulative over the team's life and compared as at least: a
parity's counter gains N - 1 an exchange of that parity, so a lead's k-th
exchange of a parity waits for k (N - 1).

What follows the exchange is the gather, and no lead finishes a gather before
every lead has entered it - each lead's every share reaches every other through
puts it makes (allgather_model.py): that alone would order x + 1's writes after
every lead's reads of x, and one slot set and one counter would hold. But a
collect whose every length is 0 has no gather, and its exchange stands alone; so
the model runs some collects with the gather - abstracted as that rule, a lead
returning only once every lead has entered - and some without, and the design
must hold either way.

Checked
  SLOT   no slot is written while it holds a total its reader has not taken.
  VALUE  a total taken from a slot is that sender's, in this collect.
  and, by the engine: no stuck lead, and every state can still finish.

Designs
  design       as above.
Breakages
  one_slot     one set of slots: a lead that finished a collect of nothing
               writes the next one's total over a slot its reader has yet to
               take (SLOT). With a gather after every exchange it holds.
  one_counter  one counter for both sets of slots: a fast lead's add for the
               next collect stands in for a slow one's for this, and the slot
               read is the collect before last's (VALUE). With a gather after
               every exchange it holds.

Every add is fenced after the put it covers, and waits are abstract (the region
wait's wake is wait_word_model.py's); runs are SC.

Usage: exchange_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import assemble, explore, report, select

SLOT = lambda n, p, j: ('slot', n, p, j)       # at lead n: parity p's slot for position j's total
COUNT = lambda n, p: ('count', n, p)           # at lead n: parity p's counter
TAG = lambda n, p, j: ('$tag', n, p, j)        # ghost: collect + 1 whose total the slot holds
UNREAD = lambda n, p, j: ('$unread', n, p, j)  # ghost: the slot holds a total not yet taken
ENTERED = lambda x: ('$entered', x)            # ghost: leads that entered collect x's gather
FIN = ('$fin',)
OVERWRITE = ('$overwrite',)


def total(x, j):
    """Position j's total in collect x: distinct across collects and positions,
    and never 0, which a slot holds before any write."""
    return 100 * (x + 1) + j


def lead(design, i, nodes, gathers):
    """Lead i: each collect x the exchange, then - where gathers[x] - the
    gather, abstracted as returning only once every lead has entered it."""
    B = []
    for x, gather in enumerate(gathers):
        p = f'l{i}x{x}_'
        nxt = f'@l{i}x{x + 1}_P0' if x + 1 < len(gathers) else '@DONE'
        # The slots' set and the counter: each by the exchange's parity, or one
        # of either under its breakage.
        parity = 0 if design == 'one_slot' else x % 2
        counter_of = lambda y: 0 if design == 'one_counter' else y % 2
        counter = counter_of(x)
        # This lead's exchanges so far whose counter is this one's: the wait's
        # target is one more of them, N - 1 adds each.
        before = sum(1 for y in range(x) if counter_of(y) == counter)
        others = [j for j in range(nodes) if j != i]
        # Enter: this lead's total into its slot at every other lead, each put
        # then the fenced add of that lead's counter.
        for k, j in enumerate(others):
            here = f'{p}P{k}'

            def put(M, L, j=j, x=x, parity=parity):
                if M[UNREAD(j, parity, i)]:
                    return [({OVERWRITE: 1}, {}, 'overwrite')]
                return [({SLOT(j, parity, i): total(x, i), TAG(j, parity, i): x + 1,
                          UNREAD(j, parity, i): 1}, {},
                         f'lead {i} puts its total into lead {j}\'s slot {parity}.{i}')]
            B += [(here, ('nd', put, f'@{here}O')),
                  (f'{here}O', ('check', lambda M, L, j=j, parity=parity:
                                None if not M[OVERWRITE] else
                                f'SLOT: lead {i} overwrote lead {j}\'s slot {parity}.{i} before '
                                f'lead {j} took the total there', f'@{here}A')),
                  (f'{here}A', ('add', lambda L, j=j, counter=counter: COUNT(j, counter),
                                lambda L: 1, '_',
                                f'@{p}P{k + 1}' if k + 1 < len(others) else f'@{p}W'))]
        # Wait for the other N - 1, then take each one's total.
        B += [(f'{p}W', ('await', lambda M, L, counter=counter, want=(before + 1) * (nodes - 1):
                         M[COUNT(i, counter)] >= want, f'@{p}T0'))]
        for k, j in enumerate(others):
            here = f'{p}T{k}'
            after = f'@{p}T{k + 1}' if k + 1 < len(others) else (f'@{p}G' if gather else nxt)
            B += [(here, ('check', lambda M, L, j=j, x=x, parity=parity:
                          None if M[TAG(i, parity, j)] == x + 1 and
                          M[SLOT(i, parity, j)] == total(x, j) else
                          f'VALUE: lead {i} took {M[SLOT(i, parity, j)]} from its slot '
                          f'{parity}.{j}, collect {M[TAG(i, parity, j)] - 1}\'s, in collect {x}, '
                          f'where lead {j}\'s total is {total(x, j)}', f'@{here}K')),
                  (f'{here}K', ('nd', lambda M, L, j=j, parity=parity: [(
                      {UNREAD(i, parity, j): 0}, {}, f'lead {i} takes slot {parity}.{j}')],
                      after))]
        if gather:
            # The gather: no lead returns from it before every lead has entered.
            B += [(f'{p}G', ('add', lambda L, x=x: ENTERED(x), lambda L: 1, '_', f'@{p}GW')),
                  (f'{p}GW', ('await', lambda M, L, x=x: M[ENTERED(x)] == nodes, nxt))]
    B += [('DONE', ('gpost', lambda M, L: ({FIN: M[FIN] + 1}, {}), '@X')),
          ('X', ('done',))]
    return assemble(B)


def case(design, nodes, gathers):
    threads = [lead(design, i, nodes, gathers) for i in range(nodes)]
    mem = {FIN: 0, OVERWRITE: 0}
    for n in range(nodes):
        for p in range(2):
            mem[COUNT(n, p)] = 0
            for j in range(nodes):
                mem[SLOT(n, p, j)] = mem[TAG(n, p, j)] = mem[UNREAD(n, p, j)] = 0
    for x in range(len(gathers)):
        mem[ENTERED(x)] = 0
    shown = ''.join('g' if g else '-' for g in gathers)
    return f'exchange/{design} nodes={nodes} collects={shown}', threads, mem, nodes


# (design, nodes, each collect's gather - True where its total is not 0 - and
# expect: True, or the property it must fail). '-' in a case's name is a
# collect of nothing, its exchange alone.
CASES = [
    ('design', 2, (False, False, False), True),
    ('design', 3, (False, False, False), True),
    ('design', 3, (True, False, True, False), True),
    ('design', 4, (False, False, False), True),
    ('design', 4, (False, True, False), True),
    ('one_slot', 3, (False, False), 'SLOT'),
    ('one_slot', 3, (True, True, True), True),
    ('one_counter', 3, (False, False, False), 'VALUE'),
    ('one_counter', 3, (True, True, True), True),
]


def main():
    want = select(sys.argv)
    ok = True
    for design, nodes, gathers, expect in CASES:
        name, threads, mem, n = case(design, nodes, gathers)
        if not want(name):
            continue
        res = explore(name, threads, mem, max_seconds=900, liveness_max_states=3_000_000,
                      done=lambda M, n=n: M[FIN] == n)
        ok &= report(res, expect=expect)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
