# Round four: synthesis - osoe after the release fixes

Review of `main` at `08b3da0`, run 2026-10-03 against
`docs/reviews/adversarial-review-prompt.md`, at the first of the three merge
points of the first release's task (`docs/prompts/task-first-release.md`): the
tree the release fixes left (`docs/decisions/2026-10-02-release-fixes.md`,
Parts 1 to 6). Five investigators, one a question, each holding the previous
round's findings as claims to check; then a refutation pass, three fresh agents
given every finding rated *fix before release* to reproduce, explain away or
find permitted; then this synthesis, by the lead. The team redesign's step D was
being built on its own branch while the review ran and is not reviewed here:
each investigator kept what it noticed of the proposal and the brief in a
section of its own, which the step's own reads take up.

The five deliverables: `phase1-release-fixes-conformance.md`,
`phase1-release-fixes-correctness.md`, `phase1-release-fixes-scale-and-performance.md`,
`phase1-release-fixes-portability.md` and `phase1-release-fixes-readiness.md`.

## Verdict

**Release after named changes - one of them code, a few lines; the rest
sentences, a recipe and one decision. The release fixes did what their record
says, and the previous round's findings stay fixed under runs made to break
them.** Every defined behaviour of OpenSHMEM 1.6 the conformance read checked is
implemented as the text says, and the previous round's two gaps are closed. The
global exit and the start-up as rebuilt are sound within the limits they state;
the global exit's model's eighteen broken variants each fail by the property
they name; the suite passes whole idle and under eight busy loops. Every row of the scaling
table matches the code and, to the 254 logical nodes one machine holds, the
measurement. A build for x86-64 passes every test Rosetta can run, and a build
whose every atomic is load-exclusive/store-exclusive passes the suite and three
minutes of the stress tester's `mix`. Every refusal at start-up now ends the
refusing node by name, and every leftover of a failed start under Slurm is
collected.

What must change before the tag: a check against leftovers that covers one of
the three objects a node-mate opens by name; a README recipe that fails when it
is run again after a failed start; a setting and two platforms the README
describes wrongly or not at all; three sentences about the global exit and the
waits; and which history to publish, which only the author can decide.

## What must precede release, ranked

Nothing was rated *blocks release*. After the refutation pass, which confirmed
every one of these and corrected most (below), the *fix before release* set,
in the order the lead would do them:

1. **A start by the same ids binds the job tables and the connection area a
   failed start left** (correctness F1; `job.c:209-265`, `connection-area.c:278-445`;
   *measured*, and again by the refutation). Part 3 taught the control segment
   to tell a leftover of another launch; the job tables and the connection area,
   which a non-lead also opens by name once they are sized and `ready`, have no
   such check. With both left by a cancelled start, a start by the same ids ended
   at 11 s as a false deadlock, or stayed silent with the detector off; with the
   tables alone it was refused with a message about the tables' name. And the
   failing start leaves its own ready leftovers, so every later start by the same
   ids meets them, until `osoe-stat --gc` or the epilog removes them. The
   conditions are rare - a Slurm requeue onto the same node index without the
   README's epilog, a hand-set id reused after a failed start - and the end is
   loud. *Fix*: a non-lead accepts the job tables or the connection area only if
   its creator is the creator of the control segment it has already accepted -
   the node's lead of this launch - and otherwise waits for the lead to reclaim
   it, as it waits for an object not yet ready; the three headers record their
   creator's pid already, so no layout changes. The refutation's other option, a
   word in the control segment the lead raises after each object's `ready`,
   needs a new layout; the pid comparison relies on a new lead not having the
   dead one's pid, which on the same node within a requeue is a coincidence of
   `pid_max`. Test: the refutation's case, leftovers from a cancelled stalled
   start and then a start by the same ids, which fails today.
2. **The README's launcher-free recipe fails when run again after a failed
   start** (readiness Q5-2; README :944-954; *measured* 8 of 10 by the
   investigator, 4 of 10 by the refutation, whose control with a new launch
   number passed 10 of 10). The recipe sets no `OSOE_LAUNCH`, so a re-run's
   non-leads cannot tell its control segment from a leftover. *Corrected*: the
   proposed `export OSOE_LAUNCH=$RANDOM` fails twice over - under dash, Debian's
   `sh` and the recipe's own code fence, `$RANDOM` is empty and every PE refuses
   it; and exported once, beside the job id, it does not change when only the
   start is run again, which failed 5 of 10. *Fix*: `export
   OSOE_LAUNCH=$(date +%s%N)` immediately before each start; the README's "a
   later start with the same ids takes over" conditioned on a new launch number;
   the `OSOE_LAUNCH` row saying a start that may follow a failed one by the same
   ids must set it.
