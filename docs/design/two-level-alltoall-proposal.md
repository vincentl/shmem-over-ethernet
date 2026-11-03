# A two-level alltoall: a message a member a destination node

*A proposal, written by the lead on 2026-11-02 at the author's word of that
morning (`docs/decisions/2026-11-01.md`, answer 6): the second of the four
design changes the large-cluster playbook names
(`docs/prompts/large-cluster-playbook.md`, C8 and the second row of its §5) to
be modelled here before any cluster session. Modelled by a cost model fitted to
the cloud's measurements; not built. The author decides whether it is built,
and when.*

## 1. The problem

`shmem_alltoall` as built (`collectives.c`, `osoe_alltoall`; design §5.5 as
built): every PE puts its block to every other PE directly - a message of a
24-byte header and the block, up to a chunk, with `OSOE_TRANSPORT_MORE` - from
the rank after its own round the team, then the team's barrier, whose quiet
completes the puts. A node of L PEs in a job of N PEs on n nodes receives
L × (N - L) messages, every one parsed and announced by one of its owner
threads - the lead's agent, or a progress helper - and each block's bytes
copied by that owner into its target's heap. An `alltoalls` with a stride puts
one element at a time: nelems messages a block.

The bytes are inherent: every PE has a block for every other, so a node
receives L × (N - L) × block bytes whatever the algorithm (the README's
"Scaling with nodes, not PEs"). The messages are not: at 8-byte blocks a node
of 256 PEs in a job of 512 nodes receives 33.5 million messages of 32 bytes,
each parsed, counted and announced, where the same bytes fit 131,000 messages
of 2 KiB. The third cloud session measured the shape of it (H4, H11, H12,
`docs/fast-link-results.md`): on two nodes an 8-byte alltoall took 306 µs at 16
PEs a node and 13,916 at 256 - 65,536 messages into each node, about 0.2 µs a
message - and a 4 KiB one 814 and 262,727 µs, 256 MiB into each node at about
1 GB/s, the owner's copy.

## 2. The proposal

**A message a member a destination node.** A member sends, for each other
node d of the team, one message carrying its L_d blocks for d's members in the
team's local order - a new message kind, `ALLTOALL_PIECE`, whose fixed part
names the team block, the epoch, the source's team rank, the destination's
symmetric offset, the block's bytes and the strides - chunked at the
transport's limit as a put is, with `MORE` on all but the last. The owner that
parses it at d copies each block into its member's `dest` at the place the
source rank gives - L_d memcpys, where a put's parser does one - and announces
the last. A node then receives L × (n - 1) messages, L times fewer, and sends
as many; the bytes a pair are unchanged but for the headers, which fall from
one a block to one a message. Blocks for the node's own members are memcpys,
as today. The barrier that ends the call, and its quiet, are as today.

**The schedule.** Node s sends its k-th message to node (s + k) mod n, every
member in the same order, so that at each step every link carries one stream
and every node receives from one node: the incast an alltoall makes when every
PE starts on the rank after its own - a node's L members then send to about
one node at a time, but every node to a different one only by luck - becomes
a permutation a step. The model does not price it; it is the ladder's to show.

**An `alltoalls` with a stride** sends the same message: its L_d blocks'
elements gathered from the strided source as the member reserves and copies
them, and scattered by the parser into the strided `dest`. One message a
member a node where today there is one an element: nelems times fewer.

**What does not change.** Push, never pull (invariant 17): a block lands in a
`dest` before its owner has called, as the specification allows an alltoall
and as the puts do today, and nothing is read from a peer. The barrier's quiet
completes every member's messages before any arrives; `OSOE_CHECK_COLLECTIVES`
compares the node's calls at that barrier, as today. The active-set form
(`active-set.c`) and the single-node job are untouched. The rank map gives the
destination's members and their local order (`team.h`, the lead's `local_ids`
of a team on each node); the parser needs the team's members on its own node,
which the lead's engine learns at the team's creation for the barrier's
arrivals and the helpers can be given.

**What changes.** A new message kind, so the wire's version; the parser's
copy of L_d blocks into L_d heaps where it copies one into one; the sender's
gather of its blocks for one node into one reservation, which the connection
buffer's room shapes as a put's parts are shaped (`payload_limit`,
`transport-internode.c`); a team's local member lists in the engine's team
record for every team, not only the barrier's needs; and the design notes'
§5.5 "as built" paragraph on alltoall. Not a protocol with waits of its own:
the only wait is the barrier's, unchanged, which is why there is a cost model
here and no interleaving model - `active_set_model.py` already holds the
properties an alltoall's landing has (ENTERED, LANDED, RETURNED), and this
message keeps them.

## 3. The cost model, and what it says

