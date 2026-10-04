# Team resources: a number a PE holds, counters a team keeps, buffers a PE lends

*Proposed by Claude on 2026-10-02, at the author's request, after the
round-three review (`docs/reviews/archive/phase1-release-scale-and-performance.md`,
P1) and the plan that followed it (`docs/prompts/done/phase1-release-fixes-plan.md`,
§3.3). The author decided the same day that both steps here precede the first
release's tag, and accepted a significant rewrite for the better design; the
same evening he answered §8's six questions (pool with an index for the node
blocks; the small collective's size a setting, default 1 KiB; a lane per
thread; 256 numbers; the names; D a merge point before E), and D3, E2, E9,
E10 and §5 below carry the answers. Nothing here is built; one model and one
timing simulation exist, in `build/scratch/teams-design/`, and §6 says what
each shows. The record is `docs/decisions/2026-10-02.md`.*

*Step D is built (2026-10-03): `docs/decisions/2026-10-03-team-storage.md`
and `docs/decisions/2026-10-03-team-allocation.md`, design §5.7. Three
departures, each in the second record with its undo: the team's local lead
clears its name's map entry as it leaves, not the last holder (D4); a pool
found empty while a block may be about to come back - in the split's first
round, or with a block on its way back - makes the split's round busy, not
dry; and D7's mask token, which the adversarial read of the built step found
deadlocks a valid program, is replaced: nothing is held across the first
reduction, and each member takes its agreed numbers pending between the two
reductions (`tools/modelcheck/split_numbers_model.py`).*

The author's goals, the axes to judge it by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks** - a constraint on
  the seams, not work here (design §1).
- **And for this proposal**: no limit on teams that a program meets before it
  has many teams *on one PE*, and no memory set aside for teams that do not
  exist.

## 0. What is wrong today

A team is a number (design §5.7; `team.c`). The number is **job-wide** - one
counter on world PE 0 - and **never given back**. Each number owns, on every
PE of the job, about 200 KiB of the internal region: 160 KiB of slots for
reductions and broadcasts, 40 KiB for scans, and a few lines of counters
(`internal-region.h`). `OSOE_MAX_TEAMS` is 64 by default, so a job can make 61
teams in its life.

Three things follow, each measured on `b0e0b53`
(`build/scratch/teams-limit/`, `build/scratch/review-r3/refutation/lead/`):

