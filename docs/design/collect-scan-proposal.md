# collect's places by a scan of the node totals (P30)

*Stage 1 of P30 of `docs/prompts/v0.2-open-items.md` - the second of the two
ways the author asked `collect`'s places to be built besides the members'
vector (his A11 of 2026-10-11, item 12 of `docs/decisions/2026-10-11.md`) -
written 2026-10-12: the design and its model, for the lead's decision whether
stage 2, the C, goes ahead. Not built. Its finding first: **P30 takes exactly
the hops P29 takes wherever P29's vector of node totals is a small collective -
at the defaults, up to 128 nodes - so it cannot be faster than P29 at eight
nodes. From 129 nodes it saves the large class's grant, up to a hop, and its
gain past that is bytes, which buy time on a real link only at about a thousand
nodes and more**, a scale the project cannot run (below, "Does it pay?").*

*Stage 2 was built on 2026-10-13 by the second cloud session, from the author's
1(a) of that day (`docs/decisions/2026-10-13.md`): "Stage 2, as built", at the
end, says what was built, where it differs from the list above, and what is a
wire change and what a layout one.*

## What P30 changes

P29 (on branch `collect-scan` at `2b4c3375`) gives every lead every node's
total: a reduction of a vector of the team's *N* nodes, eight bytes a node,
each lead's own total at its node's position; every lead then knows every
node's base, and in the gather - Bruck's allgather among the leads, whose puts
land at their places in the receiver's buffer - the *sender* computes each
share's place from them.

P30 gives each lead only what it needs of its own: **its node's base and the
total, from a scan of the nodes' totals among the leads**; and **each step of
the gather carries the byte ranges of the shares it sends**, so that a lead
forwards at a later step the shares it received without knowing any other
node's place. Its bytes a call are O(log *N*) a lead - a few words a step -
where P29's are O(*N*) at every level of the reduction's tree.

**Where it applies.** Where P29's way applies - a team spanning nodes, each of
which holds one run of its ranks - and where also the runs, met in team-rank
order, are the node set's positions in order or in reverse. That is every team
strided in world ranks over a block distribution whose node numbers rise with
the ranks, as Slurm's do: the world, a positive stride, in order; a negative
stride in reverse. P29's walk (`osoe_team_node_runs`) already finds the runs; it
would note which of the three the order is. Elsewhere P30 cannot keep a step's
shares to two ranges: on a block map whose nodes are numbered out of the ranks'
order, consecutive positions can lie apart in the result, up to a range a share,
and P29's way stays; on a cyclic or plane distribution, or a team of one node, the
members' vector stays.

## The scan

**The library's own scan does not serve**, for three reasons.
`osoe_internode_scan` (`internode-generic.c`; `scan_model.py`,
`scan_pipeline_model.py`, `lane_scan_model.py`) is every member's, not the
leads': a pre-order tree over the team's *M* ranks, each place a member, at the
scans' fan-out of 4 under `link`. Called with each member's length it would give
every member its own place, in team-rank order whatever the stride - its one
advantage - but:

- **it is deeper than P29's tree, and on every member's thread.** At eight
  nodes of 16 PEs, 128 members, its tree is four levels each way - from the
  root at rank 0 to ranks 33 and 50, two of them crossing nodes, then two within
  node 3 - where P29's tree over the eight nodes, at the ceiling's fan-out of 8,
  is one level each way. It could only be slower;
- **it gives no total.** Its down carries the sum of the ranks before a subtree;
  the total is the root's alone, after its up-sweep. "The total with it" would
  need one more element on every down of every scan, or one more collective - a
  broadcast from the last rank's node or a reduction, each a tree of its own;
- **its rows are every member's.** Its slots and counters count every scan of
  the team on every member, one base a team (`scan_pieces`): a scan among the
  leads alone, through the same rows, would leave the other members' bases
  behind, and the team's next member scan would wait forever.

