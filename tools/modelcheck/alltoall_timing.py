#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Not an interleaving model: the cost of an alltoall as built against a two-level
one and a node-level Bruck (the large-cluster playbook's C8;
`docs/design/intranode-combine-proposal.md`'s sibling,
`docs/design/two-level-alltoall-proposal.md`; 2026-11-02, not built).

**As built** (`collectives.c`, `osoe_alltoall`): every PE puts its block to every
other PE directly - a message of a 24-byte header and the block, up to a chunk -
with `OSOE_TRANSPORT_MORE`, then the team's barrier, whose quiet completes the
puts. A node of L PEs in a job of n nodes (N = nL PEs) therefore receives
L x (N - L) messages, each parsed and announced by one of its owner threads -
the lead's agent, or a progress helper - and each block's bytes copied into
its target's heap by the owner that parses it. An `alltoalls` with a stride
puts one element at a time: nelems times the messages.

**Two-level**: a member sends one message a destination node, carrying its L_d
blocks for that node's members in the team's local order - a new message kind
whose header names the team, the epoch, the source rank and the dest's place,
and whose owner at the far end copies each block into its member's dest - so
a node receives L x (n - 1) messages, L times fewer, each chunked at the
transport's limit; the bytes a pair are unchanged, and the headers fall from
one a block to one a message. Node s sends its k-th message to node
(s + k) mod n, so that every link carries one stream at a time (the schedule,
whose gain against the incast this model does not price).

**Bruck at node level**: ceil(log2 n) rounds among the leads, each moving half
of the node's data to one peer, as fcollect's allgather does; messages a node
become a few a round, at the price of log2(n) / 2 times the bytes and a copy
of the data at every round.

The cost of each, on one node, as the slowest node's time:

    T = T_sync + max(messages_in x o / owners + bytes_in / (B_copy x owners),
                     bytes_in / B_link, bytes_out / B_link)
        + blocks_in x c_block

o is an owner's cost to parse and announce one message, whatever its size;
B_copy an owner's rate copying payload into heaps; c_block a per-block cost -
the memcpy's call and the dest's arithmetic - which the two-level message pays
a block as the put does; owners is 1 + the helpers (H, `OSOE_PROGRESS_HELPERS`),
each owning the data wires to a share of the peers; T_sync the barrier that
ends the call and the senders' issue. The sender's cost a put - the memcpy
into the connection buffer and three locked operations on the node's shared
lines (C4) - is folded into o here, since the same thread's work bounds both
ends alike in a symmetric exchange; the model is of the receiving node.

`check` fits the as-built form to the third cloud session's alltoalls - two
nodes at 16 to 256 PEs a node (H12) and eight at 8 to 32 (H4, H11), 8-byte and
4 KiB blocks, at no helpers and at two - and holds every point within a factor
of 1.6, the spread the rounds themselves show; its broken variant, the
per-message cost dropped (`no_message_cost`), misses the 8-byte points by
three to twenty times, which shows the fit rests on o and not on T_sync alone.
`table` prints the three forms at 8 to 4,096 nodes and 8 to 256 PEs a node;
`crossover` the block size below which the two-level form is twice as fast,
and where Bruck beats it.

Usage: alltoall_timing.py check | table | crossover
"""
import math
import sys

# Fitted to the third cloud session (docs/fast-link-results.md, H4, H11, H12):
# one owner, unbound, Google Cloud c4-standard-16, 23 Gbit/s.
MESSAGE_NS = 170.0          # o: an owner's parse and announce of one message
COPY_BYTES_PER_NS = 1.7     # B_copy: an owner's copy into heaps, bytes a nanosecond
BLOCK_NS = 20.0             # c_block: a block's memcpy call and arithmetic
LINK_BYTES_PER_NS = 2.7     # B_link: one node's share of the link each way
HEADER_BYTES = 24           # a put's message header and fixed part (am.h)
CHUNK_BYTES = 64 << 10      # OSOE_AM_CHUNK
PUT_NS = 1500.0             # c_put: a PE's issue of one message - the reservation's
                            # CAS, the memcpy, the commit, the dirty bit, the wake -
                            # on lines the node's L writers share (C4)
FILL_NS = 150_000.0         # the quiet's round trip at the end, and the first hop
BARRIER_NS = 100_000.0      # the barrier that ends the call: measured 125 to 672 us
BARRIER_NS_PER_PE = 2200.0  # at 16 to 256 PEs a node on two nodes (H12), 132 to 184
BARRIER_NS_PER_LEVEL = 20_000.0  # at 8 to 32 on eight (H4)
BRUCK_ROUND_NS = 45_000.0   # a hop between leads, the cloud's put round trip


def barrier_ns(n, pes_a_node):
    levels = math.ceil(math.log(max(n, 2), 8))
    return BARRIER_NS + BARRIER_NS_PER_PE * pes_a_node + BARRIER_NS_PER_LEVEL * levels


def node_time(n, messages_in, bytes_in, blocks_in, puts_a_pe, pes_a_node, helpers,
              message_ns=MESSAGE_NS):
    """The slowest node's time: the barrier and the fill, each PE's own issue of
    its puts, and the owners' work to take what arrives - or the link's time
    for it, whichever is longer - with a block's own cost on top."""
    owners = 1 + helpers
    apply = messages_in * message_ns / owners + bytes_in / (COPY_BYTES_PER_NS * owners)
    link = bytes_in / LINK_BYTES_PER_NS
    return (barrier_ns(n, pes_a_node) + FILL_NS + puts_a_pe * PUT_NS + max(apply, link) +
            blocks_in * BLOCK_NS)


