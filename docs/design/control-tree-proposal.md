# A control tree: the control wires of the collectives' trees alone

*A proposal, written by the lead on 2026-11-02 at the author's word of that
morning (`docs/decisions/2026-11-01.md`, answer 6): the last of the four
design changes the large-cluster playbook names
(`docs/prompts/large-cluster-playbook.md`, C17 and the "Start-up" row of its
§5) to be modelled here before any cluster session. Modelled, and costed; not
built. The author decides whether it is built, and when.*

## 1. The problem

A node's lead holds two connections to every other node, a control wire and a
data wire (design §3.1): 2(n - 1) sockets a node, n(n - 1) TCP flows a job -
998 and 249,500 at 500 nodes, 8,190 and 16.8 million at 4,096. The data wires
can be made on demand since H23 (`OSOE_MESH_DATA_WIRES`, §6.2); the control
wires are eager at every setting, made by the start-up schedule
(`mesh-schedule.c`, `mesh_timing.py`), and each has the kernel's state - its
buffers, its timers, keepalive's probes - and a place in the neighbour table.
The playbook's C17 has what no model has, the kernel's cost of 8,192
established sockets a node, and asks for `ss -tm` and the memory-pressure
counters at every rung.

Two of the control plane's protocols are stars as well. The deadlock detector
(§3.15) probes every node from node 0's lead and reads every answer there:
4(n - 1) messages a round, sent and parsed one after another at one lead - at
the cloud's 35 µs a message (C16), 287 ms a collection at 4,096 nodes, two
collections a round, where the tree the same lead uses for a barrier is four
levels deep. An abort's goodbye leaves the aborting node's lead for every
other node in turn, 143 ms until the last at 4,096 nodes before the hop, and
every node told relays it to every node again (`progress-host.c`,
`abort_wires`; the relay is what `OSOE_TEST_HOLD_GOODBYE` exercises).

What a control wire carries (`progress.c`, `control_kind`): a tree's `ARRIVE`
and `RELEASE`, the detector's messages, the request for a data wire
(`OSOE_AM_DATA_WIRE_WANTED`, from the pair's non-caller to its caller under
the schedule), and the goodbye or abortive goodbye that is every wire's last
segment. What assumes a wire to every node:

- **Every tree's edges.** The barrier and the small class run over the
  ceiling's tree of the team (`OSOE_COLLECTIVE_FANOUT`, 8), a k-ary heap over
  the team's node set in ascending node order (`collective-tree.c`); a
  derived team's edges are other node pairs than the world team's, since a
  position in the set is not a node number; and a large reduction or
  broadcast takes a fan-out from 2 to the ceiling from the link for its size
  (§5.3, as built), another tree for each k. `forward` is fatal where no
  control wire leads to a tree neighbour.
- **The detector's star**, and the request for a data wire, each fatal
  without the wire (`osoe_progress_deadlock_send`,
  `osoe_progress_ask_data_wire`).
- **The abort's rules.** The carrier's node judges itself outranked by a
  goodbye it reads itself, naming a lower caller than its own
  (`osoe_progress_outranked`); it tells a peer whose own goodbye arrived when
  that goodbye named a higher caller (`peer_outranked_here`); and it waits for
  every other node's end of stream before its own
  (`osoe_progress_peers_ended`), so that the carrier leaves last (§3.15; P1;
  `carrier_model.py`, `abort_exit_model.py`). A death - an end of stream
  without a goodbye - is a fatal at every node that held a wire to it, each
  ending its process at once with no goodbye of its own (`receive_wire`,
  `fatal.c`): the mesh is the relay.
- **The close**, which waits for every peer's goodbye on every wire; the
  silent-peer check, `TCP_INFO` on every wire of the lead's agent every
  keepalive interval; `osoe-stat`'s record a peer.

## 2. The proposal

**The control wires are the collectives' trees' edges, and the control plane
travels them.**

