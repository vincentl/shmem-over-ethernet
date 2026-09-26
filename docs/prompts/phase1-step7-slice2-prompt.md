# Implement phase 1, step 7, second slice: team creation

You are building **team creation** for **osoe** (OpenSHMEM Over Ethernet):
`shmem_team_split_strided`, `shmem_team_split_2d` and `shmem_team_destroy`
(spec §9.4), and every team routine on the teams they create. The first slice
(merged at `3af41f5`) built the storage: every team's state by its number,
sized by the run-time `OSOE_MAX_TEAMS`, a node block per number with a record
the progress thread learns a team from, an internal-region block per number,
and a team descriptor that carries its local shape and world mapping.

*Approved by the author on 2026-09-25 (`docs/decisions/2026-09-25.md`), as
proposed in `docs/prompts/phase1-step7-slice1-prompt.md`: read its "Proposed
decisions" 6 to 10, which this brief builds out. The SOS test suite's team tests
are the acceptance set.*

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks.**

## Read first

1. `CLAUDE.md`.
2. The spec, docs/reference/OpenSHMEM-1.6.pdf, §9.4 whole: the team routines,
   `shmem_team_config_t`, what a split returns on success and on failure, who
   gets `SHMEM_TEAM_INVALID`, the valid ranges of `start`, `stride` (the spec
   says whether it may be negative - SOS has `shmem_team_negative_stride`) and
   `size`, `split_2d`'s grid and its `xrange`, and destroy's scope.
3. `docs/prompts/phase1-step7-slice1-prompt.md`: the proposal (decisions 6-10)
   and slice 1's build brief, which says what exists.
4. Design §5.2-§5.7 as the first slice left them, and §3.16.
5. The code: `team.c` and `team.h` whole; `node.h` (the record, `struct
   osoe_team_block`, `team_numbers_published`); `internode-generic.c`
   (`registered_team`, `lead_of`, `find_lead_tree`: the refusal to lift);
   `internal-region.h`; `progress.c` and `progress-host.c` (how the engine meets
   a team from its record); `barrier.c`; `collectives.c`; and the allocator's
   room that `collect` borrows for a call (`collectives.c`, `osoe_allocator_*`).

## Decisions

1. **Numbers** are claimed from one counter, never reused within a session
   (§5.7). The counter is an eight-byte word in the internal region of world
   PE 0, on a line of its own after every team's words (`internal-region.h`
   names its offset; every PE's region has the line, only PE 0's is used). A
   split's parent rank 0 adds the count of teams the call creates - one for
   `split_strided`, the row and column teams together for `split_2d` - with one
   blocking fetching add on that word (`osoe_transport_rmw`, region
   `OSOE_REGION_INTERNAL`); the first number is `OSOE_PREDEFINED_TEAMS` plus the
   value fetched. A claim that would reach `max_teams` fails the split.
2. **The protocol**, collective over the parent, in this order:
   1. every parent PE checks the arguments identically: the spec's failure
      cases return nonzero on every parent PE, its undefined ones end the job
      by name;
   2. parent rank 0 claims the numbers (1);
   3. a broadcast over the parent carries the first number or the failure:
      eight bytes of symmetric room borrowed from the heap for the call, as
      `collect` borrows its lengths' room, so that every parent PE takes the
      same place;
   4. every member builds its descriptor (3, 4);
   5. **each node's local lead of the new team writes the team's record** into
      its node's block for the number and publishes it (the first slice's
      `write_record`, generalized), raising `team_numbers_published`;
   6. a sync over the parent (`osoe_team_sync`, no quiet), so that no child
      collective starts anywhere before every member node's record is
      published;
   7. members return the handle, the others `SHMEM_TEAM_INVALID`; every
      parent PE returns the same status.
3. **The descriptor.** `members`, `me`; the world mapping - `world_start` and
   `world_stride` composed from the parent's when the parent is strided in
   world ranks, else `world_of_rank`, an explicit table, which only a split of a
   team with a table needs (a split of `SHMEM_TEAM_SHARED`, at most the node's
   PEs). The local shape by testing each local PE's parent rank for
   membership, O(local PEs): `local_ids` and `local_ranks` in **team-rank
   order**, the lead `local_ids[0]`, the lowest team rank, as §5.2 says for
   created teams.