**The leads' scan.** So the scan P30 needs is a new call of the data half, one
eight-byte word among the team's node leads - `osoe_internode_scan_node_totals`
(team, epoch, this node's total) giving back the totals before this node and
the total:

- **Its tree** is a pre-order tree over the node set's positions 0 ... *N* - 1,
  rooted at position 0: a place covering positions [*a*, *b*) is position *a*,
  and its children split [*a* + 1, *b*) into at most *k* consecutive blocks, so
  every subtree is a contiguous range of positions - the member scans'
  arithmetic (`scan_place`, `scan_children`, `scan_child`), which takes any
  count, over positions instead of ranks, each place's lead found by
  `lead_of`. Not the reductions' tree (`collective-tree.h`: position *i*'s
  children *k i* + 1 ... *k i* + *k*), whose subtrees are not contiguous, so that
  no sum before a subtree can be made from subtree totals over it - the model's
  `heap_tree`, failing. At the same fan-out the two trees are equally deep: each
  holds 1, 9, 73 and 585 positions at depths 0 to 3 at *k* = 8.
- **Up**: a lead waits for each child's subtree total, adds its own node's, and
  puts the sum into its parent's small slot for its position, then a fenced add
  of the parent's arrival counter for that position. **Down**: the root has
  nothing before it and holds the total at the end of its up-sweep; every other
  lead waits for its release counter and takes (the totals before its subtree,
  the total) from its small release slot; child *j* is sent (its parent's
  before, plus the parent's own total, plus the subtrees of children 0 ... *j* -
  1; the total) - the member scans' down, and the total beside it. **The total
  comes with the scan itself**: no step more.
- **Its words are the reductions'**: the team number's small rows - a child
  position's slot and the release slot, behind a piece's header - and its
  arrival and release counters, one piece up and one down, always small (8 bytes
  up and 16 down, under the smallest `OSOE_SMALL_COLLECTIVE_BYTES`, 128), so no
  lane and no grant. Each lead advances its bases as `in_pieces` does - each
  child position's it had by one, and the release's unless it is the root - so
  a tree that differs from the reductions' and broadcasts' on the same team is
  safe for the reason theirs differ from each other: the root sends its downs
  only once every lead has sent up, each lead sends up only once its children
  have, so no lead starts the team's next collective before every lead has
  read its child slots, and nothing is written into a release slot before its
  lead's own arrival. Its header names it - a `collective` of its own, bit 30
  set and bit 31 clear, which neither a reduction's identity nor a broadcast's
  root index can be - so that a lead in the scan whose peer is in a reduction is
  named where their pieces meet, as two collectives' pieces are now. No new
  words: no change to the internal region's layout.
- **In team-rank order.** The scan runs in position order: *before* is the
  totals at the positions below this one. A node's base is *before* where the
  runs meet the positions in order, and *total* - *before* - its own total where
  they meet them in reverse - the walk's order, not the stride's sign.

## The ranges

**What a lead holds.** In the gather, which keeps fcollect's pairs - position
*i* sends to *i* - 2^*s* at step *s* - a lead's shares are always consecutive
positions, *i* ... *i* + *h* - 1, and with the runs in position order or its
reverse their bytes are one run of the result in team-rank order: up from the
lead's base under the first, down from its share's end under the second -
wrapping past position *N* - 1 at most once, to the result's other end. So a
lead keeps what it holds, for the call and in its own memory, as **two
ranges**: `first`, the bytes of its positions up to *N* - 1, and `second`, of
those past it - each (start, end), and while empty kept at the byte where its
bytes will begin: `first` at its own share, `second` at 0 in order and at the
total in reverse. At entry it holds its own share, [base, base + its total).

**What a step carries, and where.** At step *s* a lead puts what it holds into
its receiver's buffer at the same places - a put a range, none for an empty one,
one for both where they touch - then its **record**, the gather's epoch and the
two ranges, 40 bytes, past the counter in the receiver's DATA[*s*] line, then
the fenced add of that counter, as now: the record is fenced as the data is.
The line holds the counter alone today; the record fits its 128 bytes.

**The last step.** It sends *N* - 2^(*S* - 1) shares, *S* = ⌈log₂ *N*⌉ - fewer
than the lead holds unless *N* is a power of two - and the shares it does not
send begin at its receiver's own share: what it holds runs on into it. So the
lead cuts what it holds where that share begins - its start in order, its end
in reverse - in `first` where its positions stop short of the wrap, else in
`second`, `first` then whole: the positions, not the bytes, say which, so an
empty share at the cut is no ambiguity. **A lead learns its last-step
receiver's share from the receiver itself**: at entry each lead puts its own
share's range, with the epoch, past the counter in the READY[*S* - 1] line of
the lead that writes to it at the last step, before that READY add, fenced -
past the gather's shape where *S* is 1 and the line is READY[0]'s. The
writer reads it after its READY[*S* - 1] wait. Seven nodes is the first count
whose cut falls inside a range a lead received rather than at its edge; the
model's arithmetic holds every count to 70.

