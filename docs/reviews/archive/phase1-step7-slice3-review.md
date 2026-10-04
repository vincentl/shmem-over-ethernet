# Round five: synthesis - osoe after the team redesign's step D

Review of `main` at `d8a651d`, run 2026-10-04 against
`docs/reviews/adversarial-review-prompt.md`, at the second of the three merge
points of the first release's task (`docs/prompts/task-first-release.md`): the
tree step D of the team redesign left, on the round-four review's fixes
(`docs/decisions/2026-10-03-team-storage.md`,
`2026-10-03-team-allocation.md`, `2026-10-04-review-fixes.md`,
`2026-10-04-step-d-merge-point.md`). Five investigators, one a question, each
holding the previous round's findings as claims to check and keeping what it saw
of step E apart; then a refutation pass, four fresh agents given every finding
rated *fix before release* to reproduce, explain away or find permitted; then
this synthesis, by the lead. A rate limit stopped all five investigators
mid-work; each was resumed with its context and finished.

The five deliverables: `phase1-step7-slice3-conformance.md`,
`phase1-step7-slice3-correctness.md`, `phase1-step7-slice3-scale-and-performance.md`,
`phase1-step7-slice3-portability.md` and `phase1-step7-slice3-readiness.md`.

## Verdict

**Release once step E is built, after the named changes below - every one of
them made in the fixes that follow this review. Nothing blocks.** Step D does
what its records say. Every behaviour §9.4 defines for creating, using and
destroying teams works on the rebuilt code, the specification's team examples
run as printed, and splits fail alike on every parent PE whichever of the two
limits runs out. Its four models reproduce their recorded state counts and
every listed breakage fails by its property; the suite passed whole under eight
busy loops, the newest team tests a hundred times each, and a four-thread split
stress ran clean, under ThreadSanitizer too. Nothing it added grows with the
job's PE count, a destroyed team gives back its memory as well as its number,
and on the Pi it moved no line beyond 1%. It ran under the two weaker memory
orderings it had never met - the Pi 5's `ldapr` and an LL/SC build - and on
x86-64 under Rosetta, where the one thing that did not travel was a test.

What the review found is of three kinds: a race the library had carried since
step 8 and two rounds had deferred, which a tsan build of the library shows;
a test whose checks depended on the compiler's evaluation order; and sentences -
of the README, of a message, of `osoe-stat` - that step D had left saying less
than the truth about how teams run out and how they are named. And one decision
for the author, which the author took: the default of the node's pool of team
blocks.

## What must precede release, ranked

Nothing was rated *blocks release*. After the refutation pass, which confirmed
two of the seven and narrowed five, the *fix before release* set, in the order
the lead took them:

1. **A team's destroy read the context pool without its mutex** (conformance
   F1; `context.c:262-275`; *measured*, and again by the refutation). Under
   `SHMEM_THREAD_MULTIPLE`, any team's destroy - of a team that never had a
   context too - raced any context create or destroy on another thread, and a
   ThreadSanitizer build of the library reported it in 20 of 21 runs. The
   refutation narrowed it: in a valid program no element another thread writes
   ever carries the destroyed team's descriptor, so no wrong result or crash
   follows, and a user's own tsan build against an ordinary libosoe cannot see
   it; what remains is formal undefined behaviour and a breach of design §3.16.
   It had been deferred twice (round two's N6, round four's C6) on the premise
   that the scan reads words claimed by compare-and-swap - which they are not.
   *Fixed*: the destroy's scan, and the last finalize's, hold the pool's mutex
   and drop it around each context they end; `shmem.context-team-destroy`, the
   refutation's valid program as a test, fails 3 of 3 under tsan without the
   lock and passes 3 of 3 with it.
2. **Step D's split tests read the team handle beside the call that writes it**
   (portability Q1; `test-team-split.c`; *measured*, confirmed). Eighteen calls
   pass `split(..., &made)` and `made` as arguments of one call, an order C11
   leaves unspecified: GCC for x86-64 reads the handle first, so five one-node
   tests fail there, and three multi-node ones would on an x86-64 kernel; on
   aarch64 they pass only by the compilers' choice. The refutation scanned the
   GIMPLE of all 507 translation units on both targets and found no other
   instance. *Fixed*: the checks take the split's status before its handle.
3. **"A program can hold 61 teams of its own on every PE"** (readiness Q5-1,
   conformance M1; README and `team.c`; *measured*, narrowed). A split takes a
   number free on every member of the new team, so what runs out is what the
   members hold between them: every pair of 33 PEs made a team in a double loop
   fails at a pair holding 31 and 30, and at 256 numbers every pair of 129 at
   127 and 126. The refutation narrowed it: the README already stated the rule,
   and the message its true cause and remedy; the order the teams are made in
   decides as much as the pattern - the same pairs made a round of disjoint
   pairs at a time reach 61 - and the choice is the proposal's D1 and D2, as
   Sandia's and MPICH's. What was missing was the consequence. *Fixed*: the
   README says no PE holds more than 61, and that what the members hold between
   them runs out, with both orders' numbers; the message says between them they
   hold every number a program may use, though each may hold fewer.
