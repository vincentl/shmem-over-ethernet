# Round six, question 1: is osoe OpenSHMEM 1.6? (the release candidate, step E's merge point)

Review of `main` at `375973a`, run 2026-10-04 against
`docs/reviews/adversarial-review-prompt.md`, with the changes its lead gave: the tree
is the one step E of the team redesign left (`docs/design/team-resources-proposal.md`
§4; `docs/prompts/phase1-step7-slice4-prompt.md`; the records
`docs/decisions/2026-10-04-team-lanes.md` and `2026-10-04-team-scans.md`; design §5.7
and §5.8), the release candidate; the previous round is `phase1-step7-slice3-*`
(`d8a651d`), whose fixes merged as `docs/decisions/2026-10-04-step-d-review-fixes.md`.
`main` has since moved by one commit of text, `b2804cd` (step E's merge point,
`docs/decisions/2026-10-04-step-e-merge-point.md`), which this review reads as a record
and does not judge. Labels: *documented*, *observed in source*, *measured*, *recalled*.
Section numbers are the specification's (`docs/reference/OpenSHMEM-1.6.pdf`, present,
read as `pdftotext -layout`; byte for byte the text the previous round read).

**How it was run.** The review worktree was copied into the `osoe-load` VM
(`/tmp/conf-e/src`; identical to the 1,486 tracked files of `375973a`, checked by
SHA-1) and built with the `release` preset: 19 s, no warnings. Probes were built with
that build's `oshcc` and run under its `osoe-launch` on one to 33 PEs and one to 33
logical nodes, every build and run under `/tmp/osoe-vm.lock`; a ThreadSanitizer build of
the library from the same copy ran the threaded probes. Four mutants of
`internode-generic.c` and one of `context.c`, each made in a copy and restored by
SHA-1, showed the new probes and the previous round's fixed race can fail. The Pi
cluster was not used, and no finding needs it. The merge point's runs were taken as
evidence, not repeated: the four presets, `tier3` and `models` green on `main` whole,
the SOS sweep at 4 × 2 and the 70-PE acceptance. The probe sources, scripts and result
files are in this session's scratchpad (Sources); the VM copy was removed at the end.

## Verdict

**Yes. In this question's area the tree can be tagged 0.1.0 as it is.** Every result
§9.10 defines for the collectives step E rebuilt - reductions of every type and
operation of Table 10, broadcasts, inclusive and exclusive scans, collect and fcollect,
and the deprecated active-set forms that go through the same path - came out as the
text says, element for element, on seven team shapes, at sizes either side of
`OSOE_SMALL_COLLECTIVE_BYTES` and of every piece edge of the lanes, in place and not,
back to back with no synchronization between them, under four settings of the
collectives and split wires: 514,710 calls checked, none wrong. Under
`SHMEM_THREAD_MULTIPLE`, four threads a PE running large and small collectives on four
teams at once - in waves whose threads exit, so that the next wave takes their places
and lanes, with the teams moving from thread to thread and split anew each wave - gave
no wrong result, and no ThreadSanitizer report. The probes see what they claim: three
one-line breakages of the lanes' code each fail them, and a fourth, the model's
`one_lane`, fails the threaded one. The previous round's fix of its F1 holds and is
needed: with the lock taken out again, its probe reports four races a run. The README's
new team limit is the library's own behaviour, checked against the simulation its
figures came from, and every previous round's probe behaves on this tree as before,
with 253 teams a PE where there were 61.

**Nothing blocks, and nothing must precede the tag.** Four minors, each a sentence or a
test:

- **M1**: the README's sentence on mismatched scans holds only for a team spanning
  nodes; within one node a large scan given other counts, or counts on either side of
  the line, returns wrong results with status 0, as the other collectives do there.
- **M2**: design §10 still lists "Thorough 1.6 spec read" as blocking. Four rounds have
  now read the C interface against the code; it can be closed - the author's.
- **M3**: the README's SOS sentence is the tree of 28 September's; the release tree has
  its own sweep, which a stranger should be given.
- **M4**: no test in the suite runs large scans or broadcasts from two threads at once;
  only large reductions. This round's probe would be that test (after release).