**Taking a record.** Once DATA[*s*] says the step's shares landed, the
receiver reads its record and checks it: the epoch its own, each range within
the result, and each continuing what it holds - while *i* + 2^*s* is below *N*,
the writer's `first` continues the receiver's `first` and its `second` the
receiver's `second`; past that every position the writer sends lies past the
receiver's wrap, its `first` continues the receiver's `second`, and its
`second` is empty. Then it holds the union, and forwards it at step *s* + 1.
After the last step it holds the whole result, [0, total), which it checks.

**No record is overwritten before it is taken.** A record of step *s* is put
only after its writer's READY[*s*] wait - the wait that already guards the
data - and its writer's next one, in collective *x* + 1, needs READY[*s*] at
*x* + 2, which the receiver adds only on entering *x* + 1, after its return
from *x*, after it took every record of *x*. So the case "a record reused by
the next collective before it was forwarded" cannot arise, and for the reason
the buffer is safe: READY has a second duty under P30, and the model's
`range_before_ready` - the record put ahead of the wait, on the reasoning that
it is the library's word and not the receiver's buffer - shows it would
otherwise (on four nodes lead 1 finishes collective 0 and puts collective 1's
record over the one lead 0 has yet to take). The counters' compare as at least
stays sound: while the receiver is in *x* its DATA[*s*] never passes *x* + 1,
since the *x* + 1 add waits behind the same READY. The share in READY[*S* - 1]'s
line is safe the other way about: the receiver writes it again only on entering
*x* + 1, after its own last DATA wait, which the writer's add satisfies after
its read. Within a collective each step has its own line.

**What the shape check compares.** As now, at step 0, before any put: the
epoch, the result's bytes - the total, every lead's alike from the same scan -
and the layout, whose `by_places` becomes a layout of three values: a block,
places, ranges, so that a lead in ranges against one in places is named, and
`block` is 0 under ranges. A record's epoch and its ranges' continuity are
checked on receipt, and the cut's point against what the lead holds, each
naming the writer's node and lead as `check_shape` does; and the whole result
at the end, a broken invariant if not.

## What it costs, against P29

At the defaults - the fan-out's ceiling 8, small collectives to 1 KiB, the
reduction's tree at the ceiling for a small vector and, for a large one, at the
fan-out the shape chooser takes on the cluster's link (`shape_timing.py`'s
`choose_shape`) - counting a call's places and its gather, the hops on its
critical path and the bytes its places and records move:

| | 8 nodes | 256 nodes |
|---|---|---|
| P29's places | 2 hops; a vector of 64 bytes, small | 6 hops at the chooser's fan-out of 6, and the large class's grant: up to a hop, a round trip where a parent enters late; a vector of 2 KiB, through lanes |
| P30's places | 2 hops; 8 bytes up, 16 down | 6 hops; the same |
| the gather, either way | 4 hops: READY's crossing and 3 steps | 9 hops |
| bytes, places and records, P29 / P30 | 1,120 / 1,544 | 1,052,640 / 102,344 |
| the busiest lead's link a pass, P29 | 560 bytes, the root's seven pieces | 12.4 KB, an interior lead's six: about 28 µs over its six passes at the cluster's 2.7 GB/s |
| small puts P30 adds | 32: a record a step and the share at entry, a lead | 2,304 |

The places take as many messages either way, *N* - 1 up and *N* - 1 down. At
eight nodes P30 moves more bytes than P29, its records more than P29's vector
saves; it moves fewer from 14 nodes on. P29's vector is a small collective up to
128 nodes at the defaults (8 bytes × 128 = 1 KiB), a large one past that, at
the chooser's fan-out: 5 or 6 from 129 to 1,024 nodes, the tree's depth that of
P30's at 8 throughout. At 1,024 nodes its 8 KiB vector holds an interior lead's
link for about 146 µs over the passes, some three of the cluster's busy hops (45
to 52 µs), and at 4,096 nodes about 0.8 ms; P30's words there stay a few dozen
bytes a message.

## Does it pay?

**Not at any node count the project can run.**

- **At eight nodes**, the cluster's, the two take the same hops, and the bytes
  between them are tens a message: P30 cannot be faster than P29, and its 32
  small puts more - each a message the receiving progress thread handles, if
  inside messages already sent - make it, if anything, slower. H25's
  measurement could hardly tell them apart.
