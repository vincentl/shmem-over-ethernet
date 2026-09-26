# Phase 1, step 7: team creation - a proposal, two slices

**Status: approved by the author on 2026-09-25** ("approve 1, 2 (c) and 3 -
go"; `docs/decisions/2026-09-25.md`), as proposed. It closes design §5.7's open
item - "the team range allocator and the split protocol are not yet designed" -
and §10's entry for it. Slice 1's build brief is at the end.

*Written by Claude on 2026-09-24, after the SOS test suite (`corpus/tests-sos`)
first ran against osoe: 127 of its programs pass on two maps, and the largest
group still failing is team creation - `shmem_team_reuse_teams`,
`shmem_team_translate`, and the spec examples `shmem_team_split_strided`,
`shmem_team_context` and `shmem_team_sync` on four PEs. No corpus program
outside SOS's own tests calls `shmem_team_split_*` (surveyed the same day), so
this is conformance, not the Pi cluster's day. It is also the prerequisite of
the partial active sets that decision 1 of that day refuses by name.*

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs (4,096 nodes × 256) that no interface blocks.**

## Read first

1. `CLAUDE.md`.
2. The spec, §9.4 (teams: §9.4.4 `shmem_team_split_strided`, §9.4.5
   `split_2d`, §9.4.8 `destroy`, §9.4.2-3 translate and config).
3. Design §5.2 (team accounting: local member count, node participation, local
   lead), §5.3-§5.5 (as built), §5.7 (team resources, the leak), §3.8 (the
   control queue's bound), §7.3, invariant 32.
4. `team.c`/`team.h`, `barrier.c`, `collectives.c`, `internode-generic.c`'s
   header comment, `progress.c`'s `book_arrival` and `release_local`, `node.h`'s
   `struct osoe_node_header`.

## What exists, and what assumes the teams are fixed

The predefined teams are built once by `osoe_team_start` before the agent
thread starts. An inventory of 2026-09-24 found **three shapes of per-team
state**:

- **Named fields**: the node header's `world_arrived`, `shared_released`,
  `finalize_arrived` and so on; the private `world_epoch`/`shared_epoch`/
  `finalize_epoch` and data epochs in `struct osoe_state`; the published epochs
  in each PE's slot; and the metrics catalogue, which names each by string
  (`team.world.arrived`), as `osoe-stat --summary` does.
- **Arrays of the compile-time `OSOE_TEAMS` (3)**: the arrival handoff and the
  published tree state in the node header; the progress engine's per-team tree
  bookkeeping, the generic backend's `teams[]`, and the agent's `consumed[]`
  and `registered[]`, all in the node lead's process; and the internal region's
  per-team arrival and release words.
- **`max_teams`**, which §5.7 makes a runtime value bounding the teams and the
  control queue, exists in the design notes only.

And four assumptions that hold only for the predefined teams:

1. **The local lead is local id 0, and the local members are local ids 0 to
   `local_members` - 1** (`collectives.c`'s `data_collective`, `gather`,
   `member_copy`, `local_member_rank`). True of teams every local PE belongs to;
   false for almost any split.
2. **The internal region's data slots are not per team** (`internode-generic.c`,
   `child_slot`: "a split team (§7.3) will need its own"). A child puts its
   piece into its parent's slot without waiting for the parent to enter - safe
   within a team, because a node cannot start the next collective before this
   one's release, and unsafe across two teams whose local leads coincide.
3. **`numbered[]` is written before the agent thread starts and read by it
   without synchronization** (`team.c`). A team created later has no
   publication.
4. **The generic backend refuses every team but the world's**
   (`registered_team`, by name) - which is what masks 2 today.

## Proposed decisions

1. **Two slices: storage, then creation.** The first changes no behaviour and is
   verified by the suite as it stands; the second adds the routines.
2. **`OSOE_MAX_TEAMS`**, read at the first init (§6.4), default **32**, between
   4 and 1,024: every team in existence job-wide at once, the predefined ones
   included (§5.7). Numbers are job-wide, so **the rendezvous agrees it** beside
   `OSOE_COLLECTIVE_FANOUT`, and a node that disagrees ends the job by name.
   *(SOS's reuse test creates one team per PE, two alive at once, so it runs to
   29 PEs under the default. 32 costs, per team: about half a kilobyte of each
   node's control segment, four bytes of each PE's slot, and the internal
   region's slots of decision 4 - sparse, touched only by that team's local
   leads.)*
3. **Every per-team structure becomes an array indexed by team number, sized by
   `OSOE_MAX_TEAMS`** - one block per team in the node header (its three epoch
   counters, its arrival handoff, its published tree state, and decision 6's
   record), one published epoch per team in each PE's slot, and the node lead's
   arrays allocated at init. The metrics catalogue generates `team.<n>.*` for
   every number in use, and `osoe-stat` walks those. A layout bump.
4. **Each team number gets its own internal-region block**: its `fanout + 1`
   slots and its two words. That makes two teams' collectives disjoint in memory
   by construction, whatever their trees share; the region grows to
   `OSOE_MAX_TEAMS × ((fanout + 1) × 16 KiB + 128 B)`, 2.5 MiB a PE at the
   defaults, which the capacity check counts and nothing touches until a
   team's lead does.
5. **The team descriptor carries what the collectives now assume**: the local
   members' local ids in team-rank order (`local[0]` is the local lead, §5.2's
   "lowest team rank among local members"), their team ranks, and the world
   mapping - a start and stride for a team split, however deeply, from the
   world's strided ranks, and an explicit table for one split from
   `SHMEM_TEAM_SHARED`, which is at most the node's PEs. `collectives.c` and
   `barrier.c` read these; nothing tests a team's number to find its shape.
