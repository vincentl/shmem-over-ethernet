# §7.1 step 7, slice 3: a team number is each member PE's - step D of the team redesign

**Status: written on 2026-10-02 from `docs/design/team-resources-proposal.md`
(the proposal) §3, §5 items 1 and 2, §6 and §7, with the author's answers to
its §8 of the same evening (`docs/decisions/2026-10-02.md`, items 2 and 8).
It is the first of the two team briefs: step D, whose two parts are the
proposal's slices 1 (storage) and 2 (allocation), with a merge point after
them. Step E's brief is written from this one's record once D has merged.**
The author's decision behind it: a significant rewrite is accepted for the
better design, both steps precede the first tag, and after D `main` is a
verified checkpoint before E begins.

What D changes, in one sentence: a team number stops being spent from one
job-wide counter and never returned, and becomes a resource each member PE
holds and gives back - a split agrees it by one reduction over the parent,
destroy frees it - so that the limit a program meets is the live teams one
PE is in, not the teams a job has ever made. Sandia's `shmem_team_reuse_teams`
and `shmem_team_max`, which fail at 70 PEs today, pass at any size when D is
done. Nothing in D changes what a collective's bytes do; E does that.

It is written for a session that starts cold, in this repository, with the
VMs below. Nothing in it needs the conversation that wrote it.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks** - a constraint on
  the seams, not work here (design §1).