1. **Wires along the trees.** At start-up each lead makes the control wires
   of the world team's ceiling tree alone: to its parent and to its at most k
   children, k + 1 where it held n - 1. The rendezvous already connects every
   lead to its parent in a tree of fan-out 8 over the node indices (§6.2),
   the same tree where the ceiling is 8 - so those connections can become the
   control wires rather than close, and the schedule makes none; where the
   ceiling is another number, the schedule makes the tree's edges, a slot or
   two. Every other tree's edge - a derived team's, and a shape's for each k
   a large call takes - is made the first time a call needs it, by the
   schedule's caller of the pair (`osoe_mesh_schedule_calls`), the callee's
   listener open for the job's life as it is with on-demand data wires: both
   ends know the edge, from the team's node set or from the call's shape,
   which every node computes alike, so nobody asks and no two calls cross -
   the same rule as the data wires', with the asking left out. The call that
   needs the wire waits as a writer waits for a data wire, told after 30 s and
   every minute as a stalled wire is (§6.2). A lead's control wires are then
   the distinct neighbours across its teams' and shapes' trees: 9 at start,
   at most 42 for the world team's seven shapes, a handful more a team - never
   n - 1 unless a program's teams ask for it. The data wires go on demand by
   default past some node count, the setting's default the author's (C17's
   first change).
2. **The detector down the tree.** The root probes its children; a lead
   probed takes its sample's first half, passes the probe to its children,
   and answers its parent once its own second half and every child's answer
   are in, its `ok` the AND of them; the round number travels as today, and a
   lead drops a probe it cannot take as today, so a missing answer runs its
   parent's collection out at the root's 3 s grace, a round lost. The
   declaration goes down the tree the same way, every node printing its PEs,
   and the `DECLARED` count up it. The proof is unchanged: every node's two
   samples still bracket t*, the end of the first collection, whichever order
   the relays sample in, since a relay's own sample ends before its answer
   and the second collection begins after the root has every answer
   (`deadlock_model.py`, `tree`; §3).
3. **The abort as a notice.** A node's abort word is the lowest-ranked caller
   it knows, with that caller's status, as it is today for the first goodbye
   it reads; an abortive goodbye on a control wire is now a *notice* that may
   be followed by another naming a lower caller - the one message allowed
   after a goodbye, nothing else - where today the goodbye is the wire's last
   segment. A lead that learns a lower caller than its word's, from a PE of
   its node or a neighbour's notice, takes it and tells every neighbour whose
   last notice to it named no caller as low; that is today's "nobody to tell"
   and `peer_outranked_here` in one rule. The node's *direction* is the
   neighbour its word's notice came from, or itself where the caller is its
   own. A lead ends its process - which ends every stream it holds - once it
   has told every neighbour, its own PEs have ended, and every neighbour but
   the one in its direction has ended; the lowest caller's node, whose
   direction is itself, waits for every neighbour. So the ends gather toward
   the lowest caller's node, which leaves last, and a carrier's node finds
   itself outranked from a notice on any wire. The waits are the ones built,
   `OSOE_ABORT_LEAD_WAIT_SECONDS` at every node, running at once rather than
   nested, since every node starts its own as the notice reaches it. A lead
   told still sends its abortive goodbye on every *data* wire it holds, to
   any node, as today: a node with a data wire to the aborting one reads the
   goodbye before the end of stream that follows, which is why the goodbye
   replaced the tree's flood in the first place
   (`docs/decisions/2026-09-23-autonomous.md`, item 12) - the status is never
   lost to an end read first, since every stream's end is still preceded by
   its goodbye. A fatal's goodbyes relay as they do. A death is seen by its
   neighbours alone: a lead that reads a neighbour's end without a goodbye
   declares a fatal naming that node, as today, and - the one change - sends
   its fatal's goodbyes on its wires before its end where today it ends at
   once, so every node's message names the node that died rather than the
   neighbour that told it. The nodes beyond a dead one are cut off from the
   lowest caller's node, which sees the dead node's end as a node done and
   may leave before them; they end by their own fatals, the job a failure
   already. §3's model takes a death; §6 asks whether to accept that.
4. **The request for a data wire** travels the tree: the message names its
   destination node, and a lead that is not it passes it to the child whose
   subtree holds it or else to its parent - arithmetic on the level-order
   numbering, since a subtree of the ceiling's tree is a range of positions.
   At most 2⌈log_k n⌉ hops, once a pair a job.
5. **The close** sends and waits for goodbyes on the wires that exist; the
   silent-peer check and keepalive probe k + 1 wires a lead rather than
   n - 1, a vanished node found by its neighbours and the rest told by their
   fatals' goodbyes within the same budget plus a hop a level; `osoe-stat`
   keeps a control-wire record a wire that exists.

**What does not change.** The data wires and everything on them: one ordered
stream a pair, `fence` by construction (§3.1), the reservation protocol, the
collectives' data through the lanes and rows over whatever pairs their
algorithms use - Träff's partners, Bruck's, the scans' rank tree, `alltoall`'s
puts - made on demand as they are. The barrier's and the small class's
`ARRIVE` and `RELEASE`, which already travel the ceiling's tree. The carrier's
status rules: the first declaration on a node fixes its status, the lowest of
the nodes' winning callers carries, every other PE leaves with 0.