**The release point's conditions in this area** (design §1 items 2 and 3;
`docs/release-checklist.md`): OpenSHMEM 1.6 complete, as the checklist ticks and this
round confirms; the SOS suite clean but for SOS's own extensions and its two programs
outside the specification, met on this code by the merge point's sweep and its 70-PE
acceptance (*documented*). The checklist asks for that sweep "on the tree to be tagged":
if this round's fixes touch only text, the merge point's sweep is that tree's; if they
touch the library, it is to be run again.

## The previous round's findings, checked as claims

- **F1, a team's destroy read the context pool unlocked: fixed, and the fix is
  needed** (*observed in source*, *measured*). `osoe_context_team_destroyed` and
  `osoe_context_finalize` read the pool under `pool_mutex` and drop it around each
  context they end (`context.c:266-305`). The previous round's own probe, `ctx-mt.c`
  (one thread splitting the world, making a context on the new team and destroying the
  team, 200 times; another making, using and destroying private contexts, 1,000 times),
  on the ThreadSanitizer library, one PE a logical node: 0 reports. With the scan's lock
  taken out again in a copy: 4 data-race reports in each of two runs, status 66, every
  one at `destroy` (`context.c:225`) against the scan; restored, 0. `resolve`, the other
  unlocked read of `live` round four named, reads only the element the caller's own
  handle names, which no other thread may destroy meanwhile in a valid program; no
  report from it in any run. The suite's `shmem.context-team-destroy` gained the mode
  the fixes' read found missing, a shareable context left to the team's destroy
  (*documented*, the lanes record).
- **F2, a number taken 2^32 - 1 times ends the job**: unchanged (`team.c:1073`) and on
  the release checklist's list of what follows the release, as the synthesis decided
  (*observed in source*, *documented*). Step E does not move it.
- **M1, a split fails on what its members hold between them: fixed** (*measured*). The
  README states the rule with both orders' figures, and the message says it: "no team
  number is free on every PE of the new team - between them they hold every one a
  program may use, though each may hold fewer: OSOE_MAX_TEAMS is 256 ...". The figures
  are checked below ("`OSOE_MAX_TEAMS` 256").
