# Implement phase 1, step 5, sixth slice: an allgather for fcollect

You are replacing the internode phase of **fcollect** (and so of collect, which
gathers through it) in **osoe** (OpenSHMEM Over Ethernet). Today the generic
implementation of `osoe_internode_collect` is the reduction's tree running an or
over the whole result: every level carries the whole result, so its latency
grows with the result's size times the tree's depth - on the Pi cluster, 773 ms
for 16 PEs gathering 1 MiB each. This slice gives the seam's gather an
implementation of its own: Bruck's allgather among the team's node leads,
ceil(log2 N) steps, each node's blocks crossing the network once.

*Approved by the author on 2026-09-25 (`docs/decisions/2026-09-25.md`, item 2,
after pipelining). The algorithm and its two counters are decided and
model-checked: design §5.8, "An allgather for fcollect", and
`tools/modelcheck/allgather_model.py`.*

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks**: `internode.h` already
  names this shape - "an allgather over RDMA writes ... into each peer's buffer
  once that peer has entered" - as what an RDMA backend would do.

## Read first

1. `CLAUDE.md`.
2. `internode.h`'s contract for `osoe_internode_collect` whole: the buffer is the
   lead's `dest`, symmetric, at the same place on every node; a peer's buffer
   is written only once that peer has entered the collective.
3. Design §5.5 (as built: fcollect and collect, `places`), §5.8 ("An allgather
   for fcollect").
4. `tools/modelcheck/allgather_model.py`: the lead's program is the loop to
   build; its breakages are the mistakes to avoid.
5. `internode-generic.c` as the pipelining slice left it - its loop takes
   whichever input is ready and waits for several words at once with
   `osoe_region_wait_condition` (`region-wait.h`), which a step's waits may
   use too - and as team creation left it: `registered_team`, `lead_of(team,
   index)`, `find_lead_tree`, the upcalls `osoe_internode_team_lead` and
   `osoe_internode_team_local_ranks` (`internode.h`, `team.c`);
   `internal-region.h`, `collectives.c` (`gather`, `collect_varying`), and how
   the rank map gives a rank's node (`rank-map.h`, `job.h`).

## Decisions

1. **Bruck's steps.** At step s, lead i sends every share it holds - its own
   node's and those gathered so far, nodes i to i + c - 1 by index in the
   team's node set, c = min(2^s, N - 2^s) - to the lead at index i - 2^s (mod N),
   writing each share's blocks straight to their places in that lead's buffer.
   ceil(log2 N) steps, any N.
2. **A node's share** is its members' blocks, at their places (`places`, or r x
   `block`). Build, once per call, the byte ranges each set of shares covers:
   runs of consecutive team ranks on the same set of nodes are contiguous in
   the result, so a block distribution sends one or two ranges a step (two when
   the set wraps past index N - 1) and a cyclic one at most L, the node's PE
   count. Puts carry `OSOE_TRANSPORT_MORE` but for a step's last.
3. **Two counters per step and team** in the team's internal-region words, each
   on its own `OSOE_INTERNAL_LINE_BYTES` line, cumulative over the team's life
   and waited on as at least:
   - **READY[s]**, at a writer: at entry, each lead adds one at the lead that
     will write to it at step s, i + 2^s; the writer waits for it before its
     step-s puts. This is `internode.h`'s rule, and it is why nothing is staged.
   - **DATA[s]**, at a receiver: the writer adds one after its puts, fenced;
     the receiver waits for it before forwarding at step s + 1, and before
     returning after the last step.
   Each collective adds one to each counter, so collective x waits for x + 1 -
   the lead keeps the count per team.
   ceil(log2 max_nodes) steps' counters per team: size the words by the job's
   node count, which every node agrees on.
4. **Which teams take it**: every team the generic implementation serves -
   the predefined teams and the teams a split creates. This slice is built on
   step 7's second slice (team creation), which merges before it: a team's
   node set and its leads by index are the ones the reduction's tree uses -
   this node's position in the record, and any node's lead through
   `osoe_internode_team_lead` (the job's table for the predefined teams, the
   team's lead list for created ones). A created team's members on another
   node are found from the team's mapping and the rank map, as the split
   finds them.
5. **The or-tree gather goes**, and with it `zero_others` and the gather's
   identity bit in the piece header; `collect`'s lengths, which it gathers
   through the same seam, take the new path too.

## Not in this slice

A ring or recursive-doubling variant; pipelining within a step (a step's puts
are already one batch); any change to the intranode phase or to `internode.h`'s
contract.

## Tests

Every existing test passes unchanged in all four presets. New: fcollect and
collect across two to five logical nodes - 2, 3, 4, 5 nodes, so that N is and
is not a power of two - with block and cyclic distributions (`0,0,1,1,2,2` and
`0,1,2,0,1,2`), blocks of 1 byte, 8 bytes and 64 KiB, checked byte by byte;
the same on created teams - the world's evens, a team whose members are in
reverse order, and one spanning three of five nodes -
back-to-back fcollects into the same `dest` with members copying out between, the
case READY exists for; a counter check that a gather over N nodes made
ceil(log2 N) steps and moved (N - 1) shares into each lead, from the team's
published tree state, so the algorithm is pinned by counters and not by time.

## Verification

Debug and asan in the VM, foreground, under the lock; the main session runs
release, tsan, tier 3's collective tests, mutations - the model's three
breakages (a write before READY, a forward before DATA, floor of log2 N steps),
a share's range computed from the node's first rank only, and a stale step
count - and the load loop.

**And the speed the slice exists for, checked, not assumed.** The pipelining
slice passed its model and every test while its loop ran deep trees at a
fraction of the speed it claimed, and only a timing simulation found it. So
report, from the VM, the fcollect of 1 MiB per PE and of 8 bytes per PE on
four and five logical nodes, before the slice (the or-tree, pipelined) and
after, the median of 31 each; and say in design §5.8 what the step count and
the bytes each lead moves predict against what was measured.

## Stop and ask

If a node's share cannot be ranged in O(its members) per call; if the words
need to grow with anything but the node count and the team count; or if the
contract in `internode.h` must change.