## 3. The model

Two halves, each the protocol that leaned on the mesh.

**The abort**, `tools/modelcheck/control_tree_abort_model.py`, registered as
`model.control_tree_abort`: every node its lead, the streams per directed edge
in order as TCP's are, an end of stream following what was queued; a chain
and a star of three nodes and a tree of four with an interior lead; one
caller, two, or three, on leaves, interior nodes and the root, the lower rank
on either side; and a death. Four properties: **LAST**, that when the lowest
caller's node has ended every node has; **STATUS**, that a node whose caller's
declaration won ends with the lowest caller as its word exactly when its own
caller is the lowest - so exactly that PE leaves with the status; **STUCK**,
no terminal state with a node not ended; and **LIVE**, every node's end
reachable from every state. Sixteen cases hold. The breakages, each failing
the property it is named for:

- `no_far_wait` - a node that is not the word's own waits for nobody, as a
  non-carrier node does today: its end cuts the tree, and the lowest caller's
  node, seeing it, leaves before the nodes beyond (LAST).
- `first_wins` - a node's word is its first notice's caller, as the word's
  single CAS has it today, so a lower caller is never relayed;
  `no_tell_back`, nothing to a neighbour whose own notice has arrived, the
  rule before P1; `wait_all`, every neighbour waited for, no direction: each
  STUCK, two nodes waiting for each other.
- `one_notice` - at most one notice a wire, as a goodbye is today (STUCK).
  The model found how narrow this is: a wire carries a second notice only
  where a node told a neighbour of one caller - its own, or an earlier
  notice's - and a lower one then arrived from a third side, which takes
  three callers and a lead with three neighbours (`tree_crossing`: node 1
  tells node 0 of its own caller, node 2's lower one crosses it, and the
  lowest reaches node 1 from its leaf after). With two callers the first
  notice on every wire is the right one. The design allows the second notice
  because the case exists, not because it is common.
- `as_built` - one notice a wire, the first word kept and no wait for the far
  side together, today's rules on a tree: the carrier's node leaves before a
  node beyond its ended neighbour (LAST).

What the model settled: the direction rule is enough for the ends - no node
needs to know the job's node count or the dead - and a node need not know the
lowest caller to end rightly, only a lower one than its own, which is why
`one_notice` is so hard to break; and that the carrier's node needs no view
beyond its neighbours, since a notice travels every path before an end does:
a node tells before it ends, and a node's end waits for every neighbour but
one.

**The detector**, `deadlock_model.py`'s `tree` design: node 1 the parent of
node 2, a probe passed on at the first half and the answer ANDed with the
child's; its eight scenarios - a plain deadlock, a put that wakes, a silent
put, a get near and far, a spell of computing, a chain, a barrier - hold with
the engine's liveness check, 180,000 states the largest, as they hold for the
star. `tree_no_wait`, the relay answering for itself alone, declares a job
whose far node still runs (SAFETY): the aggregation is the one thing the
relay must get right.

## 4. What it costs, and what it gains

`tools/modelcheck/control_tree_timing.py`, registered as
`model.control_tree_timing`, at the cloud's 40 µs hop and 35 µs a message
(C16), the fan-out 8:

| nodes | levels | control wires a lead | sockets a node | flows a job | collection, star → tree | abort heard, star → tree | ends gather | control wires made, schedule → tree |
|---|---|---|---|---|---|---|---|---|
| 8 | 1 | 7 → 9 | 14 → 18 | 56 → 14 | 570 → 640 µs | 285 → 395 µs | 80 µs | 1.0 → 0.7 ms |
| 64 | 2 | 63 → 9 | 126 → 18 | 4,032 → 126 | 4.5 → 1.3 ms | 2.2 → 0.8 ms | 160 µs | 5.1 → 2.0 ms |
| 512 | 3 | 511 → 9 | 1,022 → 18 | 261,632 → 1,022 | 35.9 → 1.9 ms | 17.9 → 1.2 ms | 240 µs | 41.7 → 16.2 ms |
| 4,096 | 4 | 4,095 → 9 | 8,190 → 18 | 16.8 million → 8,190 | 287 → 2.6 ms | 143 → 1.6 ms | 320 µs | 369 → 165 ms |