def direct(n, L, block, helpers=0, nelems_strided=1, message_ns=MESSAGE_NS):
    """As built: a message a block (an element, strided)."""
    if n < 2:
        return barrier_ns(n, L)
    blocks_in = L * (n - 1) * L
    messages_in = blocks_in * nelems_strided
    bytes_in = messages_in * HEADER_BYTES + blocks_in * block
    puts_a_pe = (n - 1) * L * nelems_strided
    return node_time(n, messages_in, bytes_in, blocks_in, puts_a_pe, L, helpers, message_ns)


def two_level(n, L, block, helpers=0):
    """A message a (member, destination node), its L blocks together, chunked."""
    if n < 2:
        return barrier_ns(n, L)
    payload = L * block
    pieces = max(1, math.ceil(payload / CHUNK_BYTES))
    messages_in = L * (n - 1) * pieces
    blocks_in = L * (n - 1) * L
    bytes_in = messages_in * HEADER_BYTES + blocks_in * block
    puts_a_pe = (n - 1) * pieces
    return node_time(n, messages_in, bytes_in, blocks_in, puts_a_pe, L, helpers)


def bruck(n, L, block, helpers=0):
    """ceil(log2 n) rounds among the leads, half the node's data a round, every
    byte copied by the node's PEs at each round."""
    if n < 2:
        return barrier_ns(n, L)
    rounds = math.ceil(math.log2(n))
    node_data = L * (n - 1) * L * block      # what the node must deliver in all
    per_round = node_data / 2
    pieces = max(1, math.ceil(per_round / CHUNK_BYTES))
    total_bytes = rounds * (per_round + pieces * HEADER_BYTES)
    owners = 1 + helpers
    apply = rounds * pieces * MESSAGE_NS / owners + total_bytes / (COPY_BYTES_PER_NS * owners)
    link = total_bytes / LINK_BYTES_PER_NS
    repack = rounds * per_round / (COPY_BYTES_PER_NS * L)     # the node's PEs repack in parallel
    return barrier_ns(n, L) + FILL_NS + rounds * pieces * PUT_NS / L + max(apply, link) + rounds * BRUCK_ROUND_NS + repack


# (nodes, PEs a node, helpers, block bytes, measured microseconds): the third
# cloud session, osoe-bench's alltoall, the median of 200 trips.
MEASURED = [
    (2, 16, 0, 8, 306), (2, 32, 0, 8, 504), (2, 64, 0, 8, 1203), (2, 256, 0, 8, 13916),
    (2, 16, 0, 4096, 814), (2, 32, 0, 4096, 2741), (2, 64, 0, 4096, 11129),
    (2, 256, 0, 4096, 262727),
    (8, 8, 0, 8, 477), (8, 16, 0, 8, 760), (8, 32, 0, 8, 1493),
    (8, 8, 0, 4096, 1321), (8, 16, 0, 4096, 4562), (8, 32, 0, 4096, 17314),
    (8, 8, 2, 8, 398), (8, 16, 2, 8, 644), (8, 32, 2, 8, 1133),
    (8, 8, 2, 4096, 1014), (8, 16, 2, 4096, 3323), (8, 32, 2, 4096, 13210),
]

