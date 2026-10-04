# §7.1 step 7, slice 4: counters per team, buffers per PE - step E of the team redesign

**Status: written on 2026-10-04 from `docs/design/team-resources-proposal.md`
(the proposal) §4, §5 items 3 and 4, §6 and §7, the author's answers to its §8
(`docs/decisions/2026-10-02.md`, items 2 and 8), and the record of step D
(`docs/decisions/2026-10-03-team-storage.md`, `2026-10-03-team-allocation.md`
and the merge point's, `2026-10-04-step-d-merge-point.md`). It is the second of
the two team briefs: step E, whose two parts are the proposal's slices 3
(reductions and broadcasts) and 4 (scans, the default of 256, the
measurements), with a merge point after them.** The author's decisions behind
it: a significant rewrite is accepted for the better design, both steps
precede the first tag, and D is a verified checkpoint before E begins
(2026-10-02, items 2 and 4). It is the proposal's build plan, not a new design:
build from it without waiting, and put any point where the build must depart
from the proposal to the author before building that point
(`docs/prompts/task-first-release.md`).

What E changes, in one sentence: a team number stops owning 200 KiB of slots
on every PE and owns counters and one small slot a position instead, while the
16 KiB pipeline slots move to each PE's threads, one *lane* a thread, lent to
one large collective at a time by a *grant* that tells a child its parent is
in the same collective - so that a number costs a PE about 15 KiB and the
default of `OSOE_MAX_TEAMS` can be 256. Nothing changes for a barrier, a gather
or a small collective; a large reduction or scan may pay one hop, once.

It is written for a session that starts cold, in this repository, with the
VMs below. Nothing in it needs the conversation that wrote it.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks** - a constraint on
  the seams, not work here (design §1). The grant is a put and a fenced add,
  which every transport has; a lane's index on the wire is a word where an
  offset was arithmetic.
- **For the teams** (the proposal's own): no limit on teams that a program
  meets before it has many teams *on one PE*, and no memory set aside for
  teams that do not exist.

## Read first

1. `CLAUDE.md`: delegation, names (words, not contractions - *lane*, *grant*,
   *small slot*, *class* are this brief's), the rename audit rule.
2. The proposal, whole, §4 above all: E1 to E10 are what to build, E5 is the
   argument every part of this brief rests on, and "What it costs" is the
   claim the measurements must hold to. §8 is answered: E2 (a setting,
   `OSOE_SMALL_COLLECTIVE_BYTES`, default 1 KiB), E9 (a lane per thread
   record) and E10 (the default of 256) carry the answers.
3. The record of the day the steps were decided, `docs/decisions/2026-10-02.md`,
   items 2, 4, 6 and 8: the decisions, the merge rule, the README rule, the
   answers.
4. Step D's records: `2026-10-03-team-storage.md` (a node's team blocks a pool
   found by name), `2026-10-03-team-allocation.md` (a number each member PE's
   own, agreed by an AND reduction and given back; "What a number owns, and what
   is reset when a PE takes it again" is the table E's layout must keep true)
   and the merge point's, `2026-10-04-step-d-merge-point.md`, with the Pi's
   numbers E is measured against. And the review of D's merge point,
   `docs/reviews/archive/phase1-step7-slice3-review.md`, whose fixes come
   before this brief's first part.
5. Design §5.5 (the reduction's destination), §5.7 (team resources as built
   through D), §5.8 (the generic internode phase: pipelined pieces, the
   allgather, the scans), §3.16 (atomics: named orders only, no fences).
6. The specification: §9.10 (collectives are not concurrent on one team, and
   the rules for a team's collectives across threads), §9.4 (teams).
7. The code, whole where named whole:
   - `internal-region.h` (whole): the layout E rewrites - per team number the
     slots (`osoe_internal_team_slots_bytes`), the words (an arrival counter a
     child position and the release counter, then D's two split lines and the
     release line's spare words, `OSOE_INTERNAL_COLLECT_LENGTH` and `_TOTAL`),
     the allgather's words, the scans' area;
   - `internode-generic.c` (whole): the header comment's five rules of the
     pipelined loop; `child_slot`, `release_slot`, `arrival_counter` and
     `release_counter` (`:231-274`), the addressing by number E replaces;
     `in_pieces` (`:770`), `osoe_internode_generic_reduce` (`:819`) and
     `_broadcast` (`:1243`); the scan's slots and counters (`:1321-1360`) and
     `osoe_internode_generic_scan` (`:1437`); `osoe_internode_generic_number_taken`
     (`:1566`), which zeroes a number's counters when a PE takes it again;
   - `collectives.c`'s `data_collective_at` (`:130`): the lead's wait for its
     node's members (`:180`), before which E8's entry call goes, and the
     internode calls (`:198-205`); the scans' entry in `scan.c`;
   - `internode.h` (whole): the seam, its Teams section, and how a backend
     that offloads a collective ignores what it does not need;
   - `bootstrap.h` and `bootstrap.c`'s reading of `OSOE_COLLECTIVE_PIPELINE`
     (`bootstrap.h:116-130`, `bootstrap.c:331-338`), its agreement among a
     node's PEs (`node.c:1143-1147`) and between nodes (`rendezvous.c`'s
     `struct agreement`, `:48-60` and `:126-130`): `OSOE_SMALL_COLLECTIVE_BYTES`
     is agreed the same three ways;
   - the thread records (`node.h`, `threads_per_pe`, `OSOE_MAX_THREADS`): a
     lane follows a thread record's index;
   - the tests that pin pieces in flight: `test-pipeline.c`
     (`shmem.pipeline.*`) and the scan's (`shmem.scan.pipeline-*`).
8. The models: `tools/modelcheck/pipeline_model.py` (the loop, its D = 1 cases
   being the small slot's argument), `scan_pipeline_model.py`, and the two the
   proposal wrote for E and left outside the tree,
   `build/scratch/teams-design/lane_model.py` (the slots a node's, the grant;
   eleven design cases, `no_grant` and `stale_grant`) and `grant_timing.py`
   (the loop's timing with and without the grant, "What it costs").

## The state of the tree

- `main` holds step D, verified whole at its merge point
  (`2026-10-04-step-d-merge-point.md`), and then the review of that merge
  point and its fixes. Start from `main` after those fixes.
- The VMs (`dev/lima/`, `docs/README.md`, design §8.5): `osoe`, shared with
  other sessions - every run under the lock (`exec 9>/tmp/osoe-vm.lock;
  flock -w 10800 9`), killed by PID, `git` not installed there; `osoe-load`
  and `osoe-x86` until the tag (the author's word of 2026-10-02). Logical
  nodes: `osoe-launch -m` runs up to 254 of them on one machine.
- The Pi cluster holds a tree in `~/osoe` (D's merge point copied it) and
  nothing else of ours; touch nothing outside `~/osoe`. Reach it through the
  Mac; a script that ssh-es into the nodes holds a session open on each while
  jobs start, or logind removes `/dev/shm` ten seconds after it ends
  (`RemoveIPC`). D's merge point found the cluster faster than the
  2026-09-28 records on the 8-byte reduce and broadcast, the records' own tree
  included: compare E with D's merge point, alternating on the day, not with
  older records.
- The host disk is near full; remove each part's worktree once it has merged.

## How to work

- **A branch and a worktree a part** (`build/scratch/wt/<branch>`, `git
  worktree add`); never edit a worktree a verification is building from.
- **Models before code**, each with its broken variants failing before its
  passing means anything (below).
- **Verification of a part**: the four presets with the floor pins on,
  `tier3`, the `models` preset, the SOS team and collective programs,
  `osoe-pacer` (`collectives` sweeping sizes across the class boundary, and a
  `mix` run), and every new test against the mutation it was written for.
  Long models run in the background from the main session, never from a
  subagent.
- **A speed claim is simulated and timed before it merges** (the rule of
  2026-09-26): "one hop at most, once, on a large reduction or scan" is
  `grant_timing.py`'s, and the measurements (Part 2) are what hold it.
- **Merge into `main` at green checkpoints**: Part 1 when it is green, Part 2
  when it is green, then the merge point. The README is true of the tree at
  every commit: the sentences D left *temporary* are E's to replace, and E's
  own temporary ones say so.
- **An adversarial read before every merge**, by a fresh agent given the diff
  and E5's rule, asked to find the write into a slot or a lane that its owner
  does not wait for.
- **Subagents** (CLAUDE.md): Sonnet for the sweeps (every caller of the slot
  addressing; every test that names a slot's size; `osoe-stat`'s reading of
  the region), for running presets and reporting failures, for the checklist
  passes. The protocol work - the layout, the grant, the lane's binding, the
  models - stays in the main session.
- **Names.** The proposal's words: *small* and *large* (the class), *small
  slot*, *lane*, *grant*, *lane index*. In code: `osoe_collective_class`,
  `small_slot`, `lane`, `grant_counter`, `lane_index`; the setting
  `OSOE_SMALL_COLLECTIVE_BYTES`.

## Part 1: reductions and broadcasts (proposal §5 item 3)

### Models first

1. **`lane_model.py` into the tree** (`tools/modelcheck/`, the `model`
   label, the README's table), and taken from a lane a node to a lane a
   thread (E9): a node runs two threads of one PE, each leading collectives of
   its own teams, each with its own lane; the grant carries the parent's lane
   index and a child's word beside its arrival counter carries its own (E4),
   and every put goes to the lane those words name. Its properties stay UP,
   DOWN and liveness; its breakages: `no_grant` and `stale_grant` as they are,
   and **a lane index read before its word is written** (a child that puts
   into the lane of the parent's previous grant, or a parent that releases into
   a child's previous lane) - each must fail. The case the model found and the
   proposal records - a child outside the collective its parent is in - stays
   among the design cases.
2. **The small slot** is `pipeline_model.py` at D = 1, whose cases exist:
   name them in the record as the small class's argument (E3), and add one
   whose two teams share a lead and alternate small and large collectives, the
   small through each team's own slot and the large through the lead's lane,
   if the lane model does not already hold that interleaving.
3. **`grant_timing.py` into the tree**, beside `scan_timing.py`, as the record
   of "What it costs": its figures in the record, rerun.

### What to build

4. **`OSOE_SMALL_COLLECTIVE_BYTES`** (E1, E2): default 1,024, from 128 to
   16,384; read and validated by every PE in `osoe_bootstrap_read` as
   `OSOE_COLLECTIVE_PIPELINE` is; compared among a node's PEs in
   `open_control` (it lays out every PE's internal region) and agreed between
   nodes at the rendezvous (`struct agreement`, a new field, and with it a new
   `UP_MAGIC`, as the up header's static assertion asks of a new shape;
   `shmem.job.disagree.small-collective-bytes` and the invalid-value tests
   beside `shmem.collective-pipeline.*`). `SHMEM_INFO`'s text; a README
   row, *checked* between nodes.
5. **The class rule** (E1): a data collective is *small* when its bytes are at
   most `OSOE_SMALL_COLLECTIVE_BYTES`, *large* otherwise, decided from the
   call's bytes by every lead alike - for a reduction its `count × width`, for
   a broadcast its bytes - in one function (`osoe_collective_class`) the
   reduction, the broadcast and the scan call, so that one place decides.
6. **The layout** (E2, E4; `internal-region.h`, the file's comment rewritten):
   per team number, `fanout + 1` lines of a counter, a lane-index word and a
   small slot - `OSOE_SMALL_COLLECTIVE_BYTES` plus the piece header
   (`HEADER_BYTES`, 16 bytes; the proposal reckoned 32), rounded up to the
   region's 128-byte line - a child position each and one for the parent; a
   grant line; D's two split lines and the spare words,
   kept (the split's mask and marks, the collect's length and total); the
   allgather's words as they are; the scans' small lines likewise (Part 2).
   Then, past every number's block, **one lane per thread record**: `fanout +
   1` rows of D slots of 16 KiB, a lane's place its index times its size, the
   same arithmetic on every node. `osoe_internode_generic_number_taken` zeroes
   the number's counters and grant line as it zeroes them now; a lane holds no
   counter and needs no zeroing (its rows are read only once a counter says
   they are written). The table of "What a number owns" in D's record is the
   check: every row of it true of the new layout, written down in this part's
   record.
7. **Small collectives** (E3): one piece, into the neighbour's small slot for
   the position, then the fenced add - no grant, no lane, no change in hops.
   The piece header and its check as they are.
8. **Large collectives** (E4): a lead entering a large reduction binds its
   thread's lane to it and sends each child of its tree a grant - the epoch,
   the collective's identity and the lane's index, put into the child's grant
   line for the team, then one fenced add to the child's grant counter - as it
   enters, **before it waits for its own node's members** (E8, below). A child
   sends its first piece up only once its grant counter shows this
   collective's grant, counted cumulatively and as at least, and the grant's
   epoch and identity are its own: a difference ends the job naming both leads.
   A child writes its own lane's index beside the parent's arrival counter for
   its position before its first fenced add of the collective; the parent
   reads it when it first sends that child a release. A large broadcast needs
   no grant - its pieces go up empty and a child's first arrival carries its
   lane's index. From there `in_pieces` runs by its five rules as written, a
   slot's address taken from the neighbour's word where it is computed from
   the number today. At return the lane is free for the thread's next
   collective, on any team (E5).
9. **The entry call** (E8; `internode.h`): one call more, for the grant to
   leave before the intranode phase is over - the local lead calls it as it
   enters a reduction, with the team, the epoch and the bytes; it never blocks;
   a backend that offloads the reduction ignores it. In `data_collective_at`
   before the lead's wait for its node's members (`collectives.c:180`).
10. **`osoe-stat`**: the region's new shape where it reads it (a lane's
    binding, if the tree state publishes it, beside `most_pieces_in_flight`).
11. **Tests** (proposal §6): two teams whose leads coincide on a node running
    large collectives back to back and, under `SHMEM_THREAD_MULTIPLE`, at once
    on two threads; a child outside its parent's current team, held by a
    parent that enters late - the model's failing case, on the code, with
    `no_grant` as its mutation; the class boundary, one byte either side, at
    the setting's smallest and at its default, the large class's pieces in
    flight pinned as `shmem.pipeline.*` pins them now; the mismatch a grant
    names (two leads in different collectives of one team), ending the job by
    name; `shmem.pipeline.*` and the scan's pipeline tests moved to the large
    class, which is the change the proposal names - any other test that must
    change is a stop-and-ask. Every model breakage that is a line of code is a
    mutation run against the tests, as D's record did.
12. **The records**: design §5.8 (the slots, the lanes, the grant, the class),
    §5.7's what-a-number-owns, `internode.h`'s header; the README's rows and
    the cost table's reduction and broadcast lines.

Not touched (proposal §4): the barrier, the gather, the intranode phases, the
transport.

## Part 2: scans; the default of 256; the measurements (proposal §5 item 4)

1. **`scan_pipeline_model.py` with lanes and grants** first (proposal §6): a
   member granting its tree children as it enters, small scans in the number's
   small lines; its breakages as the lane model's.
2. **The scans** (E6): small scans in the number's small slots, large ones
   through each member's lane, a member granting its tree children as it
   enters; a large scan's piece becomes the lane's 16 KiB where it is 4 KiB
   today. `osoe_internode_scan` is already every member's call at entry.
3. **`OSOE_MAX_TEAMS` defaults to 256** (E10, D8): the README row as E10
   writes it, the *temporary* sentences D left replaced (the Limits bullet's
   200 KiB, the release note's line on the default), `SHMEM_INFO`'s text, the
   decisions index's settings lines, and the `/dev/shm` capacity check's sum,
   whose share a number now is about 15 KiB.
4. **The measurements** (proposal §6, the rule of 2026-09-26): on the Pi at
   4 × 4 and in the VM on 5 and 8 logical nodes, before and after E,
   alternating on the day: reductions, broadcasts and scans of 8 bytes,
   1 KiB, 64 KiB and 1 MiB; the barrier; `osoe-pacer --quick` and an hour of
   `mix`. Expected: nothing moves but the large reductions and scans, by at
   most a hop. And the memory: a number's bytes at the defaults against the
   proposal's 15 KiB, the lanes' against E9's 2.7 MiB a PE.
5. **The records**: design §5.8 (the scans), §7.1's step 7, the README's cost
   table, its limits and settings rows, `docs/pi-cluster-results.md`.

## The merge point

With both parts on `main`: the four presets, `tier3` and `models` on `main`
whole; the SOS sweep, at 4 × 2 and past 61 PEs (the release checklist's
acceptance: `shmem_team_reuse_teams`, `shmem_team_max`, the active-set
programs and `repeated_barriers` at 70 PEs); on the Pi at 4 × 4, the
measurements above against D's merge point, alternating. Record the numbers.
Then the full review (`docs/reviews/adversarial-review-prompt.md`, its
deliverables named `phase1-step7-slice4-*`), and its fixes. `main` is then the
release candidate the release point is checked on.

## Not in this slice

- A team of one PE claiming no pool block (step D's first read, item 5): after
  the release, on the release checklist's list.
- Any change to the barrier, the gather, the intranode phases, the transport,
  any wake.
- A lane pool shared by a PE's threads (E9 decided a lane per thread record).

## Stop and ask

- A write into a lead's slots or a lane that the lead does not wait for before
  it returns - E5's rule false for some collective.
- A message for a team that can arrive at a node after the last local member
  has left the team.
- The Pi showing more than the one hop on any line, or anything on a line that
  should not move.
- A test that must change other than the pipeline tests' move to the large
  class.
- A number costing a PE more than the proposal's 15 KiB at the defaults by
  more than its gather and split lines, or the lanes more than E9's 2.7 MiB.
- Any point where the build departs from the proposal: put it to the author
  before building that point.

## The record

A dated decisions file for each part, in the form of D's; the decisions index's
team and settings lines updated in the same commits; `docs/prompts/README.md`'s
row for this brief filled in as the parts land, and this file moved to `done/`
when the merge point is passed; the README true at each merge, its temporary
sentences marked.
