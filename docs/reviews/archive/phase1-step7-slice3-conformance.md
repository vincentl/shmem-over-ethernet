# Round five, question 1: is osoe OpenSHMEM 1.6? (at step D's merge point)

Review of `main` at `d8a651d`, run 2026-10-04 against
`docs/reviews/adversarial-review-prompt.md`, with the changes its lead gave: the tree
is the one step D of the team redesign left (`docs/design/team-resources-proposal.md`
§3; `docs/prompts/phase1-step7-slice3-prompt.md`; the records
`docs/decisions/2026-10-03-team-storage.md` and `2026-10-03-team-allocation.md`;
design §5.7), the previous round is `phase1-release-fixes-conformance.md` (`08b3da0`),
whose fixes merged as `docs/decisions/2026-10-04-review-fixes.md`, and step E is not
built. Labels: *documented*, *observed in source*, *measured*, *recalled*. Section
numbers are the specification's (`docs/reference/OpenSHMEM-1.6.pdf`, present, read as
`pdftotext -layout`; identical to the previous rounds' `build/scratch/review-r3/spec-1.6.txt`).

**How it was run.** The review worktree was copied into the `osoe-load` VM
(`/tmp/conf-d/src`; identical file for file to the 1,387 tracked files of `d8a651d`,
checked by SHA-1) and built with the `release` preset: 19 s, no warnings. Probes were
built with that build's `oshcc` and run under its `osoe-launch`, on one to 100
logical nodes, each run under `/tmp/osoe-vm.lock`. `osoe-launch` without `-k` stands
for `srun -K1`, with the previous round's caveats (it reacts within a millisecond, and
does not count its own kills). A ThreadSanitizer build of the same copy ran the two
threaded probes; two mutants of `team.c`, each in its own copy, ran the suite's team
tests and the probes. The Pi cluster was not used, and no finding needs it. The
merge point's own runs were taken as evidence, not repeated: the four presets, tier 3
and the models preset all green (`build/scratch/merge-d/main/verify.txt`), and the SOS
sweep (`build/scratch/merge-d/sos/`). The probe sources and every result file are in
this session's scratchpad (Sources); the VM copy was removed at the end.

## Verdict

**Yes - every behaviour §9.4 defines for creating, using and destroying teams is
implemented as the text says on the rebuilt team code, and the README's three new team
entries are true as written.** A split that cannot be made returns nonzero with
`SHMEM_TEAM_INVALID` on every parent PE, for want of numbers and for want of blocks; a
`split_2d` that cannot make one of its teams makes neither; a destroyed team's number
and block come back, and teams that share no PE share numbers while every collective
on them computes right; the specification's own Examples 10, 11 and 12 run as printed,
the last giving its printed output on four maps; per-node splits of
`SHMEM_TEAM_SHARED` succeed on 100 nodes; splits of
four parents at once on four threads of every PE never fail and race nothing that
ThreadSanitizer sees. The previous round's two findings are closed as its synthesis
asked: both sentences are in the README, and the ending loops' fix shrinks the
window in which a killing launcher can kill a caller from 110-230 ms to at most 31.

Two findings, neither blocking:

- **F1 (fix before release, two lines)**: under `SHMEM_THREAD_MULTIPLE`, a team's
  destroy reads the context pool without its mutex while another thread creates or
  destroys contexts - the previous round's C6, then read in source and rated minor,
  now a ThreadSanitizer data race in every run of a valid program of fifty lines. Step D
  makes destroying teams routine, which is why it is worth the two lines now.
- **F2 (after release)**: a PE that has taken one team number 2^32 - 1 times ends the
  job at the next split - about an hour of splitting and destroying on a one-PE job,
  days on a node of several - to keep a diagnostic for programs that use a destroyed
  handle exact. The README lists no such limit.

And four minors: a split can fail when every member is in one team of its own (the
numbers are free on no member *together*), which the README's rule implies and its
headline and the diagnostic hide (M1); the previous round's C5 half on `iput` strides
fell out of both the README and the checklist's after-release list (M2); the README
counts 24 model tests where the tree has 28 (M3); design §10 still lists the 1.6 read
as blocking (M4).