- **For the teams** (the proposal's own): no limit on teams that a program
  meets before it has many teams *on one PE*, and no memory set aside for
  teams that do not exist.

## Read first

1. `CLAUDE.md`: delegation, names (words, not contractions - *block index*,
   *holders*, *generation*, *name* are this brief's), the rename audit rule.
2. The proposal, whole, §3 above all: its decisions D1 to D8 are what to
   build, and "Why giving a number back is safe" is the argument every part
   of this brief rests on. §8 is answered; D3, E2, E9, E10 and §5 carry the
   answers.
3. The record of the day, `docs/decisions/2026-10-02.md`, items 2, 4, 6 and
   8: the decisions, the merge rule, the README rule, the answers.
4. How teams were built: `docs/prompts/done/phase1-step7-slice1-prompt.md`
   (decision 6, "numbers never reused", is what D reverses) and
   `phase1-step7-slice2-prompt.md`; their records `docs/decisions/2026-09-25.md`
   and `2026-09-26-overnight.md` items 3 and 4 (the review's fixes: a created
   team's node set built from its local leads, numbers claimed by
   compare-and-swap, fan-out and D agreed among a node's PEs).
5. Design §5.2 to §5.5 (teams, the intranode and internode phases), §5.7
   (team creation as built), §5.8 (pipelined pieces), §3.8's second argument
   (the reply pool's bound), §3.14 (what the control segment publishes),
   §3.16 (atomics: named orders only, no fences).
6. The specification: §9.4 (teams; §9.4.6 `split_strided`, §9.4.7 `split_2d`,
   §9.4.8 `destroy`), §9.10 (collectives are not concurrent on one team).
7. The code, whole where named whole: `team.c` and `team.h`; `node.h`'s
   `struct osoe_team_record` (`:597-628`), `osoe_team_block` (`:645-652`),
   `osoe_arrival_handoff` (`:378-385`), `osoe_collective_state` (`:486-510`),
   the accessors `osoe_node_team` and `osoe_node_published_epoch`
   (`:1014-1030`); `node.c`'s `create_control` (the team section, `:517-524`
   and the epoch loop at `:724-729`) and `open_control`'s `OSOE_MAX_TEAMS`
   compare (`:817-824`); `progress.c`'s `release_down` (`:254`),
   `book_arrival` (`:271`), `known_team` (`:320`), the barrier message
   handling, `release_local` (`:1143`); `progress-host.c`'s `team_of_record`
   (`:221`) and the reply pool's term (`:422-424`); `internode.h`'s Teams
   section (`:186-200`: how a backend learns a team); `internode-am.c`;
   `internode-generic.c`'s header comment and its slot addressing by number;
   `collectives.c`'s `data_collective_at` (`:128-136`) and
   `osoe_team_broadcast_word` (`:267-275`); `internal-region.h`'s spare words
   (`:120-133`); `am.h`'s `struct osoe_am_barrier` (`:145-150`); `tools/stat`
   (`stat.c:64-73`, `:488-493`, `:737`: the published records and the epochs
   it pairs with them); `tools/modelcheck/tree_barrier_model.py`.
8. Prior art, read from source on 2026-10-02 (proposal §2 item 6): Sandia's
   `shmem_team.c` picks a team's `psync` index by a band reduction of each
   PE's `psync_pool_avail`; MPICH's context ids are chosen the same way, with
   the retry rule D7 recalls.

## The state of the tree

- This brief may start while `docs/prompts/task-release-fixes.md` is being
  built. The two meet in `node.c`'s `create_control` and in `OSOE_NODE_LAYOUT`
  (its Part 3 reorders the segment's creation; this brief rebuilds the team
  section): whoever merges second rebases and takes the next layout number.
  The fixes brief leaves the team code alone and writes one temporary README
  sentence on the team limit (its Part 4), which this brief replaces.
- `main` holds, or will hold, the release fixes' parts; the team code is as
  step 7 slice 2 left it (`2026-09-26-overnight.md` items 3-4), the scans
  added by step 10 slice 1 with their slots per number.
- VMs (`limactl shell <name>`; the repository mounted at its host path,
  design §8.5): `osoe`, shared with other sessions - every run under the
  lock (`exec 9>/tmp/osoe-vm.lock; flock -w 10800 9`), killed by PID, `git`
  not installed there; `osoe-load` if it still exists (the fixes brief
  removes it at its end). Logical nodes: `osoe-launch -m` runs up to 254 of
  them on one machine, which is how the 64-node and 70-PE cases run.
- The Pi cluster was rebuilt on 2026-09-28 and holds no tree; the merge point
  needs it (below), so the tree is copied with `rsync` and built per
  `docs/pi-cluster.md` §2 - a `ssh` session to a node must stay open while
  jobs start, or its `/dev/shm` is removed ten seconds after the session
  ends (`RemoveIPC`).
- The host disk is near full; a worktree with four preset builds is several
  GiB. Remove each part's worktree once it has merged.

## How to work

- **A branch and a worktree a part** (`build/scratch/wt/<branch>`, `git
  worktree add`); never edit a worktree a verification is building from.
- **Models before code** for anything that changes a protocol's
  interleavings: Part 2's two models come first, each with the broken
  variants named below that must fail. Part 1 changes no protocol - it
  changes where a block is and how it is named - and says so in its record;
  `tree_barrier_model.py` stays valid because a message means the same team
  before and after.
- **Verification of a part**: the four presets, `tier3` (barrier messages
  now carry a name; created teams spanning nodes are tier 3's), the SOS team
  tests, `osoe-pacer teams` and a `mix` run, and every new test against the
  mutation it was written for. Long models run in the background from the
  main session, never from a subagent.
- **Merge into `main` at green checkpoints** (the author, 2026-10-02): Part 1
  when it is green, Part 2 when it is green, and then the merge point's
  verification on `main` whole. The README is true of the tree at every
  commit: each part carries its README, design-note and decisions-index
  sentences, and a sentence that E will replace says so ("temporary").
- **Subagents** (CLAUDE.md): Sonnet for the sweeps (every `osoe_node_team`
  caller; every per-number state in a PE; the stat tool's walk), for running
  presets and reporting failures, for the checklist passes. The protocol work
  - the reduction, the block's life, the engine's relearning, the models -
  stays in the main session. A subagent's report is evidence, not a verdict.
- **Names.** The proposal's words: a block's *name* is (the local id of the
  team's local lead, the number); the *block index* is its place in the
  pool; *holders*, *generation*, *free mask*, *mask token*. In code:
  `osoe_team_name`, `block_index`, `holders`, `generation`; the settings
  `OSOE_TEAM_BLOCKS` and, in E, `OSOE_SMALL_COLLECTIVE_BYTES`.

## Part 1: storage - the blocks move to a pool found by name (proposal §5 item 1)

The goal of this part is that nothing about numbering changes - numbers
still come from world PE 0's counter and are never given back - while every
place that finds a team's node block by its number finds it by its name
through the map, and the engine, the wire and the seam go by block index.
Every test passes as it stands; one that must change is a stop-and-ask.

1. **`OSOE_TEAM_BLOCKS`** (proposal D3): the blocks a node keeps for teams,
   default 1,024, from 8 to 65,536; read and validated by every PE in
   `osoe_bootstrap_read` as `OSOE_MAX_CONTEXTS` is (`bootstrap.c:268-272`),
   compared among a node's PEs in `open_control` as `OSOE_MAX_TEAMS` is
   (`node.c:817-824`), since it sizes the segment; free to differ between
   nodes. `SHMEM_INFO`'s text; a README row, *checked* among a node's PEs; a
   `shmem.job.disagree.team-blocks` test beside the others.
2. **The segment** (`node.h`, `node.c`; `OSOE_NODE_LAYOUT` bumped): the
   header's `OSOE_MAX_TEAMS` blocks by number become
   - the **pool**: `OSOE_TEAM_BLOCKS` blocks, the first three fixed for the
     predefined teams (world, shared, finalize: block index equals number,
     lead local id 0 everywhere, never destroyed), the rest claimed from a
     **free bitmap** by compare-and-swap in `team.c`'s `claim` manner - a
     failed claim takes nothing - and released by one atomic or. In this part
     nothing is ever released: a block, like a number, is spent for good, so
     the pool fills exactly as the numbers do (61 at the defaults) and Part 2
     brings the return;
   - the **map**: `_Atomic uint32_t slot_of[local_count × OSOE_MAX_TEAMS]`,
     name to block index plus one, 0 for none. The lead writes the three
     predefined entries in `create_control`; a created team's local lead
     writes its entry with release before the split's sync and members read
     it with acquire after the sync (the sync over the parent is what orders
     the lead's writes before any member's first collective on the new team:
     the argument `node.h` makes for the record, unchanged);
   - the directory fields for both, and the room check's sum.
   `osoe_node_team(n, block_index)` indexes the pool; a new accessor finds a
   block by name through the map. `osoe_node_published_epoch` stays a PE's
   row by number: a number is the PE's own index into its row, in this part
   and after.
3. **The block gains a line** for what the pool needs and Part 2 fills:
   its name, its holders count and its generation (D4), so that `osoe-stat`
   can label a block and the engine can tell a block's teams apart. In this
   part the lead writes the name, holders stay at the member count, and the
   generation is 1.
4. **The descriptor** (`team.h`, `team.c:468-470`): `t->block` is set from
   the map after the split's sync, on every member, and `t->block_index`
   beside it; the predefined teams' at init. Every caller of
   `osoe_node_team(&osoe.node, t->number)` - a Sonnet sweep lists them
   (`barrier.c:116`, `team.c:412`, the collectives, the scans, the gather)
   - goes through the descriptor's pointer or its block index, never the
   number.
5. **The seam** (`internode.h`): the `team` argument of every call in its
   Teams section and below becomes the block index, node-local, documented
   as such; the record it names is read from the block; the generic
   implementation (`internode-generic.c`) takes the team's number from the
   record where it computes the internal region's offsets, which stay per
   number per PE. No new call.
6. **The engine** (`progress.c`, `progress-host.c`, `internode-am.c`): its
   per-team arrays are sized by `OSOE_TEAM_BLOCKS` and indexed by block
   index; `known_team` and `team_of_record` learn a team from the block the
   index names; the scan of arrival handoffs, which walks the numbers below
   a high-water mark today, becomes a bitmap of posted blocks behind one
   summary word, so that a pass with nothing posted costs one load (D5);
   the reply pool's term for collective forwards (`progress-host.c:422-424`,
   degree × `OSOE_MAX_TEAMS`) becomes degree × `OSOE_TEAM_BLOCKS` - about
   240 KiB at the defaults, a bound that exists now, so the proposal's
   growth on demand is not needed; say so in the record.
7. **The wire** (`am.h:145-150`): a barrier message names the block on the
   node it is going to: the number in `team` as today and the receiving
   lead's local id in `reserved`, which becomes `lead`. The sender works it
   out from the neighbour lead's rank in the record and
   `osoe_job_local_index`; the receiver reads the map when it first meets a
   name and keeps the block index in its state. A message naming a block
   with no map entry cannot happen for a program that keeps the
   specification's rules (the split's sync precedes any collective on the
   new team); for one that does not, end the job naming the sender, the
   name and the node, as an epoch out of step is named today - never
   silently learn a team from a message alone.
8. **A dry pool** in this part ends the job by name - the setting, the node,
   the teams it holds - since a number has been claimed job-wide by then
   and nothing can make the split fail alike on every parent PE until Part 2's
   reduction. Unreachable at the defaults (61 numbers, 1,021 free blocks);
   reachable with `OSOE_TEAM_BLOCKS=8`, which is its test. Mark it temporary
   in the README's row.
9. **`osoe-stat`** walks the pool, labels a block by its name and generation,
   and pairs each local PE's published epoch with the block by name; `--gc`
   is unchanged.
10. **The README** carries this part's truth before it merges: the
    `OSOE_TEAM_BLOCKS` row, the team row's "a destroyed team keeps its
    number" unchanged (true until Part 2), the Limits sentence the fixes
    brief wrote (every split claims a job-wide number; a per-node split
    spends one per node; about 200 KiB a PE a number) still true, with
    "temporary - the next part gives numbers back" where it was already so
    marked. Design §5.7 gains the pool and the map; §3.8's second argument the
    new bound.

**Tests.** Everything as it stands; new: `shmem.job.disagree.team-blocks`;
the dry pool at `OSOE_TEAM_BLOCKS=8` ending by name; a tier 3 case with two
created teams spanning nodes whose local leads differ on one node (so the
name's lead is not local id 0), barriers and a reduction on each; the stat
tool's labels on a running job. **Mutations** that must fail: the sender
putting its own local id in the message's `lead` (the tier 3 case above);
the engine indexing by number where a block index is meant (a test with a
created team whose number is not its block index - every created team, since
numbers start at 3 and blocks are claimed from a bitmap: make the first
claim skip a block); the map entry written without release (the model
cannot see it; the sanitizer presets and a comment are what there is).

**Verification**: four presets, `tier3`, the SOS team tests (the runbook's
§8 list, in the VM), `osoe-pacer teams` and ten minutes of `mix` on the
largest map. Then merge.

## Part 2: allocation - a number held per PE and given back (proposal §5 item 2)

### Models first

1. **The block's life** (`tools/modelcheck/team_block_model.py`): a node
   with a lead and members, a destroy's sync, a member slow to leave, the
   lead taking the number again for a new team (same name, new generation)
   and the pool handing the block to another name; the engine meeting the
   block under each. Properties: no block is rewritten while a holder is
   left; the engine never applies a message of one generation to another's
   state; every member's view of the counters is its own team's. Broken
   variants that must fail: a lead that rewrites the block with a holder
   left (no holders count); an engine that keeps the old generation (no
   relearn); a map entry cleared before the last holder leaves.
2. **D7's mask token** (`tools/modelcheck/mask_token_model.py`): two PEs, two
   threads each, two splits at once under `SHMEM_THREAD_MULTIPLE`, one free
   mask a PE. Properties: a split's members all take one number; no PE holds
   two live teams under one number; no split is stuck (liveness, under the
   retry rule: the split with the lowest parent number among those active
   never yields). Broken variants: no token (two splits take one bit); the
   lowest yielding (both wait); a busy round not retried.

Both registered under the `model` label with the others; each in the record
with its state counts and times.

### What to build

3. **The free mask, per PE** (D1, D2): `ceil(OSOE_MAX_TEAMS / 64)` words in
   the process - bit *t* set when *t* is not predefined, this PE holds no
   live team numbered *t*, and no block is mapped under (this PE, *t*) on its
   node (one map read; D4). Taken with the number, given back at destroy.
4. **The split** (D2, the seven steps; `team.c:964-999` is the split as it
   is): arguments checked alike; each parent PE works out the call's teams
   it is in and each one's local shape - members on this node, the local
   lead (lowest team rank on the node, as now) - which needs no number; the
   would-be lead of a new team on its node **claims a block from the pool
   before the reduction** and, if it gets none, contributes zeros; then
   **one AND reduction over the parent**, in place, through
   `data_collective_at` as `osoe_team_broadcast_word` goes
   (`collectives.c:267-275`), of the mask words and one more word of marks
   written inverted so that a single AND serves - a bit set means "no
   complaint here": not busy (D7), a block claimed - with PEs outside the new
   teams contributing all ones; every parent PE reads the same result:
   `split_strided` takes the lowest set bit, `split_2d` the lowest for every
   row and the next for every column; no bit, and the marks say why: the
   split fails on every parent PE alike, said once by the parent's rank 0,
   naming `OSOE_MAX_TEAMS` as the teams one PE may be in at once, or
   `OSOE_TEAM_BLOCKS` and the node whose pool is dry, or - busy - retried by
   every parent PE; each member marks the number held and builds the rest
   (its descriptor; on each node's local lead, the block - name, holders,
   generation raised, the record - and the map entry, with release); the sync
   over the parent, as now; members get the handle, the others
   `SHMEM_TEAM_INVALID`. A block claimed for a split that fails is returned.
   The counter on world PE 0 and `claimed_seen` (`team.c:124`) go, with the
   `OSOE_INTERNAL_SPLIT_NUMBER` broadcast; the spare words
   (`internal-region.h:120-133`) grow to hold the mask and the marks at the
   largest `OSOE_MAX_TEAMS` (17 words at 1,024) - a change to the words area
   of the internal region only, sized at start-up as the rest is.
5. **What a number owns on a PE is reset when the PE takes it again**: a
   Sonnet sweep lists every per-number state in a process and in its share
   of the segment and the internal region - the descriptor's slot in
   `created[]`/`numbered[]`, the published epoch word, the internal region's
   counters and words for the number, the context pool's per-team parts, the
   stat labels - and each is reset at claim, with the argument of "Why giving
   a number back is safe" (proposal §3) for why nothing of the old team can
   still arrive. The list goes in the record.
6. **Destroy** (`team.c:1088-1102` as it is: contexts, then the sync over the
   members, then forget): after the sync, each member frees the number on
   itself and leaves the block - holders down by one; the one that brings it
   to zero retracts the record, raises the generation, clears the map entry
   and returns the block to the pool (D4). Order matters and the model says
   which: record retracted and generation raised before the map entry
   clears, the entry cleared before the bitmap bit is set.
7. **The engine follows the generation** (D4, D5): the arrival handoff
   (`node.h:378-385`) becomes one 64-bit word - the generation in the high
   half, the posted epoch in the low - still a single-writer publication
   store; the engine keeps the generation it learnt a team at and, when a
   handoff or a message shows another, reads the map and the record afresh
   and resets its state for the block index. The barrier message gains
   nothing: the name it carries resolves through the map to the live block,
   and a message of a dead generation cannot arrive (proposal §3, "A late
   message").
8. **Handles** (D6): a handle must name a destroyed team and never be
   dereferenced, and must not take a destroyed team for the live one under
   its number. Today a handle is a pointer into `created[]`, one descriptor
   per number (`team.c:475-483`, `:581-595`), which reuse breaks. One way
   that costs no memory per team ever made: the handle's value is the
   descriptor's address plus the generation times the array's size, so that
   `created_number` recovers both by division and `osoe_team_of` compares the
   generation with the descriptor's; the value is never dereferenced and
   never collides with the predefined handles (check their constants). If
   the implementer finds a cleaner way with the same properties, take it and
   record why.
9. **`OSOE_MAX_TEAMS` means the numbers one PE may hold at once** (D8), the
   three predefined included, still 4 to 1,024, agreed at the rendezvous and
   among a node's PEs; default 64 through this step. The README's team row:
   the limit is the teams one PE is in at once; a destroyed team gives its
   number back; a number costs every PE about 200 KiB - *temporary*, step E
   makes it about 15 KiB and the default 256. Limits: the per-node split
   spends one number, the same on every node; a whole-job `split_2d` two.
   `SHMEM_INFO`'s text likewise.
10. **The published epoch row** stays by number, with the block index beside
    the epoch in one 64-bit word, so that `osoe-stat`, which pairs a PE's
    epoch with a block (`stat.c:737`), pairs by block and not by number: two
    PEs of one node may hold different teams under one number (D1). The
    deadlock detector does not read these words.
11. **`SHMEM_THREAD_MULTIPLE`** (D7): the mask token as modelled; below that
    level one thread is in the library at a time and the token is never
    contended.
12. **The records**: design §5.7 rewritten for D (numbers per PE, the
    reduction, the pool, the block's life, the handles), §5.2's statement
    of the lead's rule, §3.8's bound, §10's open question on per-node
    numbering closed; the decisions index's team line; the dated record of
    this slice, with the models' counts, the acceptance runs and every
    choice made without the author and its undo.

**Acceptance tests** (proposal §5 item 2): Sandia's `shmem_team_reuse_teams`
and `shmem_team_max` at 70 PEs on logical nodes (`build/scratch/teams-limit/`
has the runs that fail today, with the launcher lines); `reduce_active_set`,
`broadcast_active_set`, `collect_active_set` and `repeated_barriers` at 70
PEs with no iteration skipped (count the collectives, not the exit status);
the per-node split of `SHMEM_TEAM_SHARED` on 64 logical nodes, every node
valid (the plan's P1 case, `build/scratch/review-r3/refutation/lead/`); a
whole-job `split_2d` taking two numbers; `osoe-pacer teams --teams-exhaust`.

**Tests**, beyond those: a split that fails alike on every parent PE for
each reason - no number (every PE holding `OSOE_MAX_TEAMS - 3` teams), a dry
pool (`OSOE_TEAM_BLOCKS=8`), busy under `SHMEM_THREAD_MULTIPLE` and then
succeeding on retry - naming the right setting once; a destroyed handle
named after its number is live again under a new team; a block returned and
taken under another name, with the engine following the generation (tier 3,
two created teams in sequence spanning nodes); two threads of a PE splitting
two parents at once, under tsan; the models' broken variants as mutations of
the code where a line of code is what they stand for.

**Verification**: four presets, `tier3`, the SOS team tests and the whole SOS
sweep at 4 × 2 and at 70 PEs, `osoe-pacer teams` and an hour of `mix` on the
largest map in the VM, an adversarial read of the split and the block's life
by a fresh subagent given the diff and the proposal's §3. Then merge.

## The merge point

With both parts on `main`: the four presets, `tier3` and `models` on `main`
whole; the SOS sweep; on the Pi at 4 × 4 (the tree copied and built), the
runbook's barrier and 8-byte collective lines against the records in
`docs/pi-cluster-results.md` - D changes no data path, so nothing may move
beyond noise; a difference is a stop-and-ask. Record the numbers. `main` is
then the checkpoint step E starts from, and E's brief is written from this
slice's record.

## Not in this slice

- **Step E**: the small slots, `OSOE_SMALL_COLLECTIVE_BYTES`, the lanes and
  grants, the default of 256 - its own brief after the merge point.
- The per-node colouring of design §10 beyond what D gives for nothing.
- Anything in the collectives' protocols, the internal region's layout
  beyond the spare words, the transport, any wake.

## Stop and ask

- A write into a lead's slots or counters that the lead does not wait for
  before it returns from the collective - the safety argument false for some
  collective.
- A message for a team that can arrive at a node after the last local member
  has left the team.
- The mask reduction costing a split more than the claim and the broadcast
  did, by more than a hop (time it: the split's cost in the VM at 5 and 8
  logical nodes before and after).
- A test that must change to pass in Part 1.
- A corpus or SOS program that holds more than 64 teams on one PE through D,
  or more than 1,024 on one node.
- The spare words cannot hold the mask at 1,024 numbers without moving
  something else in the internal region.
- No handle design meets D6's three properties.
- The layout bump or `create_control` colliding with the release-fixes
  brief's Part 3 in a way a rebase does not settle.
- The Pi moving on any line at the merge point.

## The record

A dated decisions file for each part, in the form of those beside it; the
decisions index's team line and settings line updated in the same commits;
`docs/prompts/README.md`'s row for this brief filled in as the parts land,
and this file moved to `done/` when the merge point is passed; the README
true at each merge, its temporary sentences marked.