- **From 129 to 254 nodes**, tier 3's logical nodes in the VM, P29's vector
  becomes a large collective, and P30 saves the grant - at most a hop of
  sixteen, and up to a round trip where a parent lead enters after its
  children - and about 2 KiB × 6 of an interior lead's bytes a pass, which on
  the VM's loopback cost microseconds. H24 might see the grant's step at 129 nodes; the
  VM's run-to-run noise, up to a fifth (§5.8), may hide it. Raising
  `OSOE_SMALL_COLLECTIVE_BYTES` to the vector's size would remove the same step
  from P29.
- **Its gain is bytes, and bytes buy time on a real link at about a thousand
  nodes and more**: the three busy hops at 1,024 nodes and the 0.8 ms at 4,096
  above - the scale of the RoCE target the notes keep in view, not of any
  measurement the project can make.

So building P30 now would add a scan protocol of its own, a third layout to
the gather - two ranges kept and merged by position, a cut, a record and a
share in the internal region's lines, and their checks - and the tests for
them, for no time the project can measure. **And it is not the lever for
latency at small node counts.** Neither P29 nor P30 is bounded by its bytes
there but by its tree: up and down, two hops at eight nodes. A direct exchange -
each lead putting its total into every other lead's word for its position, and
each waiting for the *N* - 1 adds on one counter - would give every lead every
node's total, P29's information, in one hop, with *N* - 1 messages a lead,
which the tree's root handles anyway; it needs no READY, since a lead reads its
words before its gather's first put, and no lead finishes the gather, to write
its next collective's words, without that put. It costs *N* words a team number
in every PE's internal region, so it would be for small node counts, the tree
beyond. That would be an item of its own, not P30.

If the author still wants the crossover A11 asked for measured with all three
built, this design and its model are what stage 2 would build from; otherwise
they are the record of why P30 waits for a system large enough to show it.

## The model

`tools/modelcheck/allgather_model.py` (every case it had keeps its verdict),
in two parts:

**The arithmetic** (`ranged_arithmetic`, no interleaving): every node count from
2 to 70, seven layouts - every share alike, every other one empty, one share
holding every byte at either end and in the middle, a spread with zeros, none -
under both orders: the leads' scan at every fan-out from 2 to 8 gives every lead
its base in team-rank order and the total; every step's ranges, the last cut at
its receiver's share, are its shares' bytes; every record continues what its
receiver holds; every lead ends holding the whole result. 964 layouts, no
failure, in about a second. Its breakages fail: the scan on the reductions' tree
(`heap_tree`, first at four nodes), the last step uncut (`no_cut`, three), and a
wrapped run sent as one range (`wrap_as_one`, four).

**The protocol** under the engine's interleavings, its own checks besides
DEST, FORWARD and P29's PLACE - now also no byte written past the result's end,
and the lead holding the whole result at return: **RANGE** - the ranges a lead
sends are its shares' bytes in this collective, every record it takes is this
collective's and continues what it holds, and the share its last-step receiver
put is that lead's own; and with the scan, **SCAN** - the base and total each
lead takes are this collective's - with scan_model.py's SLOT and VALUE for the
small slots. Each case of the design two collectives whose lengths differ, a
zero among them, so that nothing left from the collective before passes for this
one's; the breakages one, or two where the next collective is the point:

| Case | States | Verdict |
|---|---|---|
| `ranged`, 2 nodes, each order | 361 | ok, live |
| `ranged`, 3 nodes, each order | 7,342 | ok, live |
| `ranged`, 4 nodes, each order | 184,785 | ok, live |
| `ranged`, 4 nodes, a wrapped run's two ranges touching | 139,834 | ok, live |
| `ranged`, 5 nodes, in reverse | 1,997,463 | ok, live |
| `ranged_scan`, 3 nodes, each order | 7,232 | ok, live |
| `ranged_scan`, 4 nodes at fan-out 2, each order | 148,127 | ok, live |
| `range_before_data`: a record taken before its DATA | 41 | VIOLATION, RANGE: the record not yet put |
| `range_before_ready`: a record put ahead of READY | 102 | VIOLATION, RANGE: collective 1's record taken in collective 0 |
| `wrap_as_one`: a wrapped run sent as one range | 65 | VIOLATION, RANGE: bytes 3 to 4 of a 4-byte result |
| `no_cut`: the last step uncut | 30 | VIOLATION, RANGE: a share the receiver holds sent again |

