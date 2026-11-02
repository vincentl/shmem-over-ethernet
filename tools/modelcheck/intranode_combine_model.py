#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""A reduction's intranode combine sliced over the node's members (the large-cluster
playbook's C5, `docs/design/intranode-combine-proposal.md`; modelled 2026-11-02,
not built).

As built (design §5.5, collectives.c), a node's lead combines its L members'
sources one after another into its own `dest` before the internode phase:
O(L x bytes) on one thread, 255 MiB of reads for a 1 MiB reduction at 256 PEs a
node. The proposal: once every member has arrived, member i combines slice i
of every member's source - in the descriptor's order, as the lead does today -
into slice i of the lead's `dest`, and adds to a `combined` counter; the lead
combines its own slice, waits for the counter, and runs the internode phase,
the release and the copy-outs as today. Each element is still combined once,
in the same order, so the result is bit for bit the lead's.

§5.5 gave three reasons for a single writer, and this model is written to each:

  1. **Lost updates** - concurrent read-modify-writes of one element. The
     slices are disjoint: WRITER checks that no element of the lead's dest is
     written by two members in one epoch.
  2. **In-place corruption** - a member combining before the lead has called
     reads the lead's source while the application still prepares it. Every
     member waits for every arrival before it reads a source: SOURCE checks
     that each source read is of the epoch being combined (invariant 17, push
     never pull), and the `inplace` cases alias each member's source and dest.
  3. **Epoch clobber** - a lead that returns early lets the next epoch's
     writers seed its dest while a slow member still copies out. The lead
     leaves last, and - the new argument - the next epoch's slices start only
     once every member has arrived at it, which each does only after its
     copy-out: RESULT checks that every element the lead reads before its
     internode phase, and every element a member copies out, is this epoch's.

Properties
  SOURCE  a source element read for epoch e holds epoch e's value;
  WRITER  an element of the lead's dest has one writer an epoch;
  RESULT  the lead reads, and every member copies, epoch e's value in every
          element;
  and no trapped state (the engine's AG EF done), with STUCK at dead ends.

Designs
  sliced      the proposal, above.
  serial      as built: the lead combines every slice; the members wait for the
              release. Kept to show the model holds the protocol of today too.
Breakages, each named for the property it must fail
  combine_before_all_arrive  a member combines after its own arrival, waiting
                             for nobody. -> SOURCE (reason 2)
  shared_slice               two members are given one slice. -> WRITER
                             (reason 1)
  lead_before_combined       the lead runs its internode phase and releases
                             without waiting for the combined counter.
                             -> RESULT (a slice not yet written)
  copy_before_release        a member copies out without waiting for the
                             release. -> RESULT
  lead_leaves_early          the lead does not wait for the copy-outs. In place,
                             its next epoch's source is its dest: a slow copier
                             reads the next epoch's values. -> RESULT (reason 3;
                             holds where source and dest differ and the
                             application leaves its dest alone, which is why
                             the in-place case is the one run)

**One argument the model found, not the designer.** Every member now waits
on `arrived` by equality, as the lead alone did; §5.4's equality waits are
sound only under bounded skew, and a member racing into the next epoch moves
the counter past the value a slow member waits for. The skew is bounded here
by `combined`: the lead releases only once every member has bumped it, and a
member bumps it only after passing its wait on `arrived`, so no member can
arrive for epoch e + 1 while one still waits for e's arrivals. The `serial`
control variant, written first with the members waiting on `arrived` as the
sliced ones do but without `combined`, was STUCK for exactly that reason, and
now waits for the release alone, as collectives.c does.

The waits are epoch.c's wait_eq with a sweep, whose futex protocol
chained_wake_model.py and wait_word_model.py cover, so they are `await` here.
Under x86-TSO the application's stores to its source, the combine's stores and
the copy-out's are plain (buffered); each is followed by a locked add - the
arrival's, the combined counter's, the copied counter's - which drains them, as
the library's are (§3.16).

Usage: intranode_combine_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

ARRIVED, COMBINED, RELEASED, COPIED = ('arrived',), ('combined',), ('released',), ('copied',)


def src(inplace, m, i):
    return ('src', m, i)


def dest(inplace, m, i):
    return ('src', m, i) if inplace else ('dest', m, i)


def slice_of(design, p, members):
    if design == 'shared_slice':
        return p % max(members - 1, 1)
    return p