4. **`osoe-stat` labelled a created team by its number**, which step D made
   non-unique (readiness Q5-2; `stat.c`; *measured*, narrowed). On a grid's
   columns every node printed both teams as `4` in `--summary`, and its last
   column named a local id as `PE0`. The refutation narrowed it: the dump and
   the JSON already print each team's lead and its tree as world ranks, which
   tell the teams apart by hand - the reviewer's filter could not match them -
   and the walk down the tree still reached the culprit's node. *Fixed*: the
   metric table carries the team record's world ranks; the summary labels a
   created team by its number and its first member's world rank, `4@r1`, alike
   on every node and unique among live teams, and a blocked PE by its rank; the
   dump's heading gives the ranks; `shmem.team-storage` checks both, with two
   teams under one number.
5. **At 256 PEs a node the default pool of 1,024 team blocks binds long before a
   PE's numbers** (scale T1; *measured*, narrowed). A whole-job `split_2d` whose
   rows are a node long takes P + 1 of a node's blocks: three such grids fill
   the default pool at 256 PEs a node while each PE holds 9 of its 64 numbers.
   The refutation narrowed it: this is the reach the proposal's D3 chose, and the
   allocation record had measured the same count; what was missing was the
   README's sentence, and that step E, raising the numbers to 256, leaves the
   pool alone. *Put to the author* with three defaults - 1,024; one scaled with
   the node so that it never runs out first (about 63 MiB of `/dev/shm` at 256
   PEs a node after E); a middle - and *kept at 1,024*
   (`docs/decisions/2026-10-04.md` item 3). *Fixed*: the README states the
   reach and a block's cost.
6. **A node's PEs must share one PID namespace, and the README did not say so**
   (portability Q2; *measured*, narrowed). osoe watches a node's PEs, and judges
   what a failed start left, by process id. With a PID namespace for each PE -
   one container a rank under Docker or Podman, Apptainer's `--pid` - a PE
   killed by a signal went unnoticed and the others waited, in an emulation and
   in real containers alike; the deadlock detector and the stall watch do not
   catch it. Where `/dev/shm` outlives the namespaces (`--ipc=host`), a start
   killed with its containers left names `osoe-stat --gc` keeps for good and a
   later start by the same ids is refused, naming its own pid as their live
   creator - and the README's launcher-free recipe in a namespace was refused at
   every start after the first killed one. The refutation narrowed it: only a
   death by a signal is missed - an exit without `shmem_finalize`, a fatal, a
   global exit end the job across namespaces through the abort word; the hang
   needs equal pids, as identical containers give, and unequal ones end the job
   at once on a false death instead; `srun -K1` bounds the wait; and the
   reviewer's Apptainer `--containall` gives each PE a `/dev/shm` of its own, so
   that the PEs never meet and the start waits for ever. *Fixed*: a README
   paragraph says a node's PEs must share one PID namespace and one `/dev/shm`,
   what each separation does, and how to clear a namespace's leftover; the two
   promises of a dead PE ending the job say "provided"; and a leftover whose
   recorded creator is the starting process's own pid is refused as another
   namespace's, naming the `rm`. A start-up check that compares the namespaces is
   on the list of what follows the release.
7. **In a container's default `/dev/shm` even a one-PE job is refused**
   (portability Q3; *measured*, confirmed). Docker, Podman and Kubernetes' pods
   give a container 64 MiB of `/dev/shm`, less than one PE's default heap: the
   README's ring at one PE is refused at start-up, naming the sizes, and 495 of
   the 690 tests fail, 536 with io_uring refused as well - where the README's
   container paragraph said one-node jobs run and about 300 tests fail, which is
   right only of io_uring refused with an ample `/dev/shm` (324). *Fixed*: the
   paragraph gives both counts and the remedy - a `/dev/shm` of the container's
   own, about 300 MiB a PE (`--shm-size`), or a smaller heap, rather than the
   host's; the refusal names `--shm-size` and says "1 heap", not "1 heaps".

## After release, and minor

Taken with the fixes, each a sentence or a few lines: the split's two messages
written once a line, as a fatal is, so that several PEs' lines no longer
interleave (scale T3, readiness); step D's 37 `memory_order_relaxed` accesses
and its one implicit-order load, each found ordered by another word, made to
name the orders design §3.16 requires - and `shmem.memory-orders`, which fails
the suite on a C11 atomic call that names no order, or names one the project does
not use (correctness G2); the README's model
count and list, its team cost, three settings rows and the 70-PE programs'
nine rounds; the stale sentences step D left in three comments, five passages
of the design notes, the records' citation of the arrival scan, the runbook's
sizes and the docs index; a scratch path that named the account; the docs
index's note on the records' VM names and scratch paths.