The new cases take about 80 seconds, the five-node case 60 of them, and the
whole model about two and a half minutes; `selftest.py` passes.

## What stage 2 would change

- **`internode.h`**: the leads' scan, `osoe_internode_scan_node_totals`, called
  by the team's local lead on its own thread like the rest of the data half;
  and the gather by ranges, `osoe_internode_collect_ranges` (team, epoch,
  buffer, the total, this node's share's start and end), beside
  `osoe_internode_collect`, whose block and places callers stay as they are.
  `internode-am.c` hands both to the generic implementation;
  `transport-single-node.c` has no internode phase to call them.
- **`internode-generic.c`**: the scan's small loop beside `in_pieces` - its
  pre-order tree from `scan_place` over positions, its pieces through
  `small_slot` and the release row, header-checked with the new identity,
  which `describe` learns, and the bases advanced as `in_pieces` advances them;
  the gather's ranges - the share put at entry, the shape's layout of three,
  `put_step`'s ranges form with the cut, the record put before the fenced add,
  taken, checked and merged after each DATA wait, and the end's check - with
  `find_intervals` not needed for them; and `osoe_internode_generic_number_taken`
  zeroing the gather's lines whole, records and shares, not their counters
  alone, as it zeroes the grants' words.
- **`internal-region.h`**: no size or offset changes, only what the lines hold:
  the record past DATA[*s*]'s counter, the share past READY[*S* - 1]'s. Not a
  layout change but a wire change, what a lead puts into a peer's internal
  region during a gather, which nothing negotiates: every node runs one build.
- **`collectives.c`**: `collect_varying`'s third way where the runs meet the
  positions in order or in reverse - the scan in place of the reduction, never
  large, so with no `osoe_internode_reduce_enter`; the base from (before,
  total, own, the order); the places of the lead's own members only, a vector
  of its *L* members and one, not of the team's *M* and one; and `gather`
  handing the internode phase the share's range and the total.
- **`team.h`, `team.c`**: `osoe_team_node_runs` as it is, keeping one fact more
  from its walk - whether the runs meet the positions in order, in reverse, or
  neither.
- **`node.h`**: `team.tree.collect_lengths` counts one under the scan - a length
  a lead - which needs no layout change; a field naming the way, which P31 may
  want, would be layout 39, and `tools/stat/segment.h` follows either.
- **A way to force the scan**, which the tests need where P29's way would be
  chosen: P31's setting, built with P30's stage 2, or a test-only hook.
- **Tests, as P29's**: `shmem.collect-places.*` with the scan forced - block
  maps of two, three, four, five, seven (a cut inside a range received) and
  eight nodes, every byte and the count; strided splits with a negative stride;
  lengths of 0 down to a whole node's, every member 0, and one member alone; a
  cyclic map falling back to the members' vector, and a block map numbered out
  of the ranks' order (`-m 1,1,0,0,2,2`, if the launcher takes one) falling back
  to P29's way; tier 3's three links; and a mutant beside its control - the
  last step uncut, which three and seven nodes catch - in
  `docs/design/evidence/v0.2-step4/<date>-collect-scan/`.
- **A model of the scan beside the reductions**: `lane_model.py`'s small
  collectives with the leads' scan among them - its tree differs from a small
  reduction's and a rotated broadcast's on the same small rows, which the
  argument above covers and no model yet checks.
- **The notes**: design §5.8's collect's places and §4.11, `internode.h`'s and
  `internode-generic.c`'s comments, and the README's collect row and limits.

## Stage 2, as built

*The second cloud session, 2026-10-13 (`docs/prompts/cloud-session-2-prompt.md`);
its manifest, `docs/prompts/cloud-session-2-manifest.md`, has the commands and
the evidence.* Built as stage 1 designs it - the leads' scan, the gather by
ranges, the way chosen in `collect_varying`, the runs' order kept by the walk,
the count, the model beside the reductions - with five points the design left
open or the build found:

- **The order travels with the gather.** The list above gives
  `osoe_internode_collect_ranges` the share's range and the total; the
  receiver's continuity, the cut and the end's check also need whether the runs
  meet the positions in order or in reverse, which only the caller knows - from
  the walk, which keeps it now (`team.h`, `node_runs_order`) - so it is an
  argument, `reverse`. The leads' scan is
  `osoe_internode_scan_node_totals(team, epoch, own, &before, &total, routine)`.