From this question's side, once step E is built, osoe can be released after F1 and
the minors' sentences. Nothing about E's design, read against the text, would change
that (the section "Step E").

## The previous round's findings, checked as claims

- **F1, several callers of `shmem_global_exit` on more than one node: the sentence is
  in, and the liveness fix holds** (*documented*, *measured*). README :1118-1125 now
  says that when PEs of more than one node call it, the first to leave can have Slurm
  kill the others' PEs, callers among them, with their handlers and buffered output, and
  `srun` return 143, and to call it from one PE to keep both. The three ending loops
  ask their condition again after a poll that moved nothing
  (`progress-host.c:583`, `:640`, `:656`). Measured with the previous round's
  `ge-all.c` (every PE calls, instant handlers), `-k`, each rank stamping its end, 20
  runs a map: from the first nonzero exit to the last rank's end, at most **10 ms** on
  `0,0,1,1` (nonzero in 6 runs) and at most **31 ms** on `0,1,2,3` (nonzero in 13),
  where the previous round measured 110 to 230 ms in 13 runs of 20. Under Slurm's
  reaction of about 100 ms the kill becomes unlikely for programs with quick handlers,
  as the refutation predicted; a handler that takes time still opens it, and the
  sentence stays needed.
- **F2, the handlers' thread: the sentence is in** (*documented*; `abort.c` unchanged
  since `08b3da0`, so the behaviour the previous round measured stands). README
  :1091-1095.
- **M2, "usually": in, and fair** (*measured*): in the same 40 runs, 114 of the 160
  callers ran their handler, at least one in every run.
- **M4, the runbook's "What to expect": fixed** (*documented*, `docs/pi-cluster.md`
  §8): the examples' four, the active sets, `global_exit`'s 99 and
  `shmem_team_b2b_collectives` now say what the previous round measured.
- **M1, M3, M5**: in the release checklist's "After the release" list, as the
  synthesis decided (*documented*).
- **C5, silent corruption on undefined input: the `shmem_malloc` half is in the README
  (:672-675) and true** (*measured*, `malloc-mismatch.c`: the second object at
  offsets 64 and 4,096, a put to it landing inside the other PE's first object, exit
  0); **the `iput` half is in neither the README nor the checklist** (M2).
- **C6**: `team.c`'s stale header comment went with step D's rewrite (*observed in
  source*); the unlocked read of the context pool is unchanged and is F1.
- **The previous rounds' probes, rebuilt and rerun on this tree, behave as they did**
  (*measured*, 62 runs, and 32 of the undefined inputs): `lifecycle`, `ptr-access`
  (with `OSOE_FORCE_REMOTE`), `memory`, `nobarrier`, `ordering` with split wires,
  `amo-atomicity`, `lock-order`,
  `sessions-deprecated`, `profile-probe`, Example 37 and an in-place complex inscan,
  `shared-ptr`, `shared-split`, `split2d` (xrange 1, 2, 3, 5, 6, 10, 100, and 3 over
  seven), `threads-teams` in both modes on two maps, `thread-exit`, `threads-alive`,
  the sixteen behaviours of `invalid.c` on four maps, `limits` (63 and 4,095
  contexts), `signal-atomic` on four maps, `signal-c11`, `ibstride` with split wires,
  `heapfull`, `mt-quiet4`, and the undefined inputs of `argcheck` and `mismatch` -
  named where they were named, silent where the README says they are. One differs, by
  design: `teams-exhaust`, which splits the world and destroys each team, made 5,001
  teams in each of two sessions (the probe's cap), at the default and at
  `OSOE_MAX_TEAMS=4`, where the previous round stopped at 61 and at 1: a number given
  back is taken again.

## Step D, read against §9.4 and run

Read: §9.4's preamble (predefined and application-defined teams, handles, thread
safety, collective ordering, team creation), §9.4.1 to §9.4.8, §9.5.2 to §9.5.4, §9.10's
preamble and the argument rules of §9.10.5 to §9.10.10, §9.2, §9.1.4 and §9.1.6;
`team.c` whole, `context.c` whole, `collectives.c`'s split reduction, `node.c`'s
block life, and the diff of every other file the step changed, for anything a
program can see.