TOLERANCE = 1.6


def fit(message_ns=MESSAGE_NS, out=sys.stdout):
    worst = 0.0
    print(f'{"nodes":>5} {"PEs":>4} {"H":>2} {"block":>6} {"measured us":>12} {"model us":>9} {"ratio":>6}',
          file=out)
    for n, L, H, block, measured in MEASURED:
        model = direct(n, L, block, H, message_ns=message_ns) / 1000.0
        ratio = model / measured
        worst = max(worst, ratio, 1 / ratio)
        print(f'{n:>5} {L:>4} {H:>2} {block:>6} {measured:>12,} {model:>9,.0f} {ratio:>6.2f}', file=out)
    return worst


def us(ns):
    return ns / 1000.0


def table(out=sys.stdout):
    print('The slowest node\'s time, microseconds: direct (as built) / two-level / Bruck, '
          'at no helpers and at two\n', file=out)
    for n in (8, 64, 512, 4096):
        for L in (8, 64, 256):
            print(f'nodes={n} PEs a node={L} (PEs={n * L:,})', file=out)
            print(f'  {"block":>7} | {"direct 0":>12} {"two-level 0":>12} {"Bruck 0":>12} | '
                  f'{"direct 2":>12} {"two-level 2":>12} {"Bruck 2":>12}', file=out)
            for block in (8, 64, 1024, 4096, 65536):
                row = []
                for H in (0, 2):
                    row += [us(direct(n, L, block, H)), us(two_level(n, L, block, H)),
                            us(bruck(n, L, block, H))]
                print(f'  {block:>7} | {row[0]:>12,.0f} {row[1]:>12,.0f} {row[2]:>12,.0f} | '
                      f'{row[3]:>12,.0f} {row[4]:>12,.0f} {row[5]:>12,.0f}', file=out)
            print(file=out)


def crossover(out=sys.stdout):
    print('The block size below which the two-level form is at least twice as fast as the '
          'direct one, and the largest at which Bruck beats the two-level form, at no helpers\n',
          file=out)
    sizes = [8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 65536]
    print(f'{"nodes":>5} {"PEs":>4} {"two-level 2x below":>20} {"Bruck wins to":>14}', file=out)
    for n in (8, 64, 512, 4096):
        for L in (1, 8, 64, 256):
            twice = [b for b in sizes if direct(n, L, b) >= 2 * two_level(n, L, b)]
            bw = [b for b in sizes if bruck(n, L, b) < two_level(n, L, b)]
            print(f'{n:>5} {L:>4} {(str(max(twice)) + " B") if twice else "never":>20} '
                  f'{(str(max(bw)) + " B") if bw else "never":>14}', file=out)


def check(out=sys.stdout):
    ok = True
    print('The fit of the as-built form to the third cloud session:', file=out)
    worst = fit(out=out)
    verdict = worst <= TOLERANCE
    print(f'  worst ratio {worst:.2f} against a tolerance of {TOLERANCE}: '
          f'{"ok" if verdict else "FAIL"}', file=out)
    ok &= verdict
    print('\nThe broken variant, no per-message cost (must miss the 8-byte points):', file=out)
    broken = fit(message_ns=0.0, out=out)
    verdict = broken > TOLERANCE
    print(f'  worst ratio {broken:.2f}: {"fails as it must" if verdict else "HOLDS, UNEXPECTED"}',
          file=out)
    ok &= verdict
    # Two pins of what the table says, so that a change to the forms is seen.
    gain = direct(512, 256, 8) / two_level(512, 256, 8)
    print(f'\nAt 512 nodes of 256 PEs and 8-byte blocks the two-level form is {gain:.1f} times '
          f'the direct one: {"ok" if gain >= 4 else "FAIL"}', file=out)
    ok &= gain >= 4
    same = direct(512, 256, 65536) / two_level(512, 256, 65536)
    print(f'At 64 KiB blocks it is {same:.2f} times: the bytes bind both: '
          f'{"ok" if 0.9 <= same <= 1.2 else "FAIL"}', file=out)
    ok &= 0.9 <= same <= 1.2
    return ok


if __name__ == '__main__':
    what = sys.argv[1] if len(sys.argv) > 1 else 'check'
    if what == 'check':
        sys.exit(0 if check() else 1)
    if what == 'table':
        table()
    elif what == 'crossover':
        crossover()
    else:
        print(__doc__)
        sys.exit(2)