- **The share's place is one offset**: past READY[*S* - 1]'s counter and the
  shape's room, byte 32 of the line, whatever *S* is, rather than past the counter
  where *S* > 1 and past the shape where *S* is 1; READY[0]'s line then holds
  both on two nodes.
- **A record is checked for one thing more**: besides its epoch, its ranges
  within the result and their continuing what the receiver holds, the merged
  ranges must not meet past the receiver's own share - the wrapped part below
  its base in order, above its share's end in reverse. Continuity alone checks
  where a range begins, and lets a last step uncut through to the end's check of
  the whole result: the mutant below, before this check, failed every one of its
  cases there, as "a broken invariant". With it the receiver names the writer and
  the step where the share is run into. The model's `no_cut` fails RANGE either
  way, its check comparing what a lead sends with the shares' bytes, which no
  receiver knows.
- **The way is forced by a setting, P31's**: `OSOE_COLLECTIVE_PLACES` - `link`,
  the default, as before (each node's total where every node is one run, the
  members' vector elsewhere), and `members`, `totals` and `scan` to force one -
  the scan falling back to each node's total where the runs meet the positions
  in neither order, and that to the members' vector where some node is not one
  run. Every PE must agree, since each way waits on words of its own: the node
  segment's header holds it for its PEs (`open_control`), and the job
  rendezvous's agreement for the nodes.
- **The counts**: `team.tree.collect_lengths` is 1 under the scan, its own
  node's, and a field naming the way, `team.tree.collect_way` - 1 the members'
  vector, 2 each node's total, 3 the scan - lets a test tell the ways apart
  where a count cannot.

**What changes on the wire and in a layout**, plainly:

- **The internal region's layout does not change**: no size and no offset moves.
  What its lines hold does: a record past DATA[*s*]'s counter (40 bytes), a share
  past READY[*S* - 1]'s counter and the shape's room (24 bytes), and the shape's
  layout word a third value, 2 for ranges; and the leads' scan's pieces carry a
  `collective` of their own in their headers, bit 30 set. **That is a wire
  change** - what a lead puts into a peer's internal region during a collect -
  which nothing negotiates: every node of a job runs one build. A PE taking a
  number zeroes the gather's lines whole now, records and shares with the
  counters.
- **The node segment's layout changes, to 39**: `team.tree.collect_way` in room
  the tree state had, and the header's `collective_places` in room before the
  rendezvous words, which keep their place (`ready` and `failed` at 448 and 464 on
  x86-64, as at layout 38); `tools/stat/segment.h` follows, and osoe-stat shows
  both (`node.collective_places`).
- **The rendezvous's up message changes shape**, to "osoeup-6", 88 bytes,
  carrying the setting.

**The tests**, `shmem.collect-places.scan.*` at tier 2, forced by the setting:
block maps of two, three, four, five, seven and eight nodes - seven the first
whose last step cuts inside a range a lead received - and negative strides on
four of them; a cyclic map, whose world and world backwards fall back to the
members' vector and whose first rank of each node takes the scan; and a block
map numbered out of the ranks' order (`-m 1,1,0,0,2,2`, which the launcher
takes), falling back to each node's total - each with every byte, the way and
the count, lengths of 0 down to a whole node's, every member 0 and one member
alone; and the out-of-order map at the default too. Over tier 3's three links,
the seven-node map's block and negative-stride cases and the cyclic one
(`tier3.<link>.collect-places.scan.*`), registered, and run where netem is. The
mutant beside its control, the last step uncut,
`docs/design/evidence/v0.2-step4/2026-10-13-collect-scan/`: the control passes
all nineteen of `shmem.collect-places.*`; the mutant fails the nine scan cases
whose teams span a node count that is not a power of two, each at the record
that runs on into its receiver's share. `lane_model.py`'s scan cases hold the
scan's tree beside the reductions' and a turned broadcast's on the same words,
and its bases counted by the reductions' tree fail (`scan_heap_bases`, STUCK).

## A direct exchange of the totals (P33)

*Built beside P30 by the second cloud session, 2026-10-13, from the author's 1(b)
of that day: the direct exchange "Does it pay?" names above, modelled first
(`tools/modelcheck/exchange_model.py`).*