3. **On RHEL 10 and its rebuilds, and in containers under their default
   seccomp profiles, io_uring is refused, and the README's first command then
   reports about 300 of 664 tests failed** (portability Q1; *measured* by
   refusing `io_uring_setup` as the sysctl does: 311 failed, 298 at `shmem_init`).
   *Corrected*: the README does state the sysctl for multi-node jobs (:81-83,
   :141-146), and on EL10 every failure's message is right; what is missing is
   the platforms' names, the size of the failure, and that the suite's
   multi-node tests need io_uring. Docker 25 and later and containerd refuse with
   `EPERM` - for which the message names the sysctl, which reads 0 inside the
   container - and Podman and CRI-O with `ENOSYS`. *Fix*: a README paragraph after
   the Building section's io_uring requirement. After release: name the sysctl in
   the `EPERM` hint only when it reads nonzero, and a seccomp filter otherwise.
4. **`OSOE_AM_HUGEPAGES=1` can never take effect** (portability Q2, readiness
   Q5-1; `connection-area.c:345-354`; *measured*, and confirmed against Linux
   6.12's `ksys_mmap_pgoff`, which refuses `MAP_HUGETLB` on a file that is not
   hugetlbfs before any page is looked for, with huge pages reserved or not).
   Every PE warns and maps ordinary pages. The step-2 review's F9 had found the
   same, and only the heaps' half was fixed then. *Corrected*: where `/dev/shm`
   is mounted `huge=always` or `within_size`, the buffers get transparent huge
   pages whatever the setting says, so the warning's "using ordinary pages" can
   itself be wrong. *Fix*: the README row, design §3.4's table and paragraph and
   §6.2 say the setting has no effect as built.
5. **Three sentences about the global exit and the waits**, each confirmed
   with a correction:
   - **Several callers on more than one node** (conformance F1; *measured*: a
     killing launcher killed PEs, callers among them, in 13 to 21 of 30 runs).
     The README says a PE of another node can be killed (:1066-1070), but as a
     coincidence costing only the status; it is the usual outcome when every PE
     detects the same failure, and the killed callers' handlers and output are
     lost. *Fix*: the refutation's sentence. The refutation also found the
     window's cause: the ending loop (`progress-host.c:603`, `:618`) sleeps a
     whole sweep after a poll that took the peer's end of stream without saying
     so, and two lines - sleep only if the condition still fails after the
     poll - cut the window from about 110 ms to under 35. A liveness fix worth
     taking now; it does not close the race, which a job-wide single carrier
     does, after release and modelled first.
   - **The caller's handlers run on the library's thread** (conformance F2):
     `atexit` handlers and C++ static destructors run on the agent thread, with
     signals blocked, so a handler reading thread-local storage - `errno` too -
     sees the agent's, and the caller's `thread_local` objects are not
     destroyed. Design §3.15 records the choice, and the specification names no
     thread (§9.1.6 asks for C's normal termination and makes other I/O
     implementation dependent): one README sentence.
   - **Every reopening of a connection's buffer wakes every writer waiting for
     it** (scale S1; `connection.c:117-122`): at the defaults a reopened buffer
     has room for one 64 KiB message, so the PEs of a node sending large
     messages to one node wake together and all but one sleep again. Confirmed
     in its counts; *corrected* in its cost: no run showed the node's rate
     falling, a bare wake-one was 10-25% slower at 4 and 16 writers - design
     §3.11's own reason for waking many - and the progress thread paid 1-8 µs a
     turnover at 32 writers, not the microbenchmark's figure. The README is not
     contradicted. *Fix*: extend its sentence at :670-672, and correct §3.11's
     example and the two comments that call the room divisible
     (`wait-word.h:24-27`, `transport-am.c:663`). The code - a writer that
     leaves room wakes the next, and a woken writer that fails passes the wake
     on - after release, `wait_word_model.py` first.
6. **Which history to publish** (readiness Q5-3): 512 of the repository's 526
   commits predate the scrub of host-specific material from the tree, and the
   release checklist does not ask what history is published. The author's
   decision, and one that cannot be taken back after the first push.

## After release, and minor

Each report ranks its own; the lead found none to promote. The minors that are
a sentence are taken with the sentences above; the rest go to the release
checklist's list of what follows the release.

- **Conformance**: the minors of its report; C5's two silent cases, as rated
  before.
- **Correctness**: F2, a lead that dies between making its segment and finishing
  it leaves its node-mates waiting; F3, the relayed-goodbye fix's test sees its
  defect one run in five; F4, `env.probe`'s reopen check races its own child;
  F5's small items.
