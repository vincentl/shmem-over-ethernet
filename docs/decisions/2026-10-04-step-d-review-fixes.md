# 2026-10-04: the fixes of the round-five review (step D's merge point)

The round-five review ran on `main` at `d8a651d`, the tree step D left, as the
second of the three merge points of `docs/prompts/done/task-first-release.md`: five
investigators, a refutation pass and a synthesis,
`docs/reviews/archive/phase1-step7-slice3-review.md`, whose verdict is *release
once step E is built, after the named changes*. This record is those changes,
made on the branch `review-d-fixes` from `main` at `a866dc4`, by Claude under the
task's mandate - decide what a brief does not list, and record the decision with
its undo - but for one decision the author took (item 5).

## The findings rated fix before release, and what was done

1. **A team's destroy read the context pool unlocked** (conformance F1, as its
   refutation narrowed it). `osoe_context_team_destroyed` and
   `osoe_context_finalize` now read the pool under `pool_mutex` and drop it
   around each context they end, which `destroy` takes again; a context's
   implicit quiet runs without it, and `pool_mutex` is still never held with a
   context's own. `context.c`'s header comment, which still said the thread
   level stays at most `SERIALIZED`, now says what step 8 built. Test:
   `shmem.context-team-destroy` (`test-context-team-destroy.c`), the
   refutation's valid program - one thread splitting and destroying the world,
   first with no context on the team, then destroying the one it makes before
   the team; another making, using and destroying private contexts - at L1, L2
   and L4.