### What §9.4 requires, and what the tree does

Each requirement with the probe that measured it (all in the scratchpad, Sources);
"0 failures" means every PE checked every result and found none wrong.

1. **A split that cannot be made returns nonzero on every parent PE with
   `SHMEM_TEAM_INVALID`** (§9.4.6), **and a `split_2d` that cannot make one of its
   teams makes neither, on every PE** (§9.4.7) (*measured*, `teams-alive.c`; each
   status compared over the world by a max and min reduction). On `0,0,1,1`,
   `0,1,2,3`, `0,0,0,0` and `0,1,0,1`: 61 teams of the world held at once, the 62nd
   split nonzero on every PE with the handle `SHMEM_TEAM_INVALID`; one destroyed, a
   `split_2d` (two numbers) failing with both handles invalid everywhere and a
   strided split succeeding on the one number left, under a handle unequal to the
   destroyed team's; 200 rounds of a failing strided split and a failing `split_2d`,
   then two teams destroyed and a `split_2d` succeeding, its coordinates right; every
   team destroyed and 61 made again - nothing leaked. One line on standard error for
   each failed split, by the parent's rank 0 (403 lines for 403 failures). At
   `OSOE_MAX_TEAMS=16`, 13; at 1,024, 1,021 - with `OSOE_TEAM_BLOCKS=4096`: at the
   default pool of 1,024 the `split_2d` with two numbers free fails for want of blocks
   instead (a row and two columns need three blocks a node, two are free), which is
   the README's second team entry, not a defect.
2. **A destroyed team's number comes back, and teams with no PE in common may share
   one** (D1; nothing in §9.4 forbids either) (*measured*, `teams-disjoint.c`): every
   round the evens and the odds of the world, then the pairs `{2j, 2j+1}`, then a
   `split_2d`'s rows of three and its columns, each PE in one of each; on every team a
   sum reduction, a 256 KiB max reduction (several pieces), a broadcast from a moving
   root, `fcollect`, `collect` of unequal lengths, `alltoall`, an inclusive and an
   exclusive scan and a sync; then the four destroyed in an order unlike their
   creation's. Ten rounds on `0,0,0,0,0,0,0,0`, `0,0,1,1,2,2,3,3`, `0,1,0,1,0,1,0,1`,
   `0,1,2,0,1,2,0,1` and twelve PEs on four nodes, and five more on
   `0,1,2,0,1,2,0,1` with 1 KiB chunks and split wires: 0 failures. On the
   interleaved maps a node holds blocks of several pairs, several rows, and the
   evens beside the odds, each set under one number with different leads.
3. **"Upon completion of a team creation operation, the parent and any resulting
   child teams will be immediately usable ... including creating new child teams,
   without any intervening synchronization"** (§9.4) (*measured*): the
   specification's Example 12, which splits the world, splits a child at once,
   destroys the intermediate team and then synchronizes over the world, printed
   exactly the specification's expected output, in its order, on twelve PEs on one
   node, four nodes of three, an interleaved map of four and twelve nodes of one.
   Examples 10 (translation) and 11 (a strided split) exit 0 on three maps of five
   PEs. `teams-disjoint.c` splits, computes and destroys with no synchronization of
   its own between the steps.
4. **Destroy** (§9.4.8) (*measured*, `handles.c`): a team's shareable context goes
   with it; a team made next, under the destroyed team's number, has another handle;
   `shmem_ctx_get_team` answers the new team's handle for a context of it, and a
   team-relative put on that context reaches the right PE, on three maps. What §9.4.8
   leaves undefined is named: the destroyed handle used once its number names another
   team ends the job with "0x12f872d8 is team 3, which this PE has destroyed; the
   number names another team since", and a team destroyed with a live private context
   with "created from team 3 with SHMEM_CTX_PRIVATE, is still live; spec §9.4.8
   requires the program to destroy it first".