On the release checklist's list of what follows the release: the release hold
of §3.8 has no test and no model (correctness G1); a PE's descriptor generation
for a number is 32 bits, ending the job after 2^32 - 1 takes (conformance F2,
scale T2); `osoe-stat`'s counters view keeps 64 PEs a node, and a split's
pending wait wants a blocked-record kind; a split called with other arguments
on one node is silent; a busy round spins a one-node parent; the shared 64-bit
atomics are not asserted lock-free, and D's models run under sequential
consistency only; a posted-bitmap check no breakage reaches; round two's `iput`
stride; and six of round four's minors the list had dropped. For the author:
design §10's "Thorough 1.6 spec read", still listed as blocking (conformance
M4).

For step E, in its brief: the grant line's zeroing as the only guard against an
earlier team's last grant, which a mutation must show; the 1 to 16 KiB band
where the grant's hop is not paid back, with the scale read's probe and its
baseline on D; the class rule's premise that every PE passes the same `nelems`;
and the numbers' reach at 256.

## What is sound

- **Conformance** (*measured*): failed splits alike on every parent PE whichever
  limit runs out, both handles of a failing `split_2d` invalid, hundreds of
  failures leaking nothing; teams with no PE in common sharing numbers, every
  collective on them right on five maps; the specification's Examples 10 to 12
  as printed; per-node splits on 100 nodes; four threads a PE splitting
  overlapping parents; SOS at 70 PEs line for line with the step's acceptance;
  a destroy that keeps its number failing eight team tests.
- **Correctness** (*measured* and by argument): the four models' state counts
  and breakages; the suite under load and the newest team tests a hundred times;
  the D7 stress, 1,204 splits a PE a run with lost rounds and no hang; the
  per-number zeroing's removal failing eleven tests; the posted bitmap sound
  under C11's orders, not only its model's; the late-message and late-write
  arguments against the engine's code; the previous round's fixes holding.
- **Scale** (*measured*): a split costing about a broadcast and a barrier from
  one node to 128 logical nodes, at both thread levels and at 1,024 numbers;
  nothing of D's growing with the PE count; memory flat over 40,000 team cycles;
  the per-node split at 254 logical nodes; D within 1% on the Pi.
- **Portability** (*measured*): D's protocols message passing and
  read-modify-writes, none of the store-buffering shape; its four models keeping
  every verdict under x86-TSO; the suite and the team tests under the Pi 5's
  `ldapr` and under LL/SC; the exports unchanged; no warning from GCC.
- **Readiness** (*measured*): every failure D added loud and named, alike on
  every parent PE; a split or destroy one PE never enters proved a deadlock in
  3.3 s; 2,000 renewals where the 62nd failed before D; the launcher-free recipe
  restarting cleanly 10 times of 10; the scrub clean but for one path.

## Refuted, or corrected

No finding rated *fix before release* was refuted outright. The corrections that
change a fix or a claim:

- **F1**: wider than stated - any team's destroy, not one with contexts - and
  narrower in consequence: no wrong result, and invisible to a user's own tsan
  build; the reviewer's probe relied on §9.4.8's reading of a team's contexts,
  which §9.5.2 contradicts, and the refutation's valid program races the same
  way. The smaller fix needs no buffer.
- **Portability Q1**: 18 calls, not 21; three of the eight failures inferred.
- **Q5-1**: the README already stated the rule; the reviewer's proposed "as few
  as half" itself overstated - only pairs have that bound.
- **Q5-2**: the dump and JSON already told the teams apart; the culprit's node
  was still reached; the fix needed the metric table, not only `stat.c`.
- **T1**: the reach was the proposal's choice and the record's measurement; the
  reviewer's "the column team is between the two" was wrong about its cost.
- **Portability Q2**: narrower - a death by a signal only, equal pids only, and
  bounded by `srun -K1` - and its Apptainer example wrong: `--containall` hides
  the PEs from each other, a different failure; and wider in one place: the
  launcher-free recipe in a namespace refused every start after a killed one.
- **Portability Q3**: confirmed, 495 failures where the reviewer counted 496, the
  one difference a timing-dependent test.

## Sources

The five deliverables, and the refutation pass's reports, kept in the session's
scratch tree: `refute-1/` (F1, portability Q1), `refute-2/` (Q5-1, T1),
`refute-3/` (Q5-2), `refute-4/` (portability Q2, Q3). The fixes and their
verification: `docs/decisions/2026-10-04-step-d-review-fixes.md`.