2. **The split tests' order of evaluation** (portability Q1, confirmed).
   `check_made` and `check_failed` are macros that take the split's status
   before the handle is read (the refutation's form).
3. **What a PE's numbers run out of** (readiness Q5-1, narrowed). The README's
   first team limit says no PE holds more than 61, and that what runs out is
   what a new team's members hold between them - teams that split one another or
   share no PE reach 61, teams that overlap otherwise can fail sooner, as the
   order they are made in has it, with the refutation's two measured orders; the
   split's message says the same of the PEs it failed for. The two tests that
   match the message (`shmem.team-split.exhaust`, `.too-many`) match the new one.
4. **`osoe-stat`'s team labels** (readiness Q5-2, narrowed). Four team-scope
   metrics name the record's world ranks - `team.record.members`,
   `.world_start`, `.world_stride` (signed) and `.strided` - with no layout
   change. `osoe-stat --summary` labels a created team `<number>@r<first rank>`,
   a team of `SHMEM_TEAM_SHARED`'s family by its local lead's rank, and a blocked
   PE by its world rank, and `--dump` a team of `SHMEM_TEAM_SHARED`'s family by its
   local lead's rank; `--dump`'s heading gives a created team's ranks ("of
   world ranks 3 to 0 by -1"). The metric names stay `team.<number>.*`, which the
   tests build. `shmem.team-storage` checks the headings and the labels, and on
   its maps of four PEs and more splits the even and the odd ranks, which agree
   one number, and checks the summary tells them apart. A README sentence.
5. **The pool's default at 256 PEs a node** (scale T1, narrowed). Put to the
   author with three defaults; the author kept 1,024
   (`docs/decisions/2026-10-04.md` item 3). The README's second team limit gives
   the reach - a whole-job grid with rows a node long takes P + 1 of a node's
   blocks, three filling the default pool at 256 PEs a node - and a block's cost,
   and step E's brief carries it to 256 numbers.
6. **A node's PEs in one PID namespace** (portability Q2, narrowed). A README
   paragraph after the container paragraph: a node's PEs must share one PID
   namespace and one `/dev/shm`, why, what each separation does - a death by a
   signal unnoticed, or PEs that never meet - and how to remove a namespace's
   leftover; the two promises of a dead PE ending the job say "provided the
   node's PEs share a PID namespace". `shm-object.c`: a leftover whose recorded
   creator is the starting process's own pid - which another namespace's
   repeating pids give, or more rarely a pid recycled here - is refused as another namespace's, with the `rm`, where it was refused
   as another live job's. The start-up check comparing namespaces is on the
   release checklist's list of what follows the release.
7. **A container's 64 MiB `/dev/shm`** (portability Q3, confirmed). The README's
   container paragraph says a container's default `/dev/shm` refuses even a
   one-PE job and about 540 tests fail there, gives the remedy, and counts the
   io_uring refusals as about 320; the capacity refusal names a container's
   `--shm-size` and its count of heaps in the singular where it is one.

## The minors taken

- **design §3.16's rule, enforced**: step D's 37 `memory_order_relaxed`
  accesses and its implicit-order `atomic_load` (correctness G2) name acquire for
  a load, release for a store, and seq_cst for the two read-modify-writes that
  had been written as a load and a store (the predefined teams' held bits, the
  pool's free bitmap at its creation); so does an older relaxed store, the
  credit's wake word at its creation (`reply.h`). The library holds no relaxed
  access now. `shmem.memory-orders` (`cmake/CheckMemoryOrders.cmake`, tier 1)
  fails the suite on a relaxed, consume or acq_rel order, or a C11 atomic call
  without `_explicit`, anywhere in `shmem/src` and `base`; shown to fail on a
  copy with a relaxed load and two implicit calls planted, and to pass
  OpenSHMEM's own `..._atomic_fetch_add` names. What it cannot see is an
  operator on an `_Atomic` object, which its comment says.
- **The split's messages written once a line** (scale T3, readiness), through
  `osoe_notice`, which formats into one buffer and writes once, as a fatal does.
- **The README**: the model count and list (28, step D's four among them; the
  models preset's time); the team cost row (the split's closing sync, a
  destroy's barrier); the `OSOE_LAUNCH` row (one value a start); the
  `OSOE_TEAM_BLOCKS` row (a block's `/dev/shm` and memory); the huge-pages row
  (a warning only where there are buffers); the 70-PE programs' nine rounds (the
  library's nine lines); "each PE that would lead a new team there saying why".
- **Text step D left stale**: `node.h`'s `team_count`, `bootstrap.h`'s two
  numbers, a test comment that numbers are job-wide; design §5.2, §5.7's
  opening and its first part's low-water mark, §6.2's list of the settings a
  node's PEs compare, §7.3's handle; the records' citation of the arrival scan
  (§3.10, not §3.12); the runbook's sizes; the docs index's pointer to what
  waits on the author, and its note on the records' VM names and scratch paths;
  a scratch path naming the account in round four's conformance report.
- **On the release checklist**: this round's after-release items, and six of
  round four's minors it had dropped. **In step E's brief**: the review's notes
  on E.

## Choices made without the author

- **The context pool's fix holds the mutex across the scan and drops it around
  each match** (the refutation's form), not the reviewer's collect-then-destroy,
  which needed room for up to 4,096 pointers. *Undo*: collect under the mutex,
  destroy outside it.
- **`osoe-stat`'s label is `<number>@r<first rank>`**: unique among live teams -
  two teams under one number share no PE - and the same on every node, where a
  label by the local lead's rank would differ node to node. *Undo*: the number
  alone, the dump's tree by hand.
- **§3.16 kept as it stands, and the code brought to it**, rather than a row
  added for an access ordered by another word: the rule's point is that every
  access shows its order, and acquire and release cost an ordered load or store
  each - nothing on x86-64, a few cycles on aarch64 - on paths a network round
  trip or a team's creation bounds; not measured. *Undo*: relaxed again at
  those sites, and a row in §3.16.

## Verification

On the branch, in the `osoe` VM, the host otherwise quiet
(`build/scratch/review-d/verify-fixes/`):

- **The four presets**, the floor pins on: debug 694 of 694, release 694 of
  694, asan 691 of 691, tsan 469 of 469 - the four new tests among them,
  `shmem.memory-orders` and `shmem.context-team-destroy` at L1, L2 and L4.
- **Tier 3**: 113 of 113.
- **The context pool's fix against its test** (`build/scratch/review-d/f1-check.*`,
  `osoe-x86`): `shmem.context-team-destroy` under the tsan preset three times
  with the fix, every run clean, and three times with the scan's lock taken out
  again, every run failing - ThreadSanitizer's data-race reports in `context.c`,
  18 in the first run - after all its work was done.
- **`shmem.memory-orders` against planted violations**: a copy of the library
  with a relaxed load in `barrier.c`, and one with an implicit `atomic_load` and
  an `atomic_fetch_add (x, 1)` in `base/order.c`, each failing the check, which names the file and what it found, where the tree passes it and OpenSHMEM's own `..._atomic_fetch_add`
  names are not flagged.
- **The affected tests while the fixes were made** (`fixes-quick-*`): the team,
  split, stat, reply-pool and leftover tests, 97 of 97 in debug, after the first
  run had shown the new `osoe-stat` checks failing where the ranks were missing.
- The models are unchanged by these fixes; the models preset passed 28 of 28 on
  their base (`2026-10-04-step-d-merge-point.md`).
- **An adversarial read** of the branch before its merge, by a fresh agent,
  found nothing wrong in the library's behaviour - the pool's lock, the orders,
  the messages, the labels and the test macros sound - and text that was:
  five comments still calling acquire loads relaxed, the README's third promise
  of a dead PE ending the job without the namespace's proviso, the runbook's
  segment size (about 2.1 MiB at four PEs; 1.6 was two PEs'), this record's
  count of tsan reports (18, not 36), the check's line numbers - `file(STRINGS)`
  splits a file at a non-ASCII byte and a list at brackets, so it now reads
  whole files and names the file and the match - and its test of `/tests/`
  against the absolute path, the index's and the synthesis's account of what
  the check covers, the README's suite count and which PEs say why a pool ran
  dry, both outcomes of each namespace case, and a stale region size in
  `node.c`. Each fixed; the check passes the tree and fails a planted copy by
  file, and the affected tests pass, 100 of 100. Left: the new test does not
  make a team's destroy end a context while the other thread runs - a §9.4.8
  mode would.