5. **Thread safety** (§9.4: a team may be used concurrently in non-collective
   operations, collectives on different teams may run in parallel; §9.2 item 4)
   (*measured*, `teams-mt.c`): under `SHMEM_THREAD_MULTIPLE`, four threads a PE,
   each owning another parent - the world, the world reversed, a half, the node's
   shared team - split it 30 times (whole, every other member, `split_2d`), reduce
   and broadcast on the children and destroy them, all at once, while a fifth thread
   asks `team_my_pe`, `team_n_pes`, `translate_pe` and `get_config` of the reversed
   team, which the second thread is splitting: 0 failures on `0,0,0,0`, `0,0,1,1`,
   `0,1,2,3`, `0,1,0,1` and two nodes of three, and with `OSOE_TEST_PERTURB=1`; no
   split failed, as README :510-517 promises ("neither fails for it"). Under the tsan
   preset's library, on `0,1` and `0,1,2,3`: 0 failures and **no ThreadSanitizer
   report**.
6. **Teams on every node at once** (*measured*, `split-shared.c`): a split of
   `SHMEM_TEAM_SHARED`, whole, on each of 100 logical nodes of one PE: every node's
   team valid and computing; 61 such splits kept at once on 70 nodes of one and on 16
   nodes of four, every team valid; the 62nd failing on every node, one line a node,
   by each node's split's rank 0.
7. **The SOS team programs at 70 PEs on this tree** (*measured*; one node, ten nodes
   of seven in blocks, and ten nodes cyclically): `shmem_team_reuse_teams`,
   `shmem_team_split_2d`, `shmem_team_shared`, `shmem_team_translate`,
   `shmem_team_negative_stride` and `collect_active_set` exit 0 with no split
   failing; `shmem_team_max` makes 64 teams for PE 0, three splits failing on the way
   as the record describes; `reduce_active_set`, `broadcast_active_set` and
   `repeated_barriers` each have nine splits fail and skip those rounds - the README's
   temporary limit (:814-818), since their last PE would be in 70 teams;
   `shmem_team_reduce` refuses 70 PEs itself ("Requires less than 32 PEs"). The
   record's acceptance table holds on the merged tree, line for line.

### The README's team entries, checked

- **"A PE is in at most `OSOE_MAX_TEAMS` teams at once ... so a program can hold 61
  teams of its own on every PE ... With no number free on every member a split
  returns nonzero on every PE of its parent team ... and the parent's rank 0 says
  why"** (:484-497): true (items 1 and 6). The rule it states - one number free on
  every member - has a consequence it does not: M1.