def member(design, p, members, elements, epochs, inplace):
    """Member p of the node (p = 0 is the lead), running `epochs` reductions over
    `elements` elements back to back into the same dest."""
    lead = p == 0
    sliced = design != 'serial'
    my_slices = [slice_of(design, p, members)] if sliced else list(range(elements))
    if not sliced and not lead:
        my_slices = []
    my_slices = [i for i in my_slices if i < elements]
    B = []
    for k in range(epochs):
        e, pre = k + 1, f'p{p}e{k}_'
        nxt = f'@p{p}e{k + 1}_W0' if k + 1 < epochs else '@END'

        # The application prepares its source for this epoch: plain stores
        # (the program's own), drained by the arrival's locked add.
        for i in range(elements):
            B.append((pre + f'W{i}', ('store', lambda L, m=p, i=i: src(inplace, m, i),
                                      lambda L, e=e: e, False,
                                      '@' + pre + (f'W{i + 1}' if i + 1 < elements else 'A'))))
        # Arrive: collectives.c's stamped bump of `arrived`; the wake abstract.
        B.append((pre + 'A', ('add', lambda L: ARRIVED, lambda L: 1, '_', '@' + pre + 'WA')))
        if design == 'combine_before_all_arrive' and not lead:
            B.append((pre + 'WA', ('br', lambda L: True, '@' + pre + 'C0_0', '@' + pre + 'C0_0')))
        elif not sliced and not lead:
            # As built, a member waits for nothing but the release: a member's
            # equality wait on `arrived` would be unsound here, since the lead
            # releases without every member having looked, and a member back
            # for the next epoch moves the counter past the value a slow one
            # waits for (the engine found it: STUCK). In the sliced design the
            # same wait is sound because the release waits on `combined`,
            # which every member bumps only after passing it - the bounded
            # skew of §5.4, by a new counter.
            B.append((pre + 'WA', ('br', lambda L: True, '@' + pre + 'CB', '@' + pre + 'CB')))
        else:
            B.append((pre + 'WA', ('await', lambda M, L, e=e, n=members: M[ARRIVED] == e * n,
                                   '@' + pre + ('C0_0' if my_slices else 'CB'))))
        # Combine my slices: for each, read every member's element in the
        # descriptor's order (SOURCE), then write the lead's dest once (WRITER).
        for si, i in enumerate(my_slices):
            for m in range(members):
                after = f'C{si}_{m + 1}' if m + 1 < members else f'X{si}'
                B.append((pre + f'C{si}_{m}', ('load', lambda L, m=m, i=i: src(inplace, m, i),
                                                'v', '@' + pre + f'S{si}_{m}')))
                B.append((pre + f'S{si}_{m}', ('check',
                                                lambda M, L, e=e, m=m, i=i, p=p:
                                                None if L['v'] == e else
                                                f'SOURCE: member {p} read member {m} element {i} '
                                                f'at epoch {e} and found {L["v"]}',
                                                '@' + pre + after)))
            # One writer an element an epoch: the ghost records who wrote it.
            B.append((pre + f'X{si}', ('check',
                                        lambda M, L, e=e, i=i, p=p:
                                        None if M.get(('$wrote', i), 0) != e else
                                        f'WRITER: element {i} of the lead\'s dest written twice '
                                        f'in epoch {e}, by member {M.get(("$by", i), 0)} and {p}',
                                        '@' + pre + f'G{si}')))
            B.append((pre + f'G{si}', ('gpre', lambda M, L, e=e, i=i, p=p:
                                        ({('$wrote', i): e, ('$by', i): p}, {}),
                                        '@' + pre + f'D{si}')))
            # The combine's store into the lead's dest: plain, drained by the
            # combined counter's locked add (or, serial, by the release's).
            B.append((pre + f'D{si}', ('store', lambda L, i=i: dest(inplace, 0, i),
                                        lambda L, e=e: e, False,
                                        '@' + pre + (f'C{si + 1}_0' if si + 1 < len(my_slices)
                                                     else 'CB'))))
        if sliced:
            B.append((pre + 'CB', ('add', lambda L: COMBINED, lambda L: 1, '_',
                                   '@' + pre + ('WC' if lead else 'WR'))))
        else:
            B.append((pre + 'CB', ('br', lambda L: True, '@' + pre + ('WC' if lead else 'WR'),
                                   '@' + pre + ('WC' if lead else 'WR'))))
        if lead:
            # The lead waits for every slice, reads its dest whole - the
            # internode phase, abstracted to the reads it makes (RESULT) -
            # then releases and waits for the copy-outs.
            if sliced and design != 'lead_before_combined':
                B.append((pre + 'WC', ('await', lambda M, L, e=e, n=members: M[COMBINED] == e * n,
                                       '@' + pre + 'R0')))
            else:
                B.append((pre + 'WC', ('br', lambda L: True, '@' + pre + 'R0', '@' + pre + 'R0')))
            for i in range(elements):
                B.append((pre + f'R{i}', ('load', lambda L, i=i: dest(inplace, 0, i), 'v',
                                          '@' + pre + f'Q{i}')))
                B.append((pre + f'Q{i}', ('check',
                                          lambda M, L, e=e, i=i:
                                          None if L['v'] == e else
                                          f'RESULT: the lead read element {i} of its dest at epoch '
                                          f'{e} before its internode phase and found {L["v"]}',
                                          '@' + pre + (f'R{i + 1}' if i + 1 < elements else 'RL'))))
            B.append((pre + 'RL', ('add', lambda L: RELEASED, lambda L: 1, '_', '@' + pre + 'WP')))
            if design == 'lead_leaves_early' or members == 1:
                B.append((pre + 'WP', ('br', lambda L: True, '@' + pre + 'Z', '@' + pre + 'Z')))
            else:
                B.append((pre + 'WP', ('await',
                                       lambda M, L, e=e, n=members: M[COPIED] == e * (n - 1),
                                       '@' + pre + 'Z')))
        else:
            if design == 'copy_before_release':
                B.append((pre + 'WR', ('br', lambda L: True, '@' + pre + 'R0', '@' + pre + 'R0')))
            else:
                B.append((pre + 'WR', ('await', lambda M, L, e=e: M[RELEASED] == e,
                                       '@' + pre + 'R0')))
            # Copy out: read the lead's dest (RESULT), write my own.
            for i in range(elements):
                B.append((pre + f'R{i}', ('load', lambda L, i=i: dest(inplace, 0, i), 'v',
                                          '@' + pre + f'Q{i}')))
                B.append((pre + f'Q{i}', ('check',
                                          lambda M, L, e=e, i=i, p=p:
                                          None if L['v'] == e else
                                          f'RESULT: member {p} copied element {i} at epoch {e} '
                                          f'and found {L["v"]}',
                                          '@' + pre + f'O{i}')))
                B.append((pre + f'O{i}', ('store', lambda L, m=p, i=i: dest(inplace, m, i),
                                          lambda L, e=e: e, False,
                                          '@' + pre + (f'R{i + 1}' if i + 1 < elements else 'CP'))))
            B.append((pre + 'CP', ('add', lambda L: COPIED, lambda L: 1, '_', '@' + pre + 'Z')))
        B.append((pre + 'Z', ('set', 'v', lambda L: DEL, nxt)))
    B.append(('END', ('done',)))
    return assemble(B)