| A program that | meets the limit when | and then |
|---|---|---|
| makes a team of each node's PEs (one split of `SHMEM_TEAM_SHARED` per node) | the job has 62 nodes | one node gets `SHMEM_TEAM_INVALID`; the job exits 0 |
| makes teams as it runs, destroying each (Sandia's `shmem_team_reuse_teams`: two alive at a time) | it has made 62 | the split fails; at 70 PEs the test fails |
| makes one team per PE and takes `SHMEM_TEAM_INVALID` for "not a member" (Sandia's `reduce_active_set`, `repeated_barriers`) | the job has 62 PEs | the last iterations' collectives are skipped in silence |

And teams that share no PE cost as if they overlapped: a whole-job
`shmem_team_split_2d` takes 2√N numbers where each PE is in two teams, and
halving a job recursively takes P - 1 where each PE is in log P. Raising
`OSOE_MAX_TEAMS` costs every PE 200 KiB a number, which the `/dev/shm` check
counts: 500 nodes of 256 PEs would need about 25 GiB a node for it.

The applications in the reading corpus make at most one team, at start-up.
What meets the limit is a library that makes a team per call, a program that
splits in a loop, anything hierarchical at scale, and Sandia's own suite on
more than 61 PEs.

Team creation was designed this way on purpose
(`docs/prompts/done/phase1-step7-slice1-prompt.md`, decision 6): a counter is
exact while nothing is freed, and never reusing a number meant no late message
or straggling reader could meet another team under it. §3 answers that worry
directly, because the proposal reverses the decision.

## 1. The proposal in three sentences

**A number is each member PE's**, not the job's: every PE keeps the numbers
free on it, a split takes the lowest number free on every member of the new
team, and destroy gives it back (step D). **A team keeps only counters**, with
a small slot beside each for a payload that fits. **A PE keeps the large slots
and lends them to the collective it is in** (step E).

D changes whose a number is; E changes what a number costs. They touch
different code and are built one after the other.

| | Today | After D | After E |
|---|---|---|---|
| Teams a job can make in its life | 61 | no limit | no limit |
| Teams alive at once | 61 in the job | 61 on any one PE | 253 on any one PE |
| A split of `SHMEM_TEAM_SHARED` on each of n nodes | n numbers | 1 | 1 |
| A whole-job `split_2d` | 2√N numbers | 2 | 2 |
| Halving a job to single PEs | P - 1 numbers | log₂ P | log₂ P |
| The internal region, per PE, at the defaults | 12.7 MiB | 12.7 MiB | about 6.5 MiB, 2.7 of it lanes a one-thread program touches one of |
| What a number costs a PE | 200 KiB | 200 KiB | about 15 KiB at the default small collective of 1 KiB; 6 KiB at 256 bytes |
| A node's team blocks, at 256 PEs a node | 56 KiB (64 numbers) | about 1.2 MiB: a pool of 1,024 blocks and its map | the same |

Defaults: `OSOE_MAX_TEAMS` 64 through D; 256 after E. Every figure is address
space in a sparse object; a page is real once something writes it.

## 2. Read first

1. `CLAUDE.md`.
2. The spec: §9.4 (teams; §9.4.6 `split_strided`, §9.4.7 `split_2d`, §9.4.8
   `destroy`), §9.10 (collectives).
3. Design §5.2 to §5.5, §5.7, §5.8 as built; §3.8's second argument (the
   control queue's bound); §3.16.
4. `team.c` and `team.h` whole; `node.h`'s `struct osoe_team_record`,
   `osoe_team_block` and the comment over the record; `internal-region.h`
   whole; `internode-generic.c`'s header comment, `in_pieces` and
   `osoe_internode_generic_scan`; `progress.c`'s `book_arrival`,
   `release_down`, `known_team`; `internode-am.c`; `collectives.c`'s
   `data_collective_at` and `osoe_team_broadcast_word`.
5. `tools/modelcheck/pipeline_model.py`, `scan_pipeline_model.py`,
   `tree_barrier_model.py`.
6. Prior art, read from source on 2026-10-02: Sandia OpenSHMEM's
   `src/shmem_team.c` picks a team's `psync` index by an AND reduction
   (`SHM_INTERNAL_BAND`) of each PE's `psync_pool_avail` over the new team's
   PEs, takes the first bit set, clears it, and sets it again in
   `shmem_internal_team_destroy`; `SHMEM_TEAMS_MAX` is "Maximum number of teams
   per PE", at most 64. MPI libraries choose a communicator's context id the
   same way (*recalled*, MPICH's `contextid.c`).

## 3. Step D: a number is each member PE's

### What a number has to guarantee

Everything a number indexes belongs to one PE or is found through one:

- that PE's block of the internal region for the number;
- that PE's published epoch for the number, and its process's descriptors;
- the node's block for the team - its epoch counters, its arrival handoff, its
  record - which every member on the node shares.

The first two need only this: **no PE is in two live teams under one
number**. The third needs a home that two teams sharing a node and no PE
cannot collide in. Decision D3 gives it one. Nothing needs a number to be
unique in the job.

### Decisions

**D1. The invariant.** On every PE, at every moment, a number names at most
one live team that PE is a member of. Two teams with no PE in common may hold
the same number, on the same node or on different ones.

**D2. A split agrees the number by one reduction over the parent.** It
replaces the claim on world PE 0 and the broadcast of the number, and costs
about what the broadcast did. In order:

1. Every parent PE checks the arguments alike, as now.
2. Every parent PE works out which of the call's teams it is in, as now, and
   each member the team's local shape - its node's members, its local lead -
   which needs no number.
3. **The mask reduction.** Each parent PE writes into the parent's spare words
   its *free mask* if it is a member of a team the call creates, and all ones
   if it is not: bit *t* set when *t* is not predefined, this PE holds no live
   team numbered *t*, and no block is still mapped under (this PE, *t*) on its
   node (D4). A PE that would lead the new team on its node first claims a
   block from the pool (D3) and contributes zeros if there is none. An AND
   reduction over the parent, in place, through `data_collective_at` as
   `osoe_team_broadcast_word` goes today; a second reduced word carries the
   marks that say why a failure failed (no block; busy, D7).
4. Every parent PE reads the same result. `split_strided` takes its lowest set
   bit. `split_2d` takes the lowest for every row and the next for every
   column: rows share no PE with each other, nor columns, and each PE is in
   one of each. No bit left: the split fails on every parent PE alike, said
   once by the parent's rank 0, naming `OSOE_MAX_TEAMS` as the teams one PE
   may be in at once.
5. Each member marks the number held, and builds the rest: its descriptor;
   and, on each node's local lead of the new team, the node's block and
   record (D3).
6. The sync over the parent, as now: it orders every record before any
   collective on the new team (`node.h`'s argument, unchanged).
7. Members get the handle, the others `SHMEM_TEAM_INVALID`.

Destroy is what it is - the team's shareable contexts, then a sync over its
members - and then each member forgets the team, **frees the number on itself**
and leaves the node's block (D4). The counter on world PE 0 and `claimed_seen`
go.

**D3. A node's block for a team belongs to the team's local lead there, and
comes from a pool.** *(The author chose the pool over a per-PE table on
2026-10-02, for scaling; the table's cost was 14 MiB of a 256-PE node's
control segment at 64 numbers and 56 MiB at 256, and engine arrays sized the
same way.)* A team's **name** on a node is the pair (local id of its local
lead, number): the lead's rule is unchanged (lowest team rank on the node),
so one PE never leads two live teams under one number, and the name is
unique on the node for as long as the team lives there. The control segment
holds:

- **the pool**: `OSOE_TEAM_BLOCKS` `osoe_team_block`s, the first three fixed
  for the predefined teams (world, shared, finalize; their lead is local id
  0 everywhere and they are never destroyed), the rest free, with a bitmap
  of free blocks claimed by compare-and-swap exactly as team numbers are
  claimed today (`team.c`'s `claim`: a failed claim takes nothing) and
  released by one atomic or. No free list, no allocator beyond that: the
  blocks are one size;
- **the map**: `_Atomic uint32_t slot_of[local_count × OSOE_MAX_TEAMS]`, name
  to block index plus one, 0 for none. The local lead of a new team claims a
  block, fills it, and publishes the map entry with release before the
  split's sync; the last local member to leave (D4) clears the entry and
  returns the block.

Members find their block from the descriptor, which names the lead: one map
read at the split, then a pointer. The **block index** is what `internode.h`'s
`team` argument becomes and what the engine keys its per-team state by - its
arrays are the pool's size, not the name space's. A barrier message carries
the *name* of the block on the node it is going to, since a pool index is the
receiving node's own: the number in `team` as today and the receiving lead's
local id in the message's spare word (`am.h:145-150`), which the sender works
out from the neighbour lead's rank in the record and `osoe_job_local_index`.
The receiving engine reads the map once, when it first meets the name or
when the block's generation (D4) has moved, and keeps the block index in its
state from then on.

**Performance** *(the author's question)*: nothing on any hot path. A
collective's arrive and release go through a pointer the descriptor holds,
as today; the engine's per-message work is unchanged after its first map
read for a team; a split pays one bitmap claim and one map store beside the
reduction and the sync, which dwarf them; destroy one clear and one or. The
one new thing is the limit: `OSOE_TEAM_BLOCKS` bounds the teams a node holds
at once. Its default is 1,024 - 253 whole-job teams and three layers of
one-PE-per-team splits on a 256-PE node, or sixty-four such layers on a
16-PE node, under 1 MiB of blocks and 256 KiB of map at 256 PEs and 256
numbers - from 8 to 65,536, the same on every PE of a node (it sizes the
segment) and free to differ between nodes (the wire carries names). A node
whose pool is empty makes the split fail alike on every parent PE, through
the reduction: the would-be lead claims its block *before* contributing its
mask (step 3), contributes zeros if it got none, and marks a second reduced
word so that the one message names `OSOE_TEAM_BLOCKS` and the node rather
than `OSOE_MAX_TEAMS`; a claim made for a split that fails is returned.
`osoe-stat` walks the pool and shows a block by its name and generation.

**D4. A block's life.** The local lead claims it, writes it and publishes its
record and its map entry before the split's sync, as the record is now. The
block counts the local members holding it: each adds itself as it takes the
number and removes itself as it forgets the team. The one that brings the
count to zero retracts the record, raises the block's **generation**, clears
the map entry and returns the block to the pool. A lead's free mask leaves a
number out while a block is still mapped under its name, so no block is
rewritten under a member that has not left. The arrival handoff carries the
generation beside the epoch, in one word; the engine keeps the generation it
learnt the team at, and learns the team afresh - map read included - when the
word shows another. The generation lives in the block and moves with it: a
block the pool hands to a different name carries a generation the engine has
not seen under that block index.

**D5. The engine's bookkeeping follows.** Its per-team arrays are indexed by
block index. The scan of arrival handoffs, which today walks the numbers
below a high-water mark, becomes a bitmap of posted blocks behind one summary
word, so that a pass with nothing posted costs one load. The reply pool's
term for collective forwards (design §3.8, argument 2), which today is degree
× `OSOE_MAX_TEAMS`, grows by degree descriptors when the engine first learns
a team with an internode phase, since a node may now hold more such teams
than any one PE's numbers; the pool is the progress thread's own, so growing
it needs no order with anyone.

**D6. Handles tell a destroyed team from the next one under its number.** A
handle carries the number and the generation the descriptor had when it was
issued; `osoe_team_of` names a handle whose generation has passed as
destroyed, as it does today by the descriptor's address. The predefined
handles stay constants.

**D7. `SHMEM_THREAD_MULTIPLE`: two threads of one PE in two splits at once.**
Below that level one thread is in the library at a time and nothing here
applies. Under it, a PE's free mask is one resource and two reductions could
both take its lowest bit. The rule is the one MPICH uses for context ids
(*recalled*, and so to be modelled rather than trusted): a thread contributes the PE's
mask only while it holds the PE's *mask token*, which it takes if it is free
and no split with a lower parent number waits for it on this PE; otherwise it
contributes zeros and a busy mark in a second reduced word. A round that ends
busy is retried by every parent PE; a round that ends without a free bit and
not busy is the failure of D2.4. The split with the lowest parent number
among those active never yields, so some split always completes. Checked by a
model before it is written (§6). *Replaced as built (2026-10-04): the token
is held across the reduction, which waits for every parent PE, and a program
whose other collectives wait in turn on the busy split deadlocks - the status
note above, and design §5.7.*

**D8. What `OSOE_MAX_TEAMS` means.** The numbers a PE may hold at once, the
three predefined included; still 4 to 1,024, still agreed at the rendezvous
and between a node's PEs, since it lays out every PE's internal region alike.
Default 64 through this step.

### Why giving a number back is safe

The first design kept numbers for good because "a reused number meets a late
message or a straggler's read of the old team's counters, and the progress
engine would take it for the new team's". Each of the three, in turn:

- **A late message.** A barrier's messages for a team reach a node as its
  children's ARRIVE for an epoch and its parent's RELEASE for it. A node's
  members return from the destroy's sync, epoch E, only after its engine
  applied RELEASE(E) locally, which it does only after closing E - every
  child's ARRIVE(E) in - and, below the root, after RELEASE(E) arrived. No
  child sends ARRIVE(E + 1): its members are in the same destroy. So nothing
  for the team reaches that node afterwards, and its engine holds nothing
  pending for it (`release_pending` 0, no epoch open). The RELEASE it
  forwarded may still be travelling to a child - where the team is alive
  until that child's own members return, which is after the message is
  applied there. A PE offers a number as free only after it has left the old
  team, so a new team under the number cannot have, on any node, a message of
  the old one still to come.
- **A late write of a data collective.** Every write into a lead's slots and
  counters is one that lead waits for before it returns from the collective:
  a child's piece and its add, the parent's release and its add, a gather's
  READY and DATA. `internode-generic.c` argues the same within a team ("every
  node has read the last of its child slots before the collective's last
  release exists"); the destroy's sync comes after every member's return from
  the last data collective. A lead taking a number again zeroes the counters
  of its block of the internal region, with nothing left to arrive.
- **A straggler's read.** D4: the block is not rewritten until the last local
  member has left it.
- **The engine taking the new team for the old.** D4: the generation.

A program that breaks the rules - a collective on a team another member has
destroyed - meets the checks that exist (an epoch out of step, a piece's
header) and the generation, and is named.

### What changes, by file

`team.c` (the split's steps 2 to 5, destroy, the masks, handles); `node.h`
and `node.c` (the pool, its bitmap and the map in place of the per-number
blocks, the block's holders and generation, the handoff word; a layout
bump); `bootstrap.c` and `bootstrap.h` (`OSOE_TEAM_BLOCKS`); `internode-am.c`
and `progress-host.c` (the posted bitmap, the engine's arrays by block index,
the reply pool's growth); `progress.c` (the neighbours' names in a team's
tree state, used where the number is today; the map read when a name is
first met); `internode.h` (the `team` argument's meaning); `internode-generic.c`
(block index in, number for the offsets); `collectives.c` (the mask reduction
beside the word's broadcast); `tools/stat` (walking the pool; a team shown as
its name and generation); the README's Limits and reference rows (a new row);
design §5.2, §5.7, §3.8, §10.

Not touched: the collectives' protocols, the internal region's layout, the
transport, any wake.

## 4. Step E: counters per team, buffers per PE

### What a number owns today, and what it needs

Of a number's 200 KiB a PE, the counters are 640 bytes and the gather's
words a few kilobytes. The rest is slots: `fanout + 1` rows of D slots of
16 KiB for reductions and broadcasts, and the same rows of 4 KiB for scans.
They are per number so that a child may put a piece into its parent's row
before the parent has entered the collective, whatever other team's
collective the parent is in.

But a thread is in one collective at a time. The slots a lead reads during a
collective could be its own, used by one team after another, **if a child
knew when they were its team's**. That is the whole of step E.

### Decisions

**E1. Two classes of data collective, by size.** *Small*: its bytes are at
most `OSOE_SMALL_COLLECTIVE_BYTES`. *Large*: anything more. Every lead
decides alike, from the call's bytes, which is why the setting is agreed at
the rendezvous as `OSOE_COLLECTIVE_PIPELINE` is, and between a node's PEs,
since it lays out every PE's internal region.

**E2. A number's block is counters and small slots, and the small slot's size
is a setting.** *(The author, 2026-10-02: "default 1 KiB, but tuning would be
interesting.")* `OSOE_SMALL_COLLECTIVE_BYTES`, default 1,024, from 128 to
16,384 - the lane's piece, past which nothing is small: the largest
reduction, broadcast or scan that moves through the team's own slots at
once, with no grant and no lane. A small slot is a line of that many bytes
plus the 32-byte piece header, rounded up to the cache line. For reductions
and broadcasts a number's block holds `fanout + 1` lines - a child position
each, and one for the parent - each with its counter, a word for the
neighbour's lane (E4) and the small slot; and one line for grants (E4). The
scans' rows likewise. The gather's READY and DATA words as they are. At the
default, a number costs about 15 KiB a PE at a fan-out of 4 and a thousand
nodes - 3.8 MiB at 256 numbers; at 256 bytes about 6 KiB. The dial is the
operator's: lower for memory, higher to keep a 4 KiB reduction eager at a
few hundred kilobytes a PE. `osoe-pacer`'s `collectives` sweeps sizes across
the boundary, and the tests pin the class rule at the setting's smallest.

**E3. A small collective moves as it does today.** It is one piece; the piece
goes into the neighbour's small slot for the position, then the fenced add.
No grant, no lane, no change in hops. The slot is safe for the reason one
slot per position is safe at D = 1 today: a child reaches the team's next
collective only after holding this one's last release, which exists only
after every lead has read its children's slots.

**E4. A lane per thread, lent by a grant.** After the numbers' blocks, each
PE's internal region holds one *lane* per thread record: `fanout + 1` rows of
D slots of 16 KiB, which is a number's block of slots today. A lane's place
follows from its index by arithmetic every node shares.

- A lead entering a **large reduction** binds its thread's lane to it and
  sends each child of its tree a **grant**: the epoch, the collective's
  identity and the lane's index into the child's grant line for the team,
  then one fenced add to the child's grant counter. It sends them as it
  enters, before it waits for its own node's members.
- A child sends its first piece up only once its grant counter shows this
  collective's grant - counted as everything else is, cumulatively and as at
  least - and the grant's epoch and identity are its own; a difference ends
  the job naming both leads, a hop earlier than a piece's header would.
- A child writes its own lane's index beside the parent's arrival counter for
  its position before its first fenced add of the collective. The parent reads
  it when it first sends that child a release, which is after that add.
- A **large broadcast** needs no grant: its pieces go up empty already, and a
  child's first arrival carries its lane's index.
- At return the lane is free for the thread's next collective, on any team.

From there the pieces move by `in_pieces`' rules as written - the window of
D, a counter per position, cumulative counters compared as at least, a
release copied out and forwarded before it is counted, whichever input is
ready - with a slot's address taken from the neighbour's word where it is
computed from the team's number today.

**E5. Why a lane can be handed on.** The rule of §3, stated for every
collective: *every write into a lead's slots is one that lead waits for
before it returns.* So at return nothing addressed to the lane is still to
come; and the next collective's children cannot write before the lead's next
grant or, for a release, before their own next arrival. This is the rule the
model checks (§6).

**E6. The scans.** A scan's loop is the reductions' and changes the same way:
small scans in the number's small slots, large ones through each member's
lane, a member granting its tree children as it enters. A large scan's piece
becomes the lane's 16 KiB where it is 4 KiB today.

**E7. The gather does not change.** It has no slots: it writes straight into
a peer's buffer once the peer's READY says it has entered.

**E8. The seam.** One call more in `internode.h`, for the grant to leave
before the intranode phase is over: the local lead calls it as it enters a
reduction, with the team, the epoch and the bytes; it never blocks; a backend
that offloads the reduction ignores it. `osoe_internode_scan` is already every
member's call at entry.

**E9. Threads.** A lane per thread record, not a pool: two threads of a PE
may lead two large collectives at once, and a thread waiting for a lane could
close a cycle across PEs with others waiting the same way. At the default
`OSOE_MAX_THREADS` of 16 that is 17 lanes, 2.7 MiB of address space a PE, of
which a thread that never leads a large collective touches nothing. *(The
author took this, 2026-10-02.)*

**E10. `OSOE_MAX_TEAMS` defaults to 256** once a number costs 15 KiB: 3.8 MiB
a PE, with the lanes about 6.5 MiB where today's 64 numbers are 12.7. *(The
author, 2026-10-02: "256 max teams is more than sufficient.")* The README
row then reads: the teams one PE may be in at once, the three predefined
included, 4 to 1,024; a destroyed team gives its number back; a number costs
every PE about 15 KiB at the default `OSOE_SMALL_COLLECTIVE_BYTES`.

### What it costs

**One hop at most, once, on a large reduction or a large scan.** A leaf has
its node's partial result as soon as its intranode phase ends, and with the
slots its team's own it sends at once; with a lane it waits for its parent's
grant, which left the parent as the parent entered. The simulation of the
loop (§6) puts the cost at one hop minus the leaf's intranode time, never
below zero: +1.00 hop when that time is nothing, +0.70 at 0.3 of a hop,
nothing at a hop or more - the same from 4 nodes to 4,096 and from 1 piece to
64, since leads above the leaves hold their grant long before their
children's pieces arrive. Worst case: a reduction of one large piece on five
nodes or fewer, three hops where two were. Barriers, small collectives,
broadcasts and gathers pay nothing.

The small slot's size is the dial between that band and a number's cost: at
the default of 1 KiB a number costs about 15 KiB a PE and a 1 KiB reduction
stays eager; at 256 bytes, 6 KiB.

**Two more things to hold in the head**: a lane's index on the wire where an
offset was arithmetic; and the class rule, that every lead of one collective
must read its bytes the same way.

### What changes, by file

`internal-region.h` (the layout: blocks of counters and small slots, then
lanes); `internode-generic.c` (`in_pieces` and the scan's loop take a slot's
place from the class and the neighbour's lane; the grant; the lane's binding;
the bases unchanged); `internode.h`, `internode-am.c` and `collectives.c` (the
entry call); `bootstrap.c`, `bootstrap.h`, `rendezvous.c` and `node.c`
(`OSOE_SMALL_COLLECTIVE_BYTES`, agreed; the default of 256); the tests that
pin pieces in flight (`shmem.pipeline.*`, `shmem.scan.pipeline-*`), which now
pin them on the large class; the README's reference rows; design §5.5, §5.8.

Not touched: the barrier, the gather, the intranode phases, the transport.

## 5. Order of building

Each slice is modelled before it is coded, verified in the four presets and
tier 3, read adversarially, and merged before the next starts. **D is a merge
point** (the author, 2026-10-02): after its second slice `main` holds D
alone, verified whole - the four presets, tier 3, the SOS suite, the Pi
runbook's collective lines - before E's first slice begins. The README is
true at each merge (the author's rule of the same day), with the sentences D
leaves temporary marked so.

1. **D, storage.** The blocks move to the pool, found through the map by
   name; the engine, the wire and the seam go by block index and name; the
   posted bitmap; the reply pool's growth; `OSOE_TEAM_BLOCKS`. Numbers still
   come from world PE 0's counter and are never reused: every test passes as
   it stands. (This is slice 1 of step 7 again, one level down.)
2. **D, allocation.** The mask reduction, destroy's release, the block's
   holders and generation, the handles, D7's rule. Sandia's
   `shmem_team_reuse_teams` and `shmem_team_max` at 70 PEs are the acceptance
   tests, with the per-node split on 64 logical nodes and a `split_2d` that
   takes two numbers. Then the merge point.
3. **E, reductions and broadcasts.** The layout, the two classes,
   `OSOE_SMALL_COLLECTIVE_BYTES`, lanes, grants, the entry call.
4. **E, scans; the default of 256; the measurements** (§6), and the README
   and design notes for both steps.

About two weeks a step. The briefs: `phase1-step7-slice3-prompt.md` for D
(slices 1 and 2 above, its own two parts), written 2026-10-02; E's after D
has merged, from D's record.

## 6. Models, tests, measurements

**What exists now** (`build/scratch/teams-design/`; slice 3 makes them the
tree's, beside `pipeline_model.py` and `scan_timing.py`):

- `lane_model.py`: `pipeline_model.py` with the slots a node's and the grant
  added, over two and three teams that share leads. Eleven design cases pass
  with liveness - a node the parent in two teams whose children share a row;
  a parent in one and a child in the other; trees rotated apart; three teams
  round a ring with three pieces in flight. `no_grant` fails where a child is
  outside the collective its parent is in ("node 1 read 102 in its row 0,
  waiting for piece 200") and - which the model found, not the author of it -
  passes where every node is in both teams in one order, which is consecutive
  collectives on one team again: the grant matters exactly where a child's
  team is not the one its parent is in. `stale_grant`, a child taking an
  earlier grant from the team for this collective's, fails.
- `grant_timing.py`: the loop's timing with and without the grant, the figures
  of §4.

**To write before the code they cover:**

- for D, slice 2: a model of the block's life - a destroy's sync, a member
  slow to leave, the lead taking the number again, the engine meeting the
  new team - whose breakages are a lead that rewrites the block with a holder
  left and an engine that keeps the old generation; and D7's token rule with
  two PEs of two threads and two splits, checked for one number per team, no
  number shared by two teams on a PE, and no split stuck;
- for E: `lane_model.py` in the tree; `scan_pipeline_model.py` with lanes and
  grants; the small slot as `pipeline_model.py`'s D = 1 cases, which exist.

**Tests**, beyond those named in §5: two teams whose leads coincide on a node
running large collectives back to back and, under `SHMEM_THREAD_MULTIPLE`, at
once; a child outside its parent's current team, held by a parent that enters
late (the model's failing case, on the code); the class boundary, one byte
either side, at the setting's smallest and at its default; a destroyed handle
named after its number is in use again; a node's pool run dry
(`OSOE_TEAM_BLOCKS=8`), the split failing alike on every parent PE and naming
the setting and the node, and a block returned and taken again under another
name with the engine following its generation; `osoe-stat` showing a pool's
blocks by name; the mutations the models' breakages name.

**Measurements** (the rule of 2026-09-26: a speed claim is simulated and
timed before it merges). On the Pi at 4 × 4 and in the VM on 5 and 8 logical
nodes, before and after E: reductions, broadcasts and scans of 8 bytes, 1 KiB,
64 KiB and 1 MiB; the barrier; `osoe-pacer --quick` and an hour of `mix`.
Expected: nothing moves but the large reductions and scans, by at most a hop.

## 7. Stop and ask

- A write into a lead's slots that the lead does not wait for before it
  returns - E5's rule false for some collective.
- A message for a team that can arrive after the last local member has left
  it.
- The mask reduction costing a split more than the claim and broadcast did,
  by more than a hop.
- A test that must change to pass in slice 1.
- The Pi showing more than the one hop on any line, or anything on a line
  that should not move.
- A program in the corpus or in Sandia's suite that holds more than 64 teams
  on one PE through D, or more than 1,024 teams on one node.

## 8. For the author

*Answered by the author on 2026-10-02 (`docs/decisions/2026-10-02.md`, item
8); the answers are written into the decisions above.*

1. **D3**: a pool with an index, "smart for scaling". Asked whether it has
   performance concerns: none on a hot path (D3 says where the costs are);
   the one new thing is the limit, `OSOE_TEAM_BLOCKS`, default 1,024.
2. **E2**: a setting, not baked in - `OSOE_SMALL_COLLECTIVE_BYTES`, default
   1 KiB, "tuning would be interesting".
3. **E9**: the recommendation, a lane per thread record.
4. **E10 and D8**: 256 "is more than sufficient"; the README sentence is in
   E10.
5. **The names**: all fine.
6. **D before E**: yes - D is a merge point, then E.