- **"A node holds at most `OSOE_TEAM_BLOCKS` teams at once ..."** (:498-509): true in
  every clause (*measured*, `teams-blocks.c`, `OSOE_TEAM_BLOCKS=8`, four PEs on
  `0,0,0,0`, `0,0,1,1`, `0,1,0,1`): five teams fill a node's five free blocks and the
  sixth split fails on every PE, said by the would-be lead of each dry node; a team
  destroyed by the PE that leads the next one, or by its members all inside the
  split's parent, gives its block to a split made at once (the split's busy rounds
  wait for the last member to let go); a hundred dry failures leak no block. The
  documented exception happens as documented: a team `{2,3}` whose members, outside
  the parent `{0,1}`, destroy it 300 ms late makes the split over `{0,1}` fail on
  `0,0,0,0` and `0,1,0,1` (on `0,0,1,1` the pools are other nodes'), and the same
  split succeeds once they have.
- **"Two splits of different parents at once, on threads of one PE ... neither fails
  for it"** (:510-517): true (item 5); the split's waits on another split are
  internal and short.
- **The `OSOE_MAX_TEAMS` and `OSOE_TEAM_BLOCKS` rows and `SHMEM_INFO`'s text**: say
  the same (*measured*, `SHMEM_INFO=1`).
- **"At 70 PEs, programs that make a team a round and keep them all ... skip their
  last nine rounds"** (:814-818): true (item 7).

### Broken on purpose

- **A destroy that does not give the number back** (the line that clears the held
  bit, `team.c:1493`, removed in a copy): eight of the suite's 48 team tests fail -
  `shmem.team-split.exhaust.*`, `.too-many`, `.reuse.*` (three maps),
  `.stale-handle` and `tools.pacer.teams-exhaust.0-0-1-1` - and the probes see it:
  `teams-alive` fails its split on the given-back number, `teams-exhaust` stops at 61.
  The suite holds step D's central promise.
- **A number taken 2^32 - 1 times**: F2's evidence.

## New findings

### F1. Under `SHMEM_THREAD_MULTIPLE`, a team's destroy reads the context pool unlocked while another thread creates or destroys contexts (fix before release; confidence high on the race, high that no wrong result follows in a valid program)

- **Location**: `shmem/src/context.c:263-275`, `osoe_context_team_destroyed`, whose
  loop reads `c->live` and `c->descriptor` (:267) with no lock, while `create`
  (:180-197, the element written at :189) and `destroy` (:218-224, at :222) write
  them under `pool_mutex`; called by `pshmem_team_destroy` (`team.c:1483`). Unchanged
  since `08b3da0`, where it was the previous round's C6, from the source alone.
- **Scenario**: a `SHMEM_THREAD_MULTIPLE` program in which one thread makes and
  destroys teams - each destroy destroying the team's shareable contexts, §9.4.8 -
  while another thread creates and destroys contexts of its own, which §9.5.1 makes
  non-collective. Both are valid at once (§9.2 item 4).
- **Evidence** (*measured*, `ctx-mt.c`, the tsan preset's library, two PEs on two
  logical nodes, which is how the suite runs tsan: one PE a node, every writer in the
  process tsan sees): one thread splits the world, creates a context on the new team
  and destroys the team, 200 times; the other creates a private context of the world,
  puts, quiets and destroys it, 1,000 times. Two runs: 3 and 2 data-race reports,
  status 66 (tsan's), the program's own checks clean. Every report pairs a read in
  `osoe_context_team_destroyed` (`context.c:267`, sizes 1 and 8: `live` and
  `descriptor`) with a write in `create` (:189) or `destroy` (:222) holding the mutex.
  `teams-mt.c` - the same threading without contexts - gives none.
- **Why it matters**: the README provides `SHMEM_THREAD_MULTIPLE`, where §9.2 item 4
  makes every call thread-safe; a C11 data race is undefined behaviour, and design
  §3.16 asks every shared word to be atomic or locked. The practical cost is the
  report: a user who checks a threaded program under ThreadSanitizer - the ordinary
  check - is shown a race inside osoe. No wrong result was shown or argued: in a
  valid program no context of the team being destroyed is being created at that
  moment, and every earlier destroy of one happens before the team's destroy by the
  program's own ordering, so the compare at :267 fails for every element another
  thread is writing. Step D is why it matters more now than when C6 was rated:
  numbers are given back so that programs may make and destroy teams freely.
- **Fix**: take `pool_mutex` for the scan - collect the matching elements under it,
  then destroy them outside it, since `destroy` takes the mutex itself and runs the
  context's quiet. Test: `ctx-mt.c` as a tsan test on `0,1`, which fails today.

### F2. A PE that takes one team number 2^32 - 1 times ends the job at its next split (after release; confidence high)

- **Location**: `shmem/src/team.c:1069-1075` (`finish`: "this PE has taken team
  number %u %u times, every generation a handle can name"), the descriptor's
  `uint32_t generation` (`team.h:145`), `handle_of` and `created_number`
  (`team.c:573-591`).
- **Scenario**: a program that splits and destroys a team in a loop - a library that
  makes a team per call, the case step D was built for (proposal §0) - takes the same
  lowest free number every time, and each take raises that number's generation on
  the PE by one. At 4,294,967,295 takes the next split of the number ends the job.
- **Evidence** (*measured*): in a mutant whose generations start at `UINT32_MAX - 3`,
  runs of two and three split-and-destroy cycles pass, and runs of four and five end
  the job at the fourth split, the run of five printing "this PE has taken team
  number 3 4294967295 times, every generation a handle can name". How long a real
  program takes to get there, by
  `cycle.c` (split of the world and destroy, back to back): 0.8 µs a cycle on one PE -
  2^32 cycles in about an hour; 52 µs on two PEs of one node (62 hours); 114 µs on
  four (136 hours); 260 µs across two nodes (310 hours). A program with work between
  its splits takes proportionally longer; a one-PE job, or one PE a node splitting its
  own `SHMEM_TEAM_SHARED`, is the fast case.
- **Why it matters**: §9.4.6 lets a split that cannot be made return nonzero; nothing
  lets a valid program be ended, and the README lists every limit a program can meet
  and not this one. The fatal keeps the stale-handle diagnostic exact - for programs
  that use a destroyed handle, which §9.4.8 leaves undefined - at the cost of a
  program that does nothing wrong.
- **Fix**: drop the fatal and let the generation wrap: `handle_of` and
  `created_number` stay right in unsigned arithmetic, and only a handle exactly 2^32
  takes stale would be taken for the live team - a program already outside the
  specification. Or a 64-bit generation, which the handle's arithmetic has room for
  up to about 2^45 takes at the largest pool (a descriptor is 152 bytes, at most
  1,021 of them, and the sum must stay below 2^63) - near a year of the fastest loop
  measured here. Or, at the least, a README line.

### Minor

- **M1. A split can fail when every member is in one team of its own** (*measured*,
  `teams-fragment.c`; `team.c:1118-1133`, `:1316-1320`, `:1245-1253`; README
  :484-497). A split takes a number free on all of its new team's members at once, so
  what fails it is the union of the numbers they hold, not any one PE's count. On 61
  logical nodes of one PE, each PE in exactly one team of its own, under 61 different
  numbers, a split of the world returns nonzero on every PE, saying "no team number is
  free on every PE of the new team: OSOE_MAX_TEAMS is 64, the teams one PE may be in
  at once"; with 60 nodes it succeeds. Two PEs each in 31 teams, under numbers that
  cover all 61 between them, cannot make a team of the two. The README states the
  rule but leads with a count ("so a program can hold 61 teams of its own on every
  PE"), and the message points at a count no PE has reached. It is the prior art's
  behaviour - Sandia's `psync` pool and MPICH's context ids are chosen the same way
  (*recalled*, proposal §2) - it needs PEs with different histories of making and
  destroying teams, and step E's 256 numbers make it rarer. *Fix*: one README
  sentence - a split can fail before any member holds 61 teams, when its members
  together hold every number - and the message saying "the new team's members
  together hold every number".
- **M2. The `iput` half of the previous rounds' C5 dropped out of every list**
  (*measured* again: `iput` with a destination stride of 0 lands only the last
  element, and with -1 writes the elements backwards from `dest` - into the
  allocation before it when `dest` is an object's start, as the previous round
  measured - each with exit 0, where `ibput`'s stride below `bsize` is named; §9.6.1.3
  asks a stride of at least 1). The synthesis kept "C5's two silent cases,
  as rated before" after release; the fixes put the `shmem_malloc` half in the README,
  and the release checklist's "After the release" list names M1, M3 and M5 but not
  this. A line in either.
- **M3. The README counts 24 model tests; the tree has 28** (*documented*, README
  :119 and :782; the merge point's models preset ran 28 of 28), and its list of the
  modelled protocols (:786-790) leaves out step D's four: a block's life, the posted
  bitmap, a split's numbers under `SHMEM_THREAD_MULTIPLE`, and a dry pool. The README
  is meant to be true at every commit.
- **M4. Design §10 still lists "Thorough 1.6 spec read" as blocking**, naming the
  session routines and "the routines beyond phase 1". Two rounds have now read the C
  interface whole against the code - the previous round §9.1 to §9.13, §8, §10 and
  Annexes C and F, this one §9.4 and §9.5 again after their rewrite and §9.10's
  argument rules - and the sessions are accepted no-ops, which §9.9.2 permits. The
  author's to close.

### Refuted, or no finding

- **The 1,024-number run's failing `split_2d`** was the pool, not the numbers (item
  1), as the README's second team entry says.
- **`teams-blocks.c`'s first runs reported failures** on PEs 2 and 3 - the probe
  checked a status those PEs never computed. Fixed, five runs clean.
- **`shmem_team_reduce` at 70 PEs** is SOS's own limit of 32 PEs.
- **A split prints a line on standard error every time it fails** - 403 lines for
  403 failures, one by the parent's rank 0 each - which a program probing for the limit
  in a loop will show. The README says the parent's rank 0 says why; a choice.
- **A block's generation in the node segment is 32 bits and wraps without a word**
  (`node.h:730`), unlike the handle's. The engine compares it for equality with the
  generation it learnt, so a wrap could hide a new team only if exactly 2^31 uses of
  one block by teams without an internode phase came between two internode teams' on
  it. No program does that; noted, not a finding.
- **E1 and a broadcast's count**: §9.10.7 requires the same `dest`, `source` and
  `PE_root` of every PE, not in words the same `nelems` (below); not a finding against
  D, which classifies nothing.

## The SOS suite as evidence, and what no test exercises

**The merge point's sweep** (`build/scratch/merge-d/sos/`, *documented*, the lead's
run on `d8a651d`): the whole suite at 4 × 2 in the VM, the two automake variants
built, 161 programs exit 0, the two that do not build and the two failing runs the
README's four (`shmem_team_b2b_collectives` stopped by name, `shmemalign` refused by
name); the 26 team programs clean on `0,1` and `0,0,1,1` but for
`shmem_team_b2b_collectives`'s two runs. And at 70 PEs on this tree, item 7 above.

**What no SOS program exercises in §9.4** (*observed in source*, the corpus's
`tests-sos`): the all-or-nothing rule of a failing `split_2d` (`unit/shmem_team_split_2d.c`
checks only the success branch); a destroy destroying the team's shareable contexts
(`spec-example/shmem_team_context.c` destroys its contexts first); splits, or
collectives on different teams, from several threads at once (no program splits under
`SHMEM_THREAD_MULTIPLE`); a split failing alike for want of resources on every PE
(`shmem_team_max` reports PE 0's count only). osoe's suite covers each -
`shmem.team-split.*`'s modes, `shmem.team-threads.*`, `shmem.team-levels.*`,
`tools.pacer.teams-exhaust`, which holds 61 at the default - and so do this round's
probes. The previous round's list for the rest of 1.6 stands: SOS predates it.

**Requirements no test in any suite exercises**, after step D:

1. A team destroyed on one thread while another makes contexts (F1): no test; the
   probe fails under tsan today.
2. A number taken 2^32 times (F2).
3. A split failing for numbers spread over its members, none at its count (M1).
4. Per-node splits of `SHMEM_TEAM_SHARED` past 61 nodes: in the step's acceptance
   runs and here (100 nodes), in no ctest.
5. From the previous round, still standing: several callers of the global exit on more
   than one node under a killing launcher; which thread runs the handlers; data
   collectives from several threads on different teams at once (`shmem.team-threads`
   runs two splits at once and checks each team; this round's `teams-mt.c` and the
   previous rounds' `threads-teams.c` run the collectives); fence ordering across nodes
   as a third PE sees it; `SHMEM_DEBUG` and the `SMA_` spellings of the other three;
   `iput` strides below 1 (M2); a fractional heap filled whole.

## Step E, read for conformance (kept apart)

Read: the proposal's §4 and §6 to §7, against §9.4's thread-safety paragraph and
§9.10's argument rules. **Nothing found that E would get wrong against the text.**

- **E1's class rule** - every lead classifies a collective's bytes alike - relies on
  every PE passing the same `nelems`. The specification says so in words for
  `alltoall` (§9.10.5), `alltoalls` (§9.10.6) and `fcollect` (§9.10.8); for reductions
  (§9.10.9), broadcasts (§9.10.7) and scans (§9.10.10) only through "the same dest and
  source arrays" (a broadcast: "data objects ... and the same value of PE_root"),
  `nelems` being "the number of elements in" them - another count is undefined by
  omission, as implementations assume (*recalled*). A program that passes another
  count is outside the specification either way: between nodes E's grant check names
  it, and within a node the lead classifies alone, so the README's Failures entry on
  collectives a node's PEs call differently stays true after E.
- **E10's default of 256** ends the nine skipped rounds at 70 PEs, but the programs
  that keep a team a round skip again past 253 PEs: README :814-818 should then say
  past what size, not "temporary".
- **F2 and M1 are E's too**: E changes neither the generation nor how a number is
  chosen; M1 gets rarer at 256 numbers, F2 does not move.

## What is sound, and what convinced me

- **The rebuilt team API against §9.4**: every failure alike on every parent PE with
  invalid handles, for numbers and for blocks; `split_2d`'s all-or-nothing rule; numbers
  and blocks given back with nothing leaked over hundreds of failures and thousands of
  cycles; teams sharing numbers computing every collective right on five maps and
  under split wires; the specification's Examples 10 to 12 as printed; destroy's
  contexts and the undefined cases named. What convinced me is that the statuses were
  compared across every PE
  by a reduction, every result checked, on maps that put several same-numbered teams
  on one node.
- **`SHMEM_THREAD_MULTIPLE` for teams**: four concurrent splits of overlapping parents
  a PE with queries beside them, no failure on six configurations and no tsan report;
  the pending protocol's design (no wait closes a cycle; waits only for higher parent
  numbers) read in `team.c:1145-1217`, beside the record's model.
- **The README's three team entries**: each clause measured true, the documented
  exception included.
- **The suite sees D's central promise**: a destroy that keeps its number fails eight
  team tests.
- **The previous round's fixes hold**, and its probes behave as before; the SOS sweep
  at the merge point gives the README's four exceptions and nothing new.

## Sources

The specification (`spec-1.6.txt`): §9.1.4, §9.1.6, §9.2, §9.4 (preamble, §9.4.1 to
§9.4.8), §9.5.2 to §9.5.4, §9.9.2, §9.10 preamble, §9.10.5 to §9.10.10. The tree at
`d8a651d`: `README.md`; `shmem/src/team.c`, `team.h`, `context.c`, `collectives.c`,
`node.c`, `node.h`, `active-set.c`, `progress-host.c`, `init.c`, `barrier.c`, and the
diff `08b3da0..d8a651d`; `shmem/include/shmem.h`; `shmem/tests/CMakeLists.txt`;
`tools/modelcheck/CMakeLists.txt`; `tools/pacer/CMakeLists.txt`; design §5.7 and §10;
the proposal, the brief, the two step D records and the review fixes' record;
`docs/release-checklist.md`; `docs/pi-cluster.md` §8; the merge point's
`build/scratch/merge-d/main/verify.txt` and `build/scratch/merge-d/sos/`; the step's
`build/scratch/teams-d/acceptance/`; the corpus's `tests-sos`. This round's probes,
scripts and results, in this session's scratchpad,
`<scratchpad>/conf-d/vm/`, a scratch directory outside the repository:
`p/` (`teams-alive.c`, `teams-fragment.c`, `teams-disjoint.c`, `teams-mt.c`,
`teams-blocks.c`, `split-shared.c`, `handles.c`, `cycle.c`, `ctx-mt.c`,
`example10.c`, `example11.c`, `example12.c` with `example12.expected`, `mutate.sh`
and the `run-*.sh` that ran them), and the results `prev-results.txt`,
`new1-results.txt`, `new2-results.txt`, `blocks-results.txt`, `shared-results.txt`,
`frag-results.txt`, `acc-results.txt`, `undef-results.txt`, `ge-results.txt`,
`cycle-results.txt`, `detail-results.txt`, `tsan-results.txt`,
`tsan-ctx-results.txt`, `mut-no-give-back.txt`, `mut-generation.txt`; the previous
rounds' probes beside them, `../../probes/`. A refutation run rebuilds the tree in a
VM (19 s for `release`) and the probes with its `oshcc`.