**The way.** Where every node of a team is one run of its ranks - P29's
condition, not P30's, the runs' order not mattering - each lead puts its node's
total, with the collect's epoch, into its own position's slot at every other
lead and adds one to that lead's counter, fenced after the put; a lead whose
counter shows the other N - 1 reads its slots and holds every node's total, as
P29's reduction would leave them, and the places follow as P29's do
(`places_from_node_totals`, the gather by places). **One hop** where P29's tree
takes a level each way at eight nodes and two from ten, and **N - 1 messages
out of every lead** and as many in, where the tree's root alone takes N - 1: a
way for few nodes, the tree beyond.

**Nothing waits before writing the slots** - the reason it is one hop - so a
lead may write collect x's total while a slower lead has yet to read x - 1's.
Two sets of slots and a counter for each, by the exchange's parity, as the
staged allgather's step 0 has two slots by the collective's parity:

- **the slots**: a lead writing x's total has finished x - 1, which it finished
  only with every lead's total of x - 1, which each lead put as it entered
  x - 1, after finishing x - 2 - after reading x - 2's slots, the ones x reuses;
- **the counters**: within a parity a lead's adds for x + 2 come only after it
  finished x + 1, so after every lead entered x + 1 and finished its wait for x;
  but an add for x + 1 into a counter x also counts could arrive before a slower
  lead's for x, and x's wait would read a slot not yet written.

**The gather would order it alone, but not always.** No lead finishes a gather
before every lead has entered it - each lead's every share reaches every other
through puts it makes - so with a gather after every exchange, one set of slots
and one counter would hold. A collect whose every length is 0 has no gather,
and its exchange stands alone: the model's `one_slot` (SLOT) and `one_counter`
(VALUE) each fail there and hold with a gather after every exchange, and the
design holds both ways on two to four leads.

**Its words** (`internal-region.h`, "the exchange's words"): per team number,
the two counters' lines, then the two sets of slots, sixteen bytes a node - the
epoch and the total - rounded up to the line; sized by the job's node count to
**64 nodes at most**, `OSOE_INTERNAL_EXCHANGE_NODES_MOST`, a team over more
taking each node's total through the tree. At 64 nodes and more that is 2.25 KiB
more a team number in every PE's region - 29.6 KiB at a thousand nodes where
27.4 were - and 384 bytes at two to four nodes. Not more, since past a few dozen
nodes the tree costs less on any link the project knows: at 64 nodes P29's
tree is two levels each way at the ceiling's fan-out, and the exchange 63
messages a lead. Each slot's epoch is checked as it is read, naming the sender
of another collect's total; a reader clears what it read; a PE taking a number
zeroes its exchange's counters and slots. A lead's puts begin at the next
position on, so that no lead is every lead's first.

**What changes on the wire and in a layout**: the internal region's layout -
the exchange's words go between the scans' area and the lanes, so the lanes
move by the words' size, which every node computes alike from the job's node
count; and what a lead puts into a peer's region during a collect, a wire
change, which nothing negotiates. The node segment does not change beyond
layout 39 (P30's), `team.tree.collect_way` naming the exchange 4; nor the
rendezvous beyond "osoeup-6".

**The calls**: `osoe_internode_exchange_node_totals(team, epoch, own, totals)`
and `osoe_internode_exchange_most_nodes()` (`internode.h`); the generic
implementation's, AM's forwarding them, the single-node build's room 0.
`OSOE_COLLECTIVE_PLACES=exchange` forces it where every node is one run and
the team spans no more nodes than the room, falling back to P29's tree beyond.
**Its tests**, `shmem.collect-places.exchange.*`, as P29's and P30's: block maps
of two to seven nodes, negative strides, the map numbered out of the ranks'
order - which the exchange takes, needing no order - and the cyclic map,
falling back to the members' vector; and `zeros`, the world collecting nothing
96 times with lengths that differ every eighth, under `OSOE_TEST_PERTURB`, at
the default and under the exchange: the collects with no gather, where the two
rules above have only themselves. Two mutants beside their control,
`docs/design/evidence/v0.2-step4/2026-10-13-collect-exchange/`, three rounds
each: the control passes all 29 of `shmem.collect-places.*` every round; one set
of slots fails 7 or 8 of the exchange's cases each round - every case runs a
collect of nothing among its others - each named by a slot holding the next
collect's epoch; one counter fails the zeros case each round, each named by a
slot holding nothing, the add that let the lead read it the next collect's; and
neither fails a case the exchange does not take.

