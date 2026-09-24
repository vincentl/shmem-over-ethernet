# Phase 1, step 7: team creation - a proposal, two slices

**Status: a proposal for the author's review, not yet a brief to build from.**
It closes design §5.7's open item - "the team range allocator and the split
protocol are not yet designed" - and §10's entry for it. Nothing here is built.

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