The sockets a node are the kernel's state the playbook asks about, and the
flows a switch sees. The detector's collection is the root's serial sends and
reads against k sends and a hop a level; at 4,096 nodes 112 times, and the
ratio holds at every message cost from 5 to 45 µs (`sensitivity`). An abort is
heard everywhere in 1.6 ms where the aborting lead's own sends took 143; the
ends gather in 2⌈log_k n⌉ hops more, a third of a millisecond, before the
carrier can leave - against node-local waits of tens of milliseconds for the
PEs. The star is faster below 64 nodes, by a tenth of a millisecond at 8. The
control wires' start-up is `mesh_timing.py`'s schedule, one wire a pair, the
leads entering as the rendezvous's tables reach them, against the tree's - the
last lead's entry and one connection - and at 4,097 nodes both are mostly the
rendezvous's hand-out, 0.164 s: 0.37 s against 0.165. The playbook's 0.26 to
0.45 s is the schedule's two wires.

**Code.** The wires' making an edge at a time is the on-demand data wires'
machinery (`data-wires.c`) with the asking left out and a listener that stays
open; the tree's wires at start-up are the rendezvous's connections kept, or
the schedule run over the tree's edges. The detector's relay is a few dozen
lines in `deadlock.c`; the abort's notice and the far-side wait, a hundred or
so in `progress.c` and `progress-host.c`, and a wire version bump for the
message after the goodbye; the request's routing, a few dozen. The loops over
`o->controls`, which is `peers` today, run over the wires that exist. Tests:
the global-exit and deadlock tests of the suite over three logical nodes and
more, `shmem.deadlock.waits.32-nodes` among them, and tier 3's vanished-node
tests - each already an interleaving the tree changes.

**Risks.** A job's end takes 2⌈log_k n⌉ hops more and a chain of ends that
today happen at once; a dead node's far side ends by its own fatals, the
carrier's node possibly gone (§2, 3); the first call of a derived team, or
the first large call of a new shape, waits for a wire's making - a round trip
and a connection's cost, once; a lead that is a tree neighbour of many teams
holds many wires still, which a program with hundreds of teams over sparse
node sets could make n - 1 again.

## 5. Alternatives weighed

- **Control wires on demand, any pair, as the data wires are**: the request
  for one cannot travel without one, so either both ends dial and a rule
  settles the crossing, or the request is routed over a tree - which is this
  proposal's wire-making without its trees. Every protocol stays a star: the
  detector's and the abort's O(n) serial sends remain, and the sockets a node
  follow the program's traffic rather than a bound.
- **A derived team's `ARRIVE` and `RELEASE` routed over the world tree**, no
  wires made: 2⌈log_k n⌉ hops an edge where the team's own tree has one, a
  team barrier several times slower. Left; a team's edges cost a wire each,
  once.
- **A wider control tree**, the ceiling at 16 or 32: fewer levels, more
  wires and more serial sends a level; the setting decides it, as C16's
  question does for the collectives, and the proposal follows the ceiling.
- **Leaving it**: 8,190 sockets a node at 4,096 nodes is state the kernel
  holds without complaint until it does not; the playbook's `ss -tm` reads
  and the start-up curve at the top rungs say whether it matters there, and
  the detector's 0.6 s a round at 4,096 nodes is paid only while node 0 looks
  quiet.

## 6. Questions for the author

*Answered 2026-11-03*: the author took every recommendation below as his answer (`docs/decisions/2026-11-03.md`).

1. **Build it?** *Recommended*: after the first cluster session's start-up
   curve and `ss -tm` reads at 512 nodes and above, and before any run past
   about 2,000 nodes, where the buffer budget halves and refuses at 4,097
   (§6.2); the four changes' order would then be C5, C8, C17, C9.
2. **The other trees' wires**, made at the first call that needs them
   (*recommended*: the data wires' rule, one mechanism) or at a team's
   creation and a shape's first choice, ahead of the call?
3. **The second notice** on a control wire after the goodbye (*recommended*:
   the same message kind, a wire version bump) or a separate kind for the
   outranking alone?
4. **A dead node's far side**: accept that the lowest caller's node may leave
   before the nodes beyond a dead neighbour (*recommended*: the job is a
   failure, its status 1 either way) or have the node that found the death
   wait a grace for its far side before its end?

## 7. The record

The model: `tools/modelcheck/control_tree_abort_model.py`,
`model.control_tree_abort`, and `deadlock_model.py`'s `tree` design under
`model.deadlock`; the costs: `tools/modelcheck/control_tree_timing.py`,
`model.control_tree_timing`; the decision record:
`docs/decisions/2026-11-02-control-tree-model.md`.