6. **Numbers are never reused within a session** (§5.7's leak, kept for the
   reason the inventory found: a reused number meets a late message or a
   straggler's read of the old team's counters, and the progress engine would
   take it for the new team's). **One word on world PE 0's internal page counts
   the numbers issued.** A split's parent rank 0 claims every number the call
   creates with one fetching add - one for `split_strided`, the row and column
   teams together for `split_2d` - and a claim past `OSOE_MAX_TEAMS` makes the
   split fail on every parent PE, with one diagnostic naming the variable.
   *(SOS reduces a mask of free slots over the parent; with nothing ever freed,
   a counter is exact and costs one remote AMO per split.)*
7. **The split protocol**, collective over the parent:
   1. every parent PE checks the arguments identically - the spec's failure
      cases return nonzero on all, its undefined ones end the job by name;
   2. the parent's rank 0 claims the numbers (6);
   3. a broadcast over the parent carries the first number, or the failure;
   4. each member builds its descriptor: its local list in O(local PEs), and the
      node list in O(team size) - the one step that is not logarithmic, taken
      once at creation, whose result the tree keeps O(*k*) of (§5.2 as built);
   5. **each node's local lead of the new team writes the team's record into
      its node's block** - its node's place in the team's tree, which §5.2
      keeps O(*k*) of, and its local member count - and publishes it with a
      release store. The node's progress
      thread, which runs in the node lead's process and need not belong to the
      team, **reads the record from shared memory, never a private
      descriptor**: the release upcall resolves a team by number to its node
      block, since that process may hold no descriptor for it. The node lead
      is not told; it finds the record when the team's first arrival does;
   6. a sync over the parent, so that no child collective starts on a node
      whose record is not yet published;
   7. members receive the handle, others `SHMEM_TEAM_INVALID`.
8. **Destroy is collective over the team's members** (§5.7): a sync over the
   team, then the descriptor is freed. The number, the records and the internal
   block stay, since nothing reuses them. Destroying `SHMEM_TEAM_INVALID` is a
   no-op; a predefined team, or a handle already destroyed, ends the job by
   name.
9. **`shmem_team_create_ctx` still returns nonzero**: contexts beyond the
   default are §7.3's other half, and §9.5.1 permits it.
10. **Active sets stay the whole job** (the day's decision 1). A partial active
    set needs a team created without a parent collective, which is a slice
    over this one.

**No new model is proposed**: per-team state is disjoint by number, and the
only ordering the protocol adds - a record published before its team's first
message - is decision 7's parent sync. A reviewer who finds an interleaving
that needs one should say so.

## Slice 1: storage

Decisions 2 to 5, with the world, shared and finalize teams as the only teams.
Every existing test passes unchanged, the SOS sweep's 127 with it; new tests
pin the descriptor's local list against local ids on the maps where the two
differ (`0,1,2,0,1,0`, and a SHARED team under force-remote), and a death test
for a rendezvous disagreement on `OSOE_MAX_TEAMS`.

## Slice 2: creation

Decisions 6 to 9: `shmem_team_split_strided`, `shmem_team_split_2d`,
`shmem_team_destroy`, and `translate_pe`, `my_pe`, `n_pes`, `get_config` and
`shmem_team_ptr` on created teams. The generic backend's refusal (assumption 4)
is lifted here.

Tests, at L1, L2, L4 and on `0,0,1,1`, `0,1,2,0,1,0` and `0,1,2,3`: the world
split into halves, evens, one PE, and a `split_2d` grid; every collective on each
child, with the arrivals table checking that no team counts another's
arrivals; **two teams sharing a node and a local lead running collectives back
to back**, the case decision 4 exists for; exhaustion returning nonzero on
every parent PE; a destroyed handle ending the job by name; and `osoe-stat`
naming a created team. SOS's team tests, through
`build/scratch/sos/sweep.sh`, are the acceptance set.

## Stop and ask

- If publishing the record to the progress thread cannot be made race-free by
  a release store and an acquire load - if the node lead's process must learn
  of a team before its first message for some reason the inventory missed.
- If the internal region's growth at the default trips the capacity check on a
  configuration in `docs/pi-cluster.md`.
- If the metrics schema change breaks a consumer beyond `osoe-stat`.

## Build brief: slice 1, storage (approved 2026-09-25)

Behaviour does not change: the world, shared and finalize teams stay the only
teams, and every existing test passes as it stands. What changes is where each
team's state lives and how it is found, so that slice 2 adds teams without
touching any of it again. Work in `build/scratch/wt/teams`, branch
`phase1-step7-slice1`.

### Decisions, precisely

1. **`OSOE_MAX_TEAMS`.** Read at the first init beside `OSOE_COLLECTIVE_FANOUT`
   (`bootstrap.c`), into `osoe.bootstrap.max_teams`; default 32; a value outside
   4 to 1,024, or not a number, ends the job naming the variable, as
   `OSOE_MAX_THREADS` does (`metrics.c`). Carried in the rendezvous hello beside
   `collective_fanout` (`rendezvous.c`), and a node that disagrees ends the job
   naming both values, as the fan-out's check does. Listed in `init.c`'s help
   text beside `OSOE_MAX_THREADS`.
2. **Numbers.** `OSOE_TEAM_WORLD` 0, `OSOE_TEAM_SHARED` 1, `OSOE_TEAM_FINALIZE`
   2, as now; slice 2's teams take 3 to `max_teams - 1`. `OSOE_TEAMS` becomes
   `OSOE_PREDEFINED_TEAMS` wherever it means "the three predefined", and every
   array it sized is sized by `max_teams` instead.
3. **A node block per team number.** One struct, `struct osoe_team_block`,
   cache-line aligned, holds what the node header now keeps per team in two
   shapes: the three epoch counters (`arrived`, `released`, `copied`), the
   arrival handoff, the published tree state, and a **record** - a cache line
   with a `published` word (0 until written, then 1, stored with release) and
   this node's place in the team's tree (parent node's lead rank, this node's
   position among its parent's children, child count, the O(k) children's lead
   ranks the tree keeps) and the team's local member count on this node. The
   control segment holds `max_teams` blocks after what it holds now; the
   header's named `world_*`/`shared_*`/`finalize_*` fields and its
   `arrival[OSOE_TEAMS]` and `collective[OSOE_TEAMS]` arrays go. Layout
   version 17 becomes 18.
4. **Published epochs.** Each local PE's per-team published epoch - today
   `world_epoch`, `shared_epoch`, `finalize_epoch` in its PE slot - becomes one
   `_Atomic uint32_t` per team number in an array of `local_pes x max_teams` in
   the control segment, indexed `[local_id][team]`.
5. **The private epochs move into the descriptor.** `struct osoe_team` holds its
   own `epoch` and `data_epoch` (k and d) as values; `struct osoe_state`'s named
   copies go.
6. **The descriptor carries its local shape.** `local_ids` - this node's members'
   local ids in team-rank order; `local_ranks` - their team ranks, same order;
   `lead_local_id = local_ids[0]`, the member with the lowest team rank (design
   §5.2). And the world mapping: `world_start` and `world_stride` for a team
   strided in world ranks, or `world_of_rank`, an explicit table, for one that
   is not (a split of `SHMEM_TEAM_SHARED` in slice 2; at most the node's PEs).
   `osoe_team_to_world` and `world_to_team` read these; nothing tests a team's
   identity (`== &shared_team`, `t->number == OSOE_TEAM_SHARED`) to find its
   shape. `collectives.c`'s `local_id == 0`, `heap_of[m]` and
   `local_member_rank` read the descriptor: the lead is `lead_local_id`, member
   `m`'s heap is `heap_of[local_ids[m]]`, its team rank `local_ranks[m]`.
   `barrier.c` likewise.
7. **The progress thread learns a team from its record.** The node lead's
   per-team bookkeeping (`progress.h`'s `team[OSOE_TEAMS]`,
   `internode-generic.c`'s `teams[OSOE_TEAMS]`, `internode-am.c`'s `consumed` and
   `registered`) becomes arrays of `max_teams` allocated at init, and a team's
   entry is filled from its node block's record the first time the team is
   used - an acquire load of `published`, then the record's fields - rather than
   by a call from `osoe_team_start`. The predefined teams' records are written
   by `osoe_team_start` as now, before the agent starts. **The release upcall
   resolves a team by number to its node block**, never to a private
   descriptor: in slice 2 the node lead's process may belong to none of a
   team's members. A message naming a number whose record is unpublished ends
   the job by name, as today's unregistered team does.
8. **`numbered[]`** becomes `max_teams` `_Atomic(struct osoe_team *)`, stored with
   release and loaded with acquire, for whatever still resolves a number to a
   descriptor in this process.
9. **The internal region: a block per team number.** Today `fanout + 1` slots of
   16 KiB, shared by every team, then a 4 KiB page of per-team words. After: a
   words area of `max_teams x 128 B` (each team's arrival word and release word,
   a cache line each), rounded up to the page, then one slot block per team
   number of `(fanout + 1) x slots_per_child` slots, `slots_per_child` a constant
   1 for now (the pipelining slice raises it). `child_slot`, `release_slot`,
   `arrivals_word` and `release_word` take the team. The generic backend keeps
   refusing every team but the world's (slice 2 lifts it). The capacity check
   counts the larger region, and `internal-region.h`'s size function takes
   `max_teams`. At 32 teams and a fan-out of 4 the region grows to about 2.5 MiB
   a PE, untouched but by a team's local leads.
10. **Metrics and `osoe-stat`.** The predefined teams keep their names
    (`team.world.*`, `team.shared.*`, `team.finalize.*`); numbers from 3 would be
    `team.<n>.*`, generated for every block whose record is published. `osoe-stat
    --summary` walks the published records rather than two names.

### Not in this slice

Creating, destroying or translating teams; the counter on world PE 0; lifting
the generic backend's refusal; any change to a protocol, a wake, or a memory
order. If a change seems to need one, stop and say so.

### Tests

Every existing test passes unchanged in all four presets. New:

- a rendezvous disagreement on `OSOE_MAX_TEAMS` (one rank's environment
  differs, `-e`, on a map of two nodes) ends the job naming the variable and
  both values;
- an `OSOE_MAX_TEAMS` of 3, of 1,025, and of `x` each end the job by name;
- the descriptor's local shape on `0,1,2,0,1,0`, where local ids and world
  ranks interleave: each PE checks, through a test-only entry the library
  already has the pattern for or a new one, that `local_ids`, `local_ranks`
  and `lead_local_id` for the world team match what the map implies;
- `osoe-stat` on a live job still names the world and finalize trees.

### Verification and stops

Build and run the debug and asan presets in the VM, foreground, under the VM
lock (`exec 9>/tmp/osoe-vm.lock; flock -w 540 9`), each whole; the main session
runs release, tsan, tier 3, mutations and the load loop. Stop and ask if the
record cannot be read by the progress thread without a new ordering argument,
if the control segment's growth at the default trips the capacity check on
any map the suite runs, or if a test must change to pass.