- **M2, `iput` strides below 1 silent**: on the release checklist's list, and unchanged
  (*measured*: a destination stride of 0 lands only the last element; -1 writes the
  elements backwards from `dest`, into the object before it; both exit 0, where
  `ibput`'s stride below `bsize` ends the job by name).
- **M3, the README's model count: fixed** (*documented*): 29 tests, the 27 models of
  `tools/modelcheck/CMakeLists.txt` with `get_completion.equality` and `selftest`, and
  the list names step D's four and the lanes.
- **M4, design §10's "Thorough 1.6 spec read", still blocking: not done** - this
  round's M2.
- **The previous rounds' probes, rebuilt and rerun on this tree, behave as they did**
  (*measured*, 100 runs): `lifecycle`, `ptr-access` (and with `OSOE_FORCE_REMOTE`),
  `memory`, `nobarrier`, `ordering` with split wires, `amo-atomicity`, `lock-order`,
  `sessions-deprecated`, `profile-probe`, Example 37 with an in-place complex inscan,
  `shared-ptr`, `shared-split`, `split2d` (`xrange` 1 to 100, and 3 over seven),
  `threads-teams` in both modes, `thread-exit`, `threads-alive` (15 workers pass, 16 end
  the job naming `OSOE_MAX_THREADS`), the sixteen behaviours of `invalid.c` on four maps,
  `limits` (63 and 4,095 contexts), `places` (`shmem_my_pe` takes no place),
  `teams-exhaust` (5,001 splits in each of two sessions, at the default and at
  `OSOE_MAX_TEAMS=4`), `signal-atomic` on four maps, `signal-c11`, `ibstride` with split
  wires, `heapfull`, `mt-quiet4`; and the previous
  round's team probes - `teams-alive` (below), the specification's Examples 10, 11 and
  12 (12 printing exactly its expected output, in order, on four maps of twelve PEs),
  `handles` (the stale handle and the live private context named), `teams-disjoint` on
  five maps and with split wires, `teams-mt` on five maps and perturbed, `teams-blocks`
  (the documented exception as documented), `ctx-mt`. All exit as they did on
  `d8a651d`.

## Step E, read against §9.10 and run

Read: §9.10's preamble (team-based collectives, "the PE may immediately call another
collective operation on that same team", "may not be called simultaneously by multiple
threads on a given team"), §9.10.7, §9.10.8's same-count rule, §9.10.9 whole with Table
10, §9.10.10 whole, §9.2 (items 1 to 5) and §9.2.3, §9.4.6; `scans.c`; `collectives.c`
(`data_collective`, `data_collective_at`, `osoe_scan`); `internode-generic.c` (the
class rule, the pieces loop, grants and lane words, the broadcast, the scan, a number's
reset); the diff of `context.c` and `team.c`; the two records and the merge point's.

### What the text requires, and what the tree does

1. **Every result, by value** (§9.10.7: "the dest object is updated on all PEs" of a
   team broadcast, the root's included; §9.10.9; §9.10.10: dest_i,j the sum over ranks
   0 to i, or to i - 1 and 0 at rank 0; §9.10.8) (*measured*, `coll-matrix.c`). Each PE
   runs 903 collectives on each of its teams - the world, its evens and its odds (which
   share no PE and so may share a number), the world reversed, a `split_2d`'s rows and
   columns, `SHMEM_TEAM_SHARED` - in an order shuffled alike on every PE, with no
   synchronization between calls: a reduction of every type of Table 10 under every
   operation it supports, at one element, just past the class line and three slots and
   more; six types (`long`, `double`, `complexd`, `uchar`, `longdouble`, `int16`) at
   eight sizes, an element either side of the line and of a 16 KiB slot; inclusive and
   exclusive scans of every type SUM takes, six of them at eleven sizes either side of
   the line and of a scan's piece edges (8 KiB, D × 8 KiB, D × 16 KiB) and past them;
   broadcasts of 1 to 16-byte elements at eight sizes from a moving root and the last;
   collect with lengths from 0 to 49 and fcollect up to 48 KiB a PE; about half the
   reductions and scans in place. Every PE checks every element of its own dest, the
   root's broadcast dest included, against a fold over the team's ranks. On `0,0,1,1`,
   `0,1,2,3`, `0,1,0,1`, `0,0,0,0`, `0,1,2,0,1,2`, eight nodes of one at a fan-out of 2
   with D of 1 and of 4,
   thirteen PEs on five nodes at a fan-out of 2, `OSOE_SMALL_COLLECTIVE_BYTES` of 128
   and of 16,384, D of 8, 1 KiB chunks with split wires, `OSOE_TEST_PERTURB`, and
   eighteen PEs on six nodes at 128 bytes, where `collect`'s lengths reduction is itself
   large: **514,710 calls, each checked on its PE, 0 wrong, every run exit 0**.
2. **The probe sees the paths it claims** (*measured*; `mutate.py`, each a line of
   `internode-generic.c`, the library rebuilt, the probes unchanged): a large scan's
   down sending a member's own source where the prefix belongs - wrong results on every
   PE of four and of six, the first a 1,032-byte exscan, the mutation reaching large
   scans alone, and on the threaded probe below; a small scan's member leaving its last
   child's total out of what it sends up - wrong results on every PE of six, the first
   an 8-byte inscan, and none on four nodes of one, where at a fan-out of 4 no member
   below the root has children; a large reduction's lead leaving its last child's piece
   out - wrong results on every PE of both probes; every thread of a PE lending one lane,
   the lane model's `one_lane` - nothing on the matrix, which runs one thread, and the
   threaded probe ending the job by name ("should be piece 0 (16368 bytes) of epoch 1's
   sum ... and is nothing"). Restored and rebuilt, identical by SHA-1.
3. **Collectives on different teams in parallel** (§9.4; §9.2 items 3 and 4: a blocked
   thread does not stop the others, every call thread-safe) (*measured*, `lanes-mt.c`).
   `SHMEM_THREAD_MULTIPLE`, four threads a PE, thread i on team i only - the world, the
   world reversed, the evens or the odds, a `split_2d` row - each round a sum of 3 to
   5,000 longs, an in-place double maximum, a broadcast from a moving root and an
   inclusive or exclusive scan, in place every other round, all checked. In waves: the
   threads exit and the next wave's take their places again (`OSOE_MAX_THREADS=5`, so
   they must); with `ROTATE` the teams move between threads each wave, so a team's
   collectives are led by another thread, and another lane, than before; with
   `MODE=split` each thread splits its team anew each wave and destroys it, so that
   splits of different parents run at once and numbers are taken again. On `0,0,1,1`,
   `0,1,2,3`, `0,1,0,1`, `0,1,2,0,1,2`, `0,0,0,0`, eight nodes at a fan-out of 2 and D of
   4, and perturbed: 0 failures in 16 runs. On the ThreadSanitizer library, one PE a
   logical node so that every thread writing a PE's memory is in the process it watches:
   three runs, 0 reports.
4. **The deprecated active-set collectives** (Annex F; §9.10.7's "updated on all PEs
   other than the root PE"; §9.10.9's pWrk) (*measured*, `deprecated-e.c`): `sum_to_all`
   of `long`, `max_to_all` of `long double`, `broadcast64`, `collect64` and
   `fcollect64` at 3, 128, 129 and 3,000 elements, on the whole job - which takes the
   team path and so the lanes - and on sets of all but PE 0 and of every other PE, which
   run PE by PE; the active-set root's dest left alone: 0 failures on five maps and at
   128 bytes.
5. **`OSOE_MAX_TEAMS` 256, as a departure** (§9.4.6: "If the new team cannot be created,
   ... nonzero on all PEs in the parent team"): permitted, and listed (README Limits,
   Teams). *Measured*: 253 teams of a PE's own held at once on four maps, each failing
   split nonzero with `SHMEM_TEAM_INVALID` on every parent PE and said once by the
   parent's rank 0; every team destroyed and 253 made again; 13 at 16 numbers, 1,021 at
   1,024 with 4,096 blocks - and 1,021 at 1,024 with `OSOE_SMALL_COLLECTIVE_BYTES=128`,
   where a split's agreement, 17 words, is itself a large reduction through the lanes.
   The README's figures for pairs - "every pair of 129 PEs made a team in a double loop
   fails at a pair holding 127 and 126 ... such rounds reach 253 teams a PE only at 254
   PEs" - came from a simulation (`pairs_sim.py`, step E's second read) that assumes a
   split takes the lowest number free on every member. The library against it, scaled
   down (*measured*, `pairs.c`): at 16 numbers, the double loop over 9 PEs fails at
   {6, 8} after 34 teams, holding 7 and 6, and over 13 at {2, 12} after 32, holding 11 and
   2; rounds of disjoint pairs over 14 PEs make all 91, over 15 fail at {0, 2} after 91,
   holding 12 and 13; at 64, the double loop over 33 PEs fails at {30, 32} after 526,
   holding 31 and 30 - each exactly the simulation's figure. Not run at 129 PEs (8,256
   splits of the world), so the README's figure stands on the simulation, which the
   library matches wherever it was compared.
6. **What a program outside the specification meets** (*measured*, `mismatch-e.c`;
   README Limits, Failures). Two PEs on two nodes, an inclusive scan of 100 against 200
   longs (800 and 1,600 bytes): with the root small, the child waits for a grant and the
   deadlock detector ends the job at 2 s, naming both PEs in
   `shmem_long_sum_inscan`; with the root large, it ends naming the child's lane word;
   2,000 against 3,000 longs ends naming the grant's bytes, 16,000 against 24,000; 50
   against 100, both small, returns with 50 of PE 1's 100 elements wrong and status 0.
   All as the README says - on two nodes. On one node, M1.

**Nothing found wrong against the text.** A collective of no elements returns at once on
every member, without synchronizing (`collectives.c:117-118`, and `osoe_scan`'s like
it), which §9.10 allows: a data collective promises its dest and source, not a barrier.

### The README's Limits, checked against this tree

Each entry step E wrote or touched, and those this question names:

- **Teams, first entry** (`OSOE_MAX_TEAMS`, 253 a PE, what members hold between them,
  the pairs figures, a per-node split of `SHMEM_TEAM_SHARED`): true (items 5 above;
  `split-shared` and the per-node acceptance at 61 to 64 nodes, *documented*). The
  second and third entries (the node's pool, two splits on threads at once): unchanged
  since step D and true (`teams-blocks`, `teams-mt`, *measured*).
- **Threads and contexts**: unchanged and true - `SHMEM_THREAD_SERIALIZED` from
  `shmem_init`, which §9.2.3 allows ("the implementation can choose to provide any one
  of the defined thread levels"); `OSOE_MAX_THREADS`, its place taken at the first call
  that does more than report (`threads-alive`, and `places` for `shmem_my_pe`);
  `OSOE_MAX_CONTEXTS`; the unenforced `num_contexts`; sessions.
- **Failures, collectives called differently**: true for reductions and broadcasts (the
  suite's `shmem.lanes.class-split-*` and `mismatch`, *documented*), and for scans on a
  team spanning nodes (item 6); not for a scan within one node (M1). The `shmem_malloc`
  entry: true (`malloc-mismatch`, *measured*).
- **The settings rows** `OSOE_MAX_TEAMS`, `OSOE_SMALL_COLLECTIVE_BYTES`,
  `OSOE_COLLECTIVE_PIPELINE`, `OSOE_MAX_THREADS`: say what the code does
  (*observed in source*), and `SHMEM_INFO` prints them (*measured*).
- **Completeness**: step E adds no behaviour a valid program can meet that the README
  does not list. The lanes add memory (a lane a thread place, which the settings rows
  and the scaling section price) and the grant's wait (the cost rows), not a limit; a
  thread is in one collective at a time, so its one lane is enough for any valid
  program, and the threaded runs above find no wait across teams.

## Findings

Nothing rated *blocks release* or *fix before release*.

### Minor

- **M1. Within one node a scan is compared with nothing, and the README says
  otherwise** (*measured*, *observed in source*). README :912-918: "A scan's members are
  compared with their tree parent when the scan is large - ... the PEs of one node too -
  ... given counts on either side of the line, they end the job by name or wait until
  the deadlock detector ends it." That is the internode scan's tree
  (`internode-generic.c:1934`, its checks at `:1870-1932`), which only a team spanning
  nodes takes (`collectives.c:616-617`); a team all of whose members are on one node -
  every team of a one-node job, `SHMEM_TEAM_SHARED` and its splits in any job - is
  scanned by its local lead with its own count and kind for every member
  (`collectives.c:622-679`). *Scenario*: `mismatch-e.c` on `0,0`: 100 against 200 longs,
  either way round, and 2,000 against 3,000, each return with status 0 - PE 1 with 100
  of its 200, and 1,000 of its 3,000, elements wrong - and where the lead's count is the
  larger it writes that many into the other member's dest. Undefined input
  (§9.10.10's same arrays), so not a departure, and the entry's heading already says a
  node's PEs are compared with nobody; but the sentence states the opposite for scans.
  *Fix*: "when the scan is large and its team spans nodes ... ; within one node, as for
  the other collectives, nothing is compared". Confidence high.
- **M2. Design §10 still lists "Thorough 1.6 spec read" as blocking** (*documented*,
  design :7780-7786; the previous round's M4, left for the author). Rounds three and four
  read §9.1 to §9.13, §8, §10 and Annexes C and F against the code, round five §9.4,
  §9.5 and §9.10's argument rules again after step D, and this round §9.10 whole again
  after step E rebuilt its internode phase, with §9.2 and §9.4.6; each confirmed by
  probes. Its remaining clause - "the exact semantics of the 1.6 session routines if they
  are ever more than no-ops" - is a condition on future work, not on this release, since
  the sessions are accepted no-ops, which §9.9.2 permits. A tagged release whose design
  notes call the read open and blocking contradicts its own checklist's "OpenSHMEM 1.6
  complete". *Fix*: close it, the session clause moved to after the release - the
  author's. Confidence high.
- **M3. The README gives the SOS sweep of 28 September, not the release tree's**
  (*documented*). README :1057-1069 cites 161 of 165 on the Pi and 159 on two maps in the
  VM, "on the tree of 28 September", before the team redesign; the release tree's
  evidence is the merge point's sweep - 161 of 165 programs clean at 4 × 2 in the VM,
  the same four exceptions, line for line step D's - and its 70-PE acceptance with
  nothing skipped (`docs/decisions/2026-10-04-step-e-merge-point.md`). Not wrong, as
  dated; but it is the sentence a stranger reads as the release's conformance evidence.
  *Fix*: one sentence, when the checklist's SOS item is ticked. Confidence high.
- **M4. No test runs large scans or broadcasts from two threads at once**
  (*documented*, *measured*). `shmem.lanes.two-threads` leads two teams' large
  reductions at once on two threads, and the model's `one_lane` breakage fails it alone
  (the lanes record); `scan-turns` passes scans between threads but never runs two at
  once; `osoe-pacer`'s `threads` exercise does RMA and atomics on contexts, no
  collectives. So a lane shared by a scan and a broadcast on two threads would pass the
  suite. `lanes-mt.c` (above) runs exactly that and fails `one_lane` by name.
  *Fix*: it, or a `two-threads` mode with scans and broadcasts, as a test - after the
  release. Confidence high that the gap exists; nothing found wrong in it.

### Refuted, or no finding

- **A broadcast's count**: §9.10.7 asks the same `dest`, `source` and `PE_root` of every
  PE and does not say "the same `nelems`" in words, as §9.10.5, §9.10.6 and §9.10.8 do;
  the class rule rests on it. The README states the premise and what a program that
  breaks it meets (Failures); passing the same objects of `nelems` elements is how
  implementations read it (*recalled*). Not a departure.
- **The SOS programs that fail against release point condition 3** ("except those using
  SOS's own extensions"): `shmem_team_b2b_collectives` collects past its own
  destination and `shmemalign` asks for an alignment §9.3 leaves undefined; both are
  outside the specification, osoe names each, and the runbook lists them. Conformant
  behaviour; the condition is met in its intent.
- **32-bit epochs and scan numbers in grants and lane words**: each is compared only
  after a 64-bit counter shows the word written, so a wrap - after 2^32 collectives of
  one team - can mislead only the checks for invalid programs (*observed in source*).
- **`resolve`'s unlocked read of `live`**: under F1 above.

## The SOS suite as evidence, and what no test exercises

**The merge point's sweep** (*documented*, `build/scratch/merge-e/sos/sos-4x2.txt`, the
lead's run on `main`'s archive): the whole suite at 4 × 2 in the VM, 161 programs clean;
the two C11 programs over `short *` that do not build (SOS's extension); the two failing
runs above. And at 70 PEs on one node and ten (`build/scratch/merge-e/acceptance/`):
`shmem_team_reuse_teams`, `shmem_team_split_2d` and `shmem_team_max` clean,
`reduce_active_set`, `broadcast_active_set` and `repeated_barriers` with nothing
skipped, `collect_active_set` at 32, the most it accepts.

**What SOS exercises of step E, and what it does not** (*observed in source*, the
corpus's `tests-sos`). It reaches the lanes on the world team only, single-threaded:
`to_all.c`'s 128-element `long double` reductions (2 KiB, large), `bcast_flood`'s 100 KB
broadcasts. No SOS program calls a 1.6 scan - its three under `shmemx/` use
`shmemx_` names and 10 elements, small, and passed on the Pi renamed by hand
(`docs/pi-cluster-results.md`) - so the large scans (E6) rest on osoe's suite
(`test-scan.c`, `test-lanes.c`), the stress tester and this round's matrix. Its
programs that reduce or broadcast on a created team do so with a few elements
(`self_collectives` one, `shmem_team_negative_stride` npes, `shmem_team_b2b_collectives`
10), and its 32-element type sweeps run on the world, so no SOS program read here runs a
large collective on a created team; none runs collectives from threads; none holds more
than 64 teams a PE but the 70-PE programs above.

**Requirements no test in any suite exercises**, after step E:

1. Large scans and broadcasts from several threads at once on different teams (M4);
   this round's `lanes-mt.c` does.
2. A one-node team's scan given other counts (M1): undefined input, silent.
3. A number taken 2^32 times (round five's F2, on the list for after the release).
4. The pairs figures at the README's 129 PEs: the simulation, and the library against
   it at 16 and 64 numbers (above).
5. Per-node splits of `SHMEM_TEAM_SHARED` past 61 nodes: the acceptance runs only.
6. From the rounds before, still standing: several callers of the global exit on more
   than one node under a killing launcher; which thread runs the handlers; fence
   ordering across nodes as a third PE sees it; `SHMEM_DEBUG` and the `SMA_` spellings
   of the other three; `iput` strides below 1; a fractional heap filled whole. One item
   of that list is closed in part: data collectives from two threads on two teams at
   once, now `shmem.lanes.two-threads` - large reductions only (item 1).

## What is sound, and what convinced me

- **Step E's collectives give the specification's results.** Not because the suite
  passes, but because a probe written from the text alone - its oracle a fold over the
  team's ranks - found nothing wrong in half a million calls over every type and
  operation, seven team shapes and fourteen configurations, and fails on each of three
  one-line breakages of the code it exercises, each failing exactly the class it
  breaks.
- **`SHMEM_THREAD_MULTIPLE` with lanes**: threads that exit and are replaced, teams that
  move between threads and are split anew, four at once a PE, no wrong result and no
  ThreadSanitizer report; the model's `one_lane` fails the probe by name.
- **The previous round's fix**: shown needed by taking it out (four races a run) and
  holding with it in (none), on the round's own probe.
- **The team limit as the README states it**: 253 held at once and given back, and the
  pairs figures matching the library at two scales, so the README's rule is the code's
  and not only the simulation's.
- **Undefined input made loud between nodes**, as the README says, for every class
  split of a scan; quiet within a node, as it says for everything but one sentence.
- **Everything earlier rounds established still holds** on this tree: 100 runs of their
  probes, behaving as on `d8a651d`.

**Noted outside this question**, for the lead to route: `docs/pi-cluster.md:32` still
gives each PE's internal region as "about 12.6 MiB - 10 since `OSOE_MAX_TEAMS` went to
64", where step E made it about 6 MiB (README, "Memory").

## Sources

The specification: §9.2, §9.2.3, §9.4.6, §9.10 preamble, §9.10.5 to §9.10.10, Table
10, Annex F. The tree at `375973a`: `README.md`; `shmem/src/collectives.c`, `scans.c`,
`broadcast.c`, `internode-generic.c`, `internal-region.h`, `context.c`, `team.c`;
`shmem/tests/test-lanes.c`, `test-scan.c`, `CMakeLists.txt`; `tools/pacer/threads.c`;
`tools/modelcheck/CMakeLists.txt`; design §1, §5.8 and §10; the step E brief and both
records, the step D fixes' record, the round-five deliverables, the release checklist,
`docs/pi-cluster.md` §8, `docs/pi-cluster-results.md`; and in the main checkout the
merge point's record and `build/scratch/merge-e/`. Step E's adversarial read's
`build/scratch/team-scans-adversarial/pairs_sim.py`. The corpus's `tests-sos`.

This round's probes, scripts and results, in this session's scratchpad, a directory
outside the repository (`<scratchpad>/`): `p/coll-matrix.c`, `p/lanes-mt.c`,
`p/mismatch-e.c`, `p/deprecated-e.c`, `p/pairs.c`; `vm/` (`build.sh`, `run-lib.sh`,
`run1.sh` to `run6.sh`, `run-prev1.sh`, `run-prev2.sh`, `build-prev.sh`, `mutate.py`,
`run-mut.sh`, `run-tsan.sh`, `run-tsan-mut.sh`); results `matrix-results.txt`,
`mut-results.txt`, `run3-results.txt` to `run6-results.txt`, `prev1-results.txt`,
`prev2-results.txt`, `tsan-results.txt`, `tsan-mut-results.txt`; the previous rounds'
probes as rerun, `prev/`. A refutation run copies the tree into a VM, builds `release`
(19 s), and builds the probes with its `oshcc`; the mutants are `mutate.py`'s four.