def case(design, members, epochs, inplace, tso):
    elements = members
    threads = [member(design, p, members, elements, epochs, inplace) for p in range(members)]
    mem = {ARRIVED: 0, COMBINED: 0, RELEASED: 0, COPIED: 0}
    for m in range(members):
        for i in range(elements):
            mem[src(inplace, m, i)] = 0
            if not inplace:
                mem[dest(inplace, m, i)] = 0
    name = (f'intranode_combine/{design} members={members} epochs={epochs} '
            f'{"inplace" if inplace else "separate"}')
    return name, threads, mem, tso


# (design, members, epochs, inplace, expect): True, or the property it must fail.
CASES = [
    ('sliced', 2, 2, False, True),
    ('sliced', 2, 2, True, True),
    ('sliced', 3, 2, False, True),
    ('sliced', 3, 2, True, True),
    ('sliced', 4, 2, True, True),
    ('serial', 3, 2, False, True),
    ('serial', 3, 2, True, True),
    ('combine_before_all_arrive', 3, 2, False, 'SOURCE'),
    ('combine_before_all_arrive', 3, 2, True, 'SOURCE'),
    ('shared_slice', 2, 1, False, 'WRITER'),
    ('shared_slice', 3, 1, True, 'WRITER'),
    ('lead_before_combined', 3, 1, False, 'RESULT'),
    ('copy_before_release', 3, 1, False, 'RESULT'),
    ('lead_leaves_early', 3, 2, False, True),      # a dest the application leaves alone: holds
    ('lead_leaves_early', 3, 2, True, 'RESULT'),   # in place: the next source is the dest
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, members, epochs, inplace, expect in CASES:
        for tso in (False, True):
            name, threads, mem0, _ = case(design, members, epochs, inplace, tso)
            if not keep(name):
                continue
            res = explore(name, threads, mem0, tso=tso, max_seconds=900,
                          liveness_max_states=2_000_000)
            ok &= report(res, expect=expect, tso=tso)
    sys.exit(0 if ok else 1)