`tools/modelcheck/alltoall_timing.py` (registered as `model.alltoall_timing`)
prices the slowest node's time as the barrier that ends the call, the quiet's
round trip and the pipeline's fill, each PE's own issue of its messages (about
1.5 µs a message on the node's shared lines, C4), and the owners' work to take
what arrives - a parse and an announce a message, a copy a byte - or the
link's time for the bytes, whichever is longer, with a block's own memcpy
beside. Fitted to the twenty alltoall points of the third cloud session, it
holds every one within a factor of 1.52 - the 4 KiB alltoall at 256 PEs a node
is the one it underprices, 0.66, where the copy rate of one owner under 256
writers fell to 1 GB/s - and its variant without a per-message cost misses the
8-byte points by up to 3.7 times, which shows the fit rests on the messages.
Three forms: as built; the two-level message; and a node-level Bruck -
⌈log₂ n⌉ rounds among the leads, half the node's data a round, at the price of
log₂(n) / 2 times the bytes and a repack a round.

The slowest node's time by the model, microseconds, at no helpers, as built /
two-level:

| Nodes × PEs a node | 8 B blocks | 64 B | 1 KiB | 4 KiB |
|---|---|---|---|---|
| 8 × 8 | 465 / 319 | 480 / 334 | 733 / 587 | 1,542 / 1,397 |
| 8 × 64 | 7,070 / 1,212 | 8,015 / 2,157 | 24,206 / 18,348 | 76,018 / 70,439 |
| 8 × 256 | 99,319 / 12,508 | 114,431 / 27,619 | 373,491 / 287,701 | 1,202,483 / 1,120,778 |
| 64 × 64 | 60,365 / 7,643 | 68,866 / 16,143 | 214,587 / 161,865 | 680,895 / 630,683 |
| 512 × 8 | 13,289 / 2,655 | 14,366 / 3,732 | 32,834 / 22,200 | 91,932 / 81,298 |
| 512 × 64 | 486,586 / 58,949 | 555,534 / 127,897 | 1,737,495 / 1,309,858 | 5,519,770 / 5,112,497 |
| 512 × 256 | 7,190,367 / 853,098 | 8,293,530 / 1,956,262 | 27,204,907 / 20,942,194 | 87,721,312 / 81,756,824 |

- **At small blocks the two-level form is 6 to 8 times as fast** wherever a
  node has 64 PEs or more: an 8-byte alltoall of 512 nodes × 256 PEs from 7.2 s
  to 0.85 s, of 64 × 64 from 60 ms to 7.6. The message count is what binds
  there, and the form takes L of it away.
- **From 1 KiB the gain narrows to 10 to 30%, and at 4 KiB it is under 10%**:
  the bytes bind, and the bytes are the same. A 4 KiB alltoall at 512 × 256
  takes 82 to 88 s by either form: each node receives 137 GB, which the link
  carries in 51 s and one owner copies in 80. That is the design's inherent
  O(N) a PE, and no algorithm changes it; what shortens it is more owners
  (two helpers: 52 s) and the link.
- **The crossover**: the two-level form is at least twice as fast below 256
  bytes a block at every node count with 8 PEs a node or more, and never at
  one PE a node, where a message a block already is a message a node.
- **Bruck** wins over the two-level form only at 8 to 64 bytes a block, and
  there by a quarter to a half, at the price of a second algorithm and a repack
  a round; at 1 KiB it is three to seven times slower. Not worth building.
- **`alltoalls` with a stride**: the as-built form's messages are nelems times
  the table's, the two-level form's the same as the table's; at eight elements
  a block the gain is eight times the table's ratio.

The numbers are a model's: fitted to one cluster's two and eight nodes, and
extrapolated to hundreds, where the message cost, the copy rate and the link
are the ladder's to measure (the playbook's phase B, the `alltoall` row). What
the model cannot say is the incast's cost, which the schedule is for.

## 4. What it costs

- **Code**: the message kind and its parser (`am.h`, `progress.c`'s `applied`
  and the parser's copy into several heaps), the sender's gather and
  reservation (`collectives.c`, a few dozen lines beside `osoe_alltoall`), the
  engine's team member lists (`internode-am.c`, `progress.h`), the strided
  gather and scatter, the wire's version bump; about two hundred lines, and a
  test that fails without it - the message count a node receives
  (`transport.am.messages_applied`) at a known alltoall, and the
  `osoe-pacer alltoall` oracles for the bytes.
- **The wire**: a new kind; a node of another version refuses it, as the
  handler table's checksum does.
- **Memory**: none; the message is gathered into the connection buffer as a
  put is.
- **Scratch for the schedule**: none; the order is arithmetic.

## 5. Alternatives weighed

- **A message a node pair** - the node's L × L_d blocks gathered by its lead
  into one message: another factor L fewer messages, but the lead copies the
  whole node's output, L × (N - L) × block bytes, on one thread, which at 1 KiB
  blocks and 256 PEs is gigabytes; and the chunk splits a message past 64 KiB
  anyway, so past small blocks the message count is the bytes' over the chunk
  either way. The member-level message takes the L-fold without the gather.
- **Bruck at node level**: above; not worth it.
- **A pairwise exchange with a barrier a step**: the schedule's order without
  the barriers, since a barrier a step is n barriers; the connection buffers'
  back-pressure keeps the steps loosely in order.
- **Leaving it**: an 8-byte alltoall at 256 PEs a node on 512 nodes of 7 s by
  the model, and bale's and ISx's exchanges are of small blocks.

## 6. Questions for the author

*Answered 2026-11-03*: the author took every recommendation below as his answer (`docs/decisions/2026-11-03.md`).

1. **Build it?** The model says the gain is 6 to 8 times at small blocks and
   nothing at large ones, on the cloud's costs. *Recommended*: after the first
   cluster session's ladder has an alltoall row at 64 nodes - the model's
   message cost at many peers is its least certain term - and before the
   second; built here, the wire's version bumped with whatever else that
   stretch changes.
2. **The schedule's order**: (s + k) mod n from k = 1, every member alike, or
   the as-built start at the rank after one's own, which already staggers a
   node's members across destinations within a node. *Recommended*: the node
   order; measured on the ladder against the as-built one, since the model
   cannot price the incast.
3. **The chunking of a member's message** at many PEs a node: 256 blocks of 1
   KiB are 256 KiB, four chunks; a message's parts take the connection
   buffer's room as a put's do. *Recommended*: as a put's parts, no new rule.
4. **The strided form first?** `alltoalls` with a stride is where the as-built
   form is worst - a message an element - and the same message fixes both.
   *Recommended*: one change, both forms.

## 7. The record

The model: `tools/modelcheck/alltoall_timing.py`, `model.alltoall_timing`;
the decision record: `docs/decisions/2026-11-02-two-level-alltoall-model.md`.
