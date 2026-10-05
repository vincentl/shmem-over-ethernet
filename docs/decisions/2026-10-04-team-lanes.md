# 2026-10-04: counters per team, lanes per thread (step E, part 1)

The first part of the team redesign's step E, built on
`docs/prompts/phase1-step7-slice4-prompt.md` (the brief), Part 1, from
`docs/design/team-resources-proposal.md` (the proposal) §4, E1 to E5, E8 and E9,
with the author's answers to its §8 (`docs/decisions/2026-10-02.md`, item 8).
A team number stops owning 160 KiB of slots for reductions and broadcasts on
every PE and owns counters and a small slot a position instead; a large
collective's pieces go through the *lane* of each thread that leads it, lent by
a *grant*. The scans keep their per-number slots until the second part, and
`OSOE_MAX_TEAMS` its default of 64.

## What was built

1. **The models first.** `tools/modelcheck/lane_model.py` (in the `model`
   label, `model.lane`): the pipeline's large slots a lane per lead thread, the
   grant naming the parent's lane, a child's lane word read by its parent, small
   collectives through each team number's own slots, broadcasts without a grant.
   Properties UP, DOWN and LANE (every put lands in a lane bound, at that
   moment, to the collective that puts), with liveness. Its design cases: two
   and three teams sharing leads, a child outside the collective its parent is
   in, trees turned apart, two threads of a node leading two teams at once, a
   team's collectives led by different threads in turn, small and large
   collectives interleaved on two teams that share a lead, and a team over four
   nodes whose tree turns between collectives. Its breakages, each failing:
   `no_grant`, `stale_grant`, `lane_early` (a child reads its parent's lane
   before the grant shows), `release_lane_early` (a parent reads a child's lane
   at entry), `release_lane_late` (below), `one_lane` (a node's threads share
   one lane) and `node_small` (a node's teams share small slots). About 3 s.
   **The small class's argument** (E3) is `pipeline_model.py`'s cases at
   D = 1, which hold one piece a position: a child reaches the team's next
   collective only once it holds this one's release; the lane model's `s`
   cases add the interleaving of small and large collectives on teams that
   share a lead, which the brief asked for. **`grant_timing.py`**, beside
   `scan_timing.py`, rerun: when the leads enter together, a large
   reduction's child pays one hop less its own intranode time, never below
   zero - +1.00 hop when that time is nothing, +0.70 at 0.3 of a hop, nothing
   at a hop or more - from 4 nodes to 4,096 and from 1 piece to 64; the
   proposal's figures. A second table, which the second part's adversarial read
   asked for (it found the case for the scans, and the proposal's figures had
   every lead enter at once): a lead with children entering late grants only
   as it enters, and its children's first pieces arrive a round trip later,
   where a number's own slots had them waiting - up to two hops less its own
   intranode time, +1.72 at 0.3 of a hop and +1.02 at a hop for the root or a
   middle lead late by two hops or more at 21 nodes, less where a deeper
   subtree hides the lateness; a late leaf costs nothing.
2. **`OSOE_SMALL_COLLECTIVE_BYTES`** (E1, E2): default 1,024, 128 to 16,384, read
   and validated by every PE in `osoe_bootstrap_read`, compared among a node's
   PEs in `open_control` (the header's field, layout 31), and agreed between
   nodes at the rendezvous: `struct agreement` gains it and a word kept zero,
   so that nothing travels as padding, and `UP_MAGIC` becomes `osoeup-4`, the
   up header 56 bytes. `SHMEM_INFO`'s text; the README's row, *checked*.
   Tests: `shmem.job.disagree.small-collective-bytes`,
   `shmem.small-collective-bytes.invalid.{127,16385,x}` and
   `shmem.small-collective-bytes.local-disagree`.
3. **The class rule** (E1): `osoe_collective_class` in `internal-region.h`,
   small when a collective's bytes are at most the setting - a reduction's count
   times its width, a broadcast's bytes - called by the reduction, the
   broadcast and the entry call alike.
4. **The layout** (E2, E4; `internal-region.h`, its comment rewritten), in
   order: every team number's **small rows**, `fanout + 1` of them - a 16-byte
   lane word, then the small slot, a piece's 16-byte header and
   `OSOE_SMALL_COLLECTIVE_BYTES`, rounded up to the region's 128-byte line;
   every number's **words**, an arrival counter a child position, the release
   counter and the **grant counter**, each alone on its line, with the grant's
   24 bytes of words past its counter, then D's two split lines; the allgather's
   words as they were; the scans' area as it was; and last **one lane per thread
   record**, `fanout + 1` rows of D slots of 16 KiB, a lane's place its index
   times its size. At the defaults - fan-out 4, D = 2, 1 KiB, 64 numbers and 17
   thread records - a PE's region is 5.64 MiB where it was 12.6: a number 47.75
   KiB at 4 nodes (small rows 5,760 bytes, words 1,024, the gather's 512 and the
   scans' 41,600) and 49.75 KiB at a thousand, the 17 lanes 2.66 MiB.
5. **Small collectives** (E3): one piece through the number's small slot for the
   position and its small release slot, then the fenced add; the header and its
   check as before.
6. **Large collectives** (E4): `in_pieces` with a lane. A lead's thread lends its
   lane (`osoe_metrics_thread_index`, the thread record's index) from its entry;
   a large reduction's child waits for its grant counter to show the grant
   (cumulative, as at least, against a new grant base), takes the grant's words
   - epoch, identity, bytes, lane - and ends the job naming both leads if they
   are not its own; a child puts its {epoch, lane} lane word before its first
   add; a parent reads each child's word as it takes the child's first piece,
   checking its epoch. A large broadcast needs no grant. The loop's one watch
   covers the arrival, release and grant counters.
7. **The entry call** (E8): `osoe_internode_reduce_enter` in `internode.h`, AM
   handing it to the generic implementation, which sends the grants and
   records the epoch entered; `osoe_internode_reduce` ends the job if a large
   reduction was not entered. Called by the local lead before it waits for its
   node's members: in `data_collective_at`, and in `collect_varying`, whose
   lengths reduction - eight bytes a member - is large past 128 members at the
   default (below).
8. **`osoe-stat`**: the team's tree state gains `team.tree.collective_class` and
   `team.tree.lane` (layout 31), read through the descriptor table as the tree
   state's other fields are; `segment.h` knows layout 31.
9. **`OSOE_TEST_PERTURB`** moves to `perturb.c`, shared by the AM transport and
   two new points in `in_pieces`, after a piece goes up and after a release goes
   down.
10. **The records**: design §5.5, §5.7 and §5.8 ("Small and large collectives,
    lanes and grants"), the settings lists of §6.2; `internode.h`'s header and
    the new call's contract; the README's cost table, its teams limit, its
    divergence limit (the class rule's premise, below), its settings rows; the
    model checker's table.

## A departure from the proposal, accepted by the author

**E4 had a parent read a child's lane word "when it first sends that child a
release". The model found that unsafe, and the code reads it as the parent
takes the child's first piece.** With no more pieces than the window, a middle
lead sends its last piece up before it takes its first release; the collective
can then end at a leaf elsewhere, and in the team's next collective - a
broadcast rooted elsewhere, whose tree is turned - that leaf can be the lead's
child at the same position and write its lane word first. On four nodes at a
fan-out of 2, node 1's child at position 0 is node 3 in a reduction's tree and
node 2 in a broadcast rooted at node 1: `lane_model.py`'s `release_lane_late`,
"LANE: node 1 releases into node 3 lane 1, bound to 0". A one-piece large
reduction - 1 to 16 KiB at the defaults, which NPB IS's 4 KiB sum is - then a
broadcast from another root is enough. Read with the first piece, the word is
ordered before anything of the next collective, which cannot begin anywhere
before this lead's last piece has gone up. The cost is the same loads at a
different moment. Put to the author the same evening, with the recommendation,
and **accepted** (`2026-10-04.md` item 7): "as long as there modification passed
rigorous correctness testing, I'm good with it" - the verification below, and
`release_lane_late` failing `shmem.lanes.rotation` 10 runs of 10 as a mutation
of the code. *Undo*: none wanted - the proposal's rule is the failing variant.

## Choices made without the author

- **Scratch first, counters contiguous.** The brief lays a number out as
  `fanout + 1` lines of a counter, a lane word and a small slot. The build keeps
  every number's counters contiguous, as they were, and puts the small slots and
  lane words in rows of their own, every number's rows first in the region: a
  lead's one watch then covers only words that adds write, so no put of a
  small piece or a lane word wakes a lead to find its counters unmoved - on
  the small path, an 8-byte reduction's, that would be a wake a piece - and a
  store running off the end of the heap still lands in scratch whose header is
  checked, not in a count, the reason the region put its slots first. The grant
  line is the last counter's, its words past the counter, outside the watch.
  The same content as the brief's; *undo*: the brief's arrangement, with a
  watch of the whole block.
- **Lanes last.** The proposal puts the lanes after the numbers' blocks; the
  build puts them after the scans' area too, at the region's end, so that no
  offset depends on how many lanes a PE has. `OSOE_MAX_THREADS` is compared only
  between a node's PEs and may differ between nodes; a lane is found by an index
  that travels, which with the agreed settings alone places it.
- **The entry call takes the operation and the element** as well as the team,
  the epoch and the count (E8 says "the team, the epoch and the bytes"): the
  grant names the collective's identity, which is the operation and the element,
  so that a child in another reduction is told by the grant.
- **A lane word carries the epoch**, and the parent checks it. Nothing in the
  protocol needs it; it turns a word read at the wrong moment from a silent put
  into the wrong lane into a fatal naming both leads, which is how
  `release_lane_early` and `release_lane_late` fail on the code.
- **The grant carries the bytes** besides the epoch and identity, so a child
  given another count is told at the grant.
- **A lane per thread record, the agent's included**: 17 at the default, as E9
  counted, though the agent never leads a collective.
- **`osoe_metrics_thread_index` is the lane**: a thread holds its record for its
  life within a session, so the index `reduce_enter` binds is the one `reduce`
  uses.

## The class rule's premise (the review of D's merge point, conformance)

Every lead decides a collective's class from the count it is given, which rests
on every PE passing the same count; the specification says so outright only
for `alltoall`, `alltoalls` and `fcollect`. The README's Limits now say what a
program that gives nodes counts on either side of the line meets, and two tests
pin it: a large parent over a small child ends the job naming the child's lane
word (`shmem.lanes.class-split-named`); a small reduction's parent over a large
child leaves the child waiting for a grant until the deadlock detector ends the
job (`shmem.lanes.class-split-hang`, at `OSOE_DEADLOCK_SECONDS=1`).

## What a number owns, and what is reset when a PE takes it again

Step D's table (`2026-10-03-team-allocation.md`, item 5), against the new
layout:

| State | At take |
|---|---|
| `numbered[]`, `created[]`, `held`, `pending`, the descriptor's `published`, `label` and `block`, the published epoch word, the generic phase's state by block, the engine's tree state, the context pool | as step D's table says: nothing of them changed |
| the internal region's counters: arrivals, release and **grant**, the gather's READY and DATA, the scans' rows | zeroed by `osoe_internode_number_taken` before the split's sync - the grant counter the one guard against a grant the team before left, since a grant's epoch and identity cannot tell one team under a number from the next, whose epochs start again at 1 (`shmem.lanes.reuse`; the `grant_unzeroed` mutation fails it) |
| **the grant's words** | zeroed with the counter, so that a stale read says "nothing" |
| the small slots, the **lane words**, the spare words, the gather's shape, the split lines | none: each is read only once a counter, or the collective itself, says it was written |
| **the lanes** | none, and owned by no number: a thread's lane is a collective's from its entry to its return, and every write into it is one its owner waits for before returning (E5) |
| the generic phase's **grant base** and **epoch entered** (by block) | emptied with the other bases when the block's generation moves |

## Tests and verification

`test-lanes.c`, `shmem.lanes.*`, one PE a logical node, tsan left out as for
`shmem.internode.*`: `shared-lead` (two teams rooted at node 1, a child outside
the collective its parent is in, large reductions of one and four pieces, small
ones and broadcasts), `two-threads` (`SHMEM_THREAD_MULTIPLE`, node 1 leading T's
and U's large reductions at once on two threads, each publishing its own lane),
`turns` (a team's large reductions led by its PEs' threads in turn, each node
late now and then), `rotation` (four nodes at a fan-out of 2, a one-piece
reduction then a broadcast rooted at node 1, 400 rounds under
`OSOE_TEST_PERTURB`), `boundary` (a byte either side of the setting, by a byte
broadcast and a byte sum, at 1,024, at 128 and at D = 4: the class, the lane
and the pieces in flight each lead publishes), `reuse`, `mismatch` (the grant
names two leads' different reductions) and the two `class-split` cases. And
`shmem.collect` on twenty PEs over five nodes at `OSOE_SMALL_COLLECTIVE_BYTES`
= 128, whose lengths reduction is then large - the test the missing entry call
failed ("team 0's large reduction at epoch 60 was not entered first"), found by
reading every caller of the reduction. `shmem.pipeline.0-0-1-1.max-teams-4`
gains `OSOE_MAX_THREADS=1`, so that its region is as small as it gets and a
slot computed from piece k rather than k mod D runs past the lanes' end, which
the target refuses; its comment says so (the brief: the pipeline tests' move to
the large class). `shmem.internal-region.*`'s comment: its words are now the
last lane's end. `shmem.context-team-destroy` gains a third pass, a shareable
context left to the team's destroy (§9.4.8), the mode the pre-merge read of the
review's fixes found missing.

**Mutations**, each a model breakage that is a line of the code, run against
the lane, pipeline, team-split, reduce and broadcast tests
(`build/scratch/mut/mutations.txt`): every one fails at least one test.
`no_grant` fails 39, `stale_grant` 27, `lane_early` 33, `release_lane_early`
39, `node_small` 3, `grant_unzeroed` 5 (`shmem.lanes.reuse` among them);
`release_lane_late` only `shmem.lanes.rotation` and `one_lane` only
`shmem.lanes.two-threads`, so those two were run ten times each: 10 of 10
failed, and the unmutated tree passed both 10 of 10.

## Verification

On the tree as verified, `fb1f457` - from which the merge differs only in
comments, the design notes, the README, this record and `grant_timing.py`'s
second table - in the `osoe` VM under the lock
(`build/scratch/team-lanes-verify/run2/`):

- **The four presets**, the floor pins on: debug 712 of 712, release 712 of
  712, asan 709 of 709, tsan 474 of 474.
- **Tier 3**: 113 of 113.
- **The models**: 29 of 29, `lane` and `scan_pipeline` among them.
- **`osoe-pacer`**: its collectives a minute each at `OSOE_SMALL_COLLECTIVE_BYTES`
  of 128, 1,024 and 16,384 on three logical nodes of two PEs, so that its
  reductions and broadcasts cross the line at each; its teams and threads two
  minutes; and ten minutes of `mix` on four of two: `failures=0` on every one.
- **SOS**: its team, collective, active-set and barrier programs, 47 of them, on
  `0,1` and `0,0,1,1` with the branch's `oshcc`: every run clean but
  `shmem_team_b2b_collectives`'s two, which fail as on step D's tree - the
  program collects more than its destination holds, which the runbook names.

## Left for the second part

The scans (E6) through the lanes and the number's small rows, with
`scan_pipeline_model.py`'s lanes and grants first; `OSOE_MAX_TEAMS` 256 (E10);
the measurements on the Pi and in the VM against step D's merge point, the
2 to 16 KiB band among them; the README's temporary sentences on a number's
cost.