- **Scale and performance**: S2, a large collective's window of two 16 KiB
  pieces an edge bounds it on links faster than gigabit (a sentence if wanted,
  and a measurement on such a link before the default moves); S3, the
  round-trip ratio the README quotes is the Pi's; no test runs past 16 logical
  nodes; `bootstrap.h`'s 160 MiB comment; the VM's stray process, since killed.
- **Portability**: Q3, the x86-64 network path has still never run (an hour on
  any x86-64 Debian 13 host closes it); Q4, root's `osoe-stat --gc` trusts
  objects any local user can write; Q5, a Clang user's first command fails
  `shmem.strict-c11`; Q6, a sandbox's refusal of futex2 reported as an old
  kernel; Q7, the 33rd start within a minute; Q8.
- **Readiness**: Q5-4, residues of the scrub; Q5-5, `osoe-stat` shows 64 nodes
  of a job on one machine; Q5-6, messages and sentences; Q5-7, one check above
  the seam that belongs below it.

## What is sound

- **Conformance** (*measured* and *observed in source*): no defined behaviour
  found implemented wrongly in what was re-read and probed - sixteen behaviours
  for invalid handles, the §8 size grammar to its own examples, the 1.6
  block-strided transfers no SOS program calls, signals with team-relative PEs,
  the team and context limits, `split_2d`'s `xrange` rules, a quiet completing
  another thread's puts as a third node sees it. The previous round's twenty-three
  probes behave as they did; the SOS suite gives the README's 159, and 161 of 165
  with its two automake variants.
- **Correctness**: the global exit, one status rule on every path and waits that
  form no cycle; its model's eighteen breakages each failing by its property and
  eleven passing cases passing with liveness; the "blocked in" record right by
  C11's argument, its test failing when its loads are made plain; the
  silent-peer gate tested, its test failing without it; the suite 664 of 664 idle
  and under eight busy loops; the debug and ThreadSanitizer builds of the newest
  tests clean.
- **Scale**: nothing grows with the job's PE count - every allocation of the
  changed code read, and footprints measured flat from 2 to 254 nodes but for
  the lead's 3.5 KB a peer; the start-up, the global exit and the deadlock proof
  at 64 to 254 logical nodes and at 127 and 200 PEs a node; the README's numbers
  against their records and raw files.
- **Portability**: no architecture in the library beyond a padding constant;
  the x86-64 and aarch64 builds export the same 3,595 symbols; kernel facilities
  refused by name and early; the trust fixes as stated; the 22 multi-node Slurm
  environments the Pi captured parse to their distributions' layouts; the
  install relocatable.
- **Readiness**: every death after init ends the whole job within about 0.1 s
  naming the culprit; four shapes of deadlock proved in 3.1-3.3 s at a 2 s
  setting, and no false end for a PE computing 12 s; a stopped node with 64 MiB
  in flight waited for, not declared dead; a global exit under Slurm returning
  the caller's status every time; the first command passing with the pins off
  and half the cores busy; the licence and the repository's content.

## Refuted, or corrected

No finding rated *fix before release* was refuted. The corrections that change
a fix or a claim:

- **Q5-2**: the proposed `$RANDOM` breaks the recipe under dash, and a value set
  once does not help the re-run it was for.
- **Q2 / Q5-1**: Linux refuses `MAP_HUGETLB` on the area's object, but does not
  refuse it huge pages: a `/dev/shm` mounted with `huge=` gives transparent huge
  pages regardless, so "the kernel refuses huge pages for a /dev/shm object"
  overstates, and the after-release idea of `MADV_HUGEPAGE` "where
  `shmem_enabled` allows it" names the wrong control - for `/dev/shm` it is the
  mount's `huge=` option.
- **Q1**: "nothing in the README to say why" overstates; the platforms' names and
  the failure's size are what is missing, and the hint misleads only under
  `EPERM` from a seccomp filter.
- **F1 (conformance)**: the README is not silent, only too narrow; and the
  ~110 ms window is a lost wake, which a two-line change cuts.
- **F2 (conformance)**: a recorded design choice, and no departure from the
  specification.
- **S1**: the herd's counts stand; its cost to the node's rate was not measured
  falling in any run, and the proposed README text claimed it.

## Sources

The five deliverables above, each with its runs and probes. The refutation
pass's reports, and its probes and scripts, are kept in
`build/scratch/release-fixes/review-r4/` (git-ignored): `rf1-f1.sh` and
`rf1-relaunch.sh` (correctness F1, readiness Q5-2), `rf2/` (`hugeprobe.c`,
`uring-errno.c`), `rf3/` (the global exit's probes, the instrumented `abort.c`
and `progress-host.c`, the herd's). The specification: `docs/reference/OpenSHMEM-1.6.pdf`,
local, and its text at `build/scratch/review-r3/spec-1.6.txt`.