4. **The node set, once, O(team size).** Walk the team's ranks, mapping each to
   its node, to get the sorted set of member nodes and, per node, its lead - the
   member with the lowest team rank - by world rank. The one step that is not
   logarithmic, taken at creation only. From it: `nodes`, `internode`, this
   node's record (its place in the reduction tree over the set, rooted at index
   0, each neighbour named by its lead's world rank), and - **kept only by each
   node's local lead of the team, in its descriptor** - the whole set's lead
   ranks by index. The generic internode phase needs any node's lead by index
   to rotate a broadcast's tree to its root: the world team finds them in the
   job's table of leads, and a created team in this list, O(team's nodes) on
   one PE per node. Say so in design §5.2 and §5.5.
5. **The generic backend** stops refusing teams other than the world's: a
   team's leads come from the job's table for the predefined teams and from the
   team's list (4) for created ones, through `lead_of` taking the team. The
   internal region's per-team blocks already keep two teams' pieces apart.
6. **Records and ordering.** Re-argue, and write in `node.h` at the record, why
   every reader sees a created team's record: its local lead publishes it
   before arriving at the parent's sync (2.6); a member's first arrival at the
   child team follows that sync's release, which the local lead's arrival
   precedes; the node's progress thread acquires the parent's arrival handoff
   (for a parent spanning nodes) after that arrival; and a message about the
   child team from another node can only follow the parent's sync's
   completion, which needs this node's arrival. A parent on one node has a
   child on one node, which sends no messages.
7. **Handles.** A created team's handle is its descriptor's address;
   `osoe_team_of` accepts it when `numbered[number]` is that descriptor, ends
   the job naming a handle never issued or already destroyed otherwise.
8. **`shmem_team_destroy`**: collective over the team's members: a sync over
   the team, then the descriptor and its lead list are freed and
   `numbered[number]` cleared. The number, its node blocks and records stay: a
   late reader or a late message never meets a reused number (§5.7).
   Destroying `SHMEM_TEAM_INVALID` does nothing; destroying a predefined team
   ends the job by name.
9. **The rest of §9.4 on created teams**: `my_pe`, `n_pes`, `get_config` (the
   config given at creation, `num_contexts` recorded), `translate_pe` (through
   world ranks), and `shmem_team_ptr`. `shmem_team_create_ctx` still returns
   nonzero: contexts beyond the default are not this slice's.
10. **The reply pool's forward term** counts `max_teams` now, and
    `shmem.reply-pool.overflow`'s registration changes with it (the first slice
    left it at three teams on purpose).

## Not in this slice

Contexts on teams; the deprecated active sets' partial sets (they stay whole-job
only); reclaiming team numbers; any change to the barrier's or the data
collectives' protocols beyond addressing a team's leads.

## Tests

At L1, L2, L4 and on `0,0,1,1`, `0,1,2,0,1,0` and `0,1,2,3`: the world split into
halves, into evens, into one PE, and a `split_2d` grid; a split of
`SHMEM_TEAM_SHARED`; a split of a split. On each child team, every collective -
sync, broadcast from a non-zero root, reductions, collect, fcollect, alltoall -
checked, with the arrivals table showing that no team counts another's arrivals.
**Two teams whose local leads coincide on a node, running collectives back to
back** - the case the per-team internal-region blocks exist for; the first
slice's `slots_shared` mutation must now be caught. Exhaustion
(`OSOE_MAX_TEAMS=4` leaves one number) returning nonzero on every parent PE; a
destroyed handle, and a destroyed predefined team, ending the job by name;
`osoe-stat` naming a created team (`team.3.*`); translate and `get_config` both
ways. And SOS's team tests through `build/scratch/sos/sweep.sh`:
`shmem_team_reuse_teams`, `shmem_team_translate`, `shmem_team_split_strided`,
`shmem_team_context` (whose context creation may still fail as the spec
permits) and `shmem_team_sync`, with every other SOS test still passing.

## Verification and stops

Debug and asan in the VM, foreground, under the lock; the main session runs
release, tsan, tier 3, mutations and the load loop. Stop and ask if the
ordering argument (6) needs anything but existing orders; if a split's cost
must grow faster than the team's size; or if the spec's §9.4 contradicts a
decision here.
