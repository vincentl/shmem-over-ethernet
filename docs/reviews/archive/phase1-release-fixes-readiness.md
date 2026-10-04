# Round four, question 5: is osoe ready to be public?

Review of `main` at `08b3da0`, run 2026-10-03 against
`docs/reviews/adversarial-review-prompt.md`, question 5 only, at the merge
point after the release fixes (`docs/decisions/2026-10-02-release-fixes.md`,
Parts 1 to 6). The previous round's answer to this question is
`phase1-release-readiness.md` (tree `b0e0b53`). One investigator; two of its
checks were finished before this run was restarted and are used here after
verification: the README's settings against the code
(`build/scratch/release-fixes/review-briefs/readme-settings-check-08b3da0.md`)
and the repository's hygiene (`.../repository-hygiene-audit-08b3da0.md`).
Labels: *documented*, *observed in source*, *measured*, *recalled*.

Every run below was made in the `osoe` VM (Debian 13, Linux 6.12, aarch64,
four cores, Slurm 24.11 on one node), under its lock, on a release build of a
copy of the tree with the stranger's defaults (`-DOSOE_TEST_PINS` off),
installed to a prefix; the whole suite passed there, 664 of 664 at `-j4` in
125 s. About 115 jobs were run, most made to fail on purpose: 61 under
`osoe-launch`, almost all with `-k` (so that the library's own reaction shows,
not the launcher's), 15 under `srun` and `sbatch`, and 40 starts of the
README's launcher-free recipe. Scripts and raw outputs: see *Sources*.

## Verdict

**Ready to be public once the teams are built, after two README lines and one
decision; nothing in this question blocks release.** The previous round's
findings in this question's area were fixed as the record says, and the fixes
hold under runs made to break them: a global exit under `srun -K1` returned
the caller's status every time (8 of 8); every refusal at start-up - thirteen
cases, on every PE and on one, the `/dev/shm` refusal and a secret given to
one node among them - now ends the refusing node by name at once instead of
hanging it; every failed start's leftovers under Slurm were removed by
`osoe-stat --gc`; and the README's first command no longer checks the floor
test's machine-dependent pins, while the one pin it still checks held beside
half the cores busy. What is new is narrow:

1. **`OSOE_AM_HUGEPAGES=1` can never take effect** (Q5-1): the kernel refuses
   huge pages for a `/dev/shm` object, so every PE prints a warning and gets
   ordinary pages, where the README promises huge pages when there are some.
2. **The README's launcher-free recipe fails 8 times in 10 when re-run after a
   failed start** (Q5-2): the very race Part 3's adversarial read fixed for
   `srun` and `osoe-launch`, left open for the one path that sets no launch
   number - the README's own example.
3. **The history carries what the tree was scrubbed of** (Q5-3): 512 of the
   526 commits predate the scrub; the release checklist does not ask what
   history is published. A decision, and one that cannot be taken back after
   the first push.

The rest is minor: residues of the scrub, `osoe-stat` dropping nodes past 64
segments on one machine, a few messages that could say more, and sentences a
little looser than the code.

## Findings, ranked

### Q5-1. `OSOE_AM_HUGEPAGES=1` can never take effect (fix before release; high)

- **Location**: README "The transport's buffers", row `OSOE_AM_HUGEPAGES`
  ("map the node's connection buffers with huge pages, falling back, with a
  warning, when there are none"); `shmem/src/connection-area.c:345-354`;
  design §3.4 (the table row and "Huge pages are off by default") and §6.2
  ("`MAP_HUGETLB` is used only by the connection area, when
  `OSOE_AM_HUGEPAGES=1`").
- **What happens** (*observed in source*): the connection area is a
  `shm_open` object on `/dev/shm` (`connection-area.c:268`, `:293`;
  `shm-object.c:37`, `:85`), mapped `MAP_SHARED | MAP_HUGETLB` when the setting
  is 1. Linux refuses `MAP_HUGETLB` on a file that is not on hugetlbfs, with
  `EINVAL`, before it looks for a page (*recalled*: `ksys_mmap_pgoff`).
- **Evidence** (*measured*): a ten-line program (`huge.c`) mapping a 4 MiB
  `shm_open` object with `MAP_SHARED | MAP_HUGETLB` gets "Invalid argument";
  the same object maps without the flag; the control, an anonymous
  `MAP_HUGETLB` mapping, gets "Cannot allocate memory" (no pages reserved) - a
  different error, so `EINVAL` is the file's kind, not the pages' absence. A
  job of two logical nodes of two PEs with the setting at 1 ran and printed
  "OSOE_AM_HUGEPAGES=1 but the connection area could not be mapped with huge
  pages (Invalid argument); using ordinary pages" four times, once a PE (the
  print is not limited to the lead); one node printed nothing (no area). No
  test sets the variable to 1 (`shmem/tests/CMakeLists.txt:594` tests only its
  refusal).
- **Scenario**: an operator reserves huge pages, sets the variable, gets
  ordinary pages and a warning from every PE of every node, and reads in the
  README that the warning means there were none.
- **Fix**: strike the row and the design's two sentences, or say "has no
  effect today; a hugetlbfs mapping is designed and not built", as §6.2
  already says of the heaps. Minutes. Confidence: high.

### Q5-2. The README's launcher-free recipe, re-run after a failed start, fails 8 times in 10 (fix before release; high)

- **Location**: README "Starting PEs without a launcher" - the example exports
  `OSOE_JOB_ID=$$` and no `OSOE_LAUNCH`; the table says `OSOE_LAUNCH` is
  "optional, `0` when unset"; and below it, "a later start with the same ids
  takes over what a process now gone left there". `shmem/src/node.c:997-1031`
  (a non-lead's `open_control`), `:182-186` (`check_failed`).
- **What happens** (*documented*, the record's Part 3 item 8; the rest
  *observed in source*): Part 3's adversarial read found a failed start's
  leftover segment ending the next start by the same ids 4 times in 10, and
  fixed it with a launch number in the segment: a non-lead waits past a
  `ready` or `failed` segment whose launch differs from its own.
  `srun` varies the number (`SLURM_RESTART_COUNT`) and `osoe-launch` sets a
  random `OSOE_LAUNCH`. The recipe sets neither, so both starts carry launch 0,
  and a non-lead that opens the old segment before its lead reclaims it reads
  its `failed` word. `$$` is the same for every run typed into one shell.
- **Evidence** (*measured*, `relaunch.sh`): the recipe's commands in one
  shell; a first start in which PE 1 refuses `SHMEM_SYMMETRIC_SIZE=abc` leaves
  the segment, a heap and a statics object; the recipe run again with good
  values failed **8 times in 10**: PE 1 printed "another local PE failed during
  rendezvous" - nothing in that start had failed - and PE 0, having reclaimed
  the leftovers ("reclaimed /dev/shm/osoe....0.0.0, left behind by a process
  that is gone"), waited for local id 1 for good, saying so every interval.
  The control, the same script with a fresh `OSOE_LAUNCH` for the second start,
  passed **10 of 10**. (`shmem.start-up.same-ids` cannot see this: it starts
  through `osoe-launch`.)
- **Fix**: the recipe exports `OSOE_LAUNCH=$RANDOM` beside `OSOE_JOB_ID` (one
  value for every PE of a start), and the row says a hand-made start that
  reuses its ids must change it. One line. The launch number is what tells
  the two starts apart - a creator that is gone does not, since a lead that
  refused in this start has also left - so the fix belongs in the recipe, not
  in a heuristic in `open_control`. Confidence: high.

### Q5-3. The history publishes what the tree was scrubbed of (fix before release, as a decision; high)

*Measured* (`git` on the host, read-only): `HEAD` names the author's home
directory in no file (the control, the placeholder, is found in 3), while the
commit before the scrub, `7bf4f7a^`, names it in 61; 512 of the 526 commits
precede the scrub; there is no tag and no remote. `docs/release-checklist.md`
has no item on what history is published. The sensitivity is low - the paths
show the account name, the first name every commit's author field carries,
and a desktop folder's name, and four earlier fixtures held the development
VM's private address - but the scrub was the author's decision (D9), and
pushing the 526 commits as they are undoes it, irreversibly. Three choices:
publish as is (and say so in the checklist), squash to one first commit at
the tag, or filter the history.
One related judgement, the author's as before (the previous round's R6): 519
of the commits say they were written with Claude, and `CLAUDE.md` and the
briefs in `docs/prompts/` address an assistant, while the README says
nothing of it; if the history is squashed, the README becomes the only place
that could.

### Q5-4. Residues of the scrub in the tree (minor; high)

*Measured*: two Pi nodes' full hardware addresses remain in four 2026-09-26
evidence files (`docs/design/evidence/pi-cluster/2026-09-26-night-f1/retest-new-port/`
`dmesg-pi-00{1,2}.txt`, `nodecfg-pi-00{1,2}.txt`; six lines), where Part 4
removed them from the newer files as a policy; `ls -l` output naming the Unix
account is in 99 lines of 10 files under
`docs/reviews/evidence/phase1-step2-slice1/diagnosability/logs/` (the
previous round's R1 named it; Part 5 fixed the home paths only); three
decision records call the author by first name seven times
(`2026-09-23-autonomous.md`, `2026-09-24-day.md`, `2026-09-24-overnight.md`)
where the rest of the tree says "the author". A `sed` each.

### Q5-5. `osoe-stat JOB` and `--summary` silently show only 64 nodes of a job on one machine (minor; high)

*Measured*: a job of 70 logical nodes (`osoe-launch -m 0,1,...,69`, the
README allows 254): `osoe-stat` with no argument listed 64 segments and
"(6 more not shown)"; `osoe-stat JOB` showed 64 nodes and `--summary JOB` 64,
with no such note. *Observed in source*: `show_job` keeps the first 64 names
found (`tools/stat/stat.c:1302-1308`), in `/proc` order, so
which nodes are missing is arbitrary - and a hang's culprit can be among them.
On a cluster a node holds one segment a job, so this bites only many logical
nodes or many jobs on one machine. Fix: a growing array, or the note.

### Q5-6. Minor items, each a sentence or a message (minor; high)

- **A divergent `shmem_malloc` is silent** (*measured*, R17): one PE asking a
  different size leaves the next object at another offset on that PE; puts
  then land in the wrong place - two PEs read -1 for 1000 and 1001 - and the
  job exits 0. Undefined input, as a collective called differently within a
  node is, which the Limits list; this one they do not. Part 4 left the
  review's C5 out because "no draft carried its evidence": this is the
  evidence, and the sentence belongs beside the collectives' one under
  "Failures".
- **A misspelt variable is silent** (*measured*, R4): `OSOE_DEADLOCK_SECOND=0`,
  meant to turn detection off, left it on, and a job waiting on a word was
  ended as deadlocked at 10 s. The README itself tells programs woken from
  outside the library to set this variable to 0. The previous round put
  "unknown `OSOE_` names are ignored" after release; it stands, with this as
  its sharpest case.
- **Six settings checked after the join end the whole job, not the node, and
  not as the README says** (*measured*, R5, R5b): `OSOE_DEADLOCK_SECONDS=abc`
  or `OSOE_AGENT_CPUS=bogus` on one PE ended all four PEs of both nodes within
  0.06 s, the others saying "ending: the job is aborting on behalf of PE 1".
  Better than README:998-1001 describes ("every other PE of its node ends with
  it, saying that another local PE failed"), but not what it says; `init.c:305`
  ("every setting is checked by every PE before its node joins") says more
  than the code does (the settings check's item 7).
- **Messages that could say more** (*measured*; all but the descriptors'
  persist from the previous round): a dead or vanished node is named by
  index ("node 1 ended its connection without its goodbye",
  `progress.c:622-638`, `:1385`), and the README never says that node N is
  the step's node N (`SLURM_NODEID`, its place in the node list);
  `OSOE_HEAP_BASE=0x1000` is answered "no candidate heap base is free ...;
  set OSOE_HEAP_BASE to a free address" (R6, `node.c:1270`); a hard
  descriptor limit too low for the mesh gives "accepting a connection on port
  18438: Too many open files" with no word of `ulimit -Hn`, which the README
  names (R3); with
  `OSOE_NO_PROGRESS_TIMEOUT` set, a PE that never called `shmem_init` can make
  another node's first line about the rendezvous rather than the missing rank
  (A15; the missing rank is named too, by its own node).
- **The previous round's small ones that stand**: `OSOE_JOB_SECRET` is compared
  between leads but not marked *checked*; `SHMEM_INFO` prints 21 of the
  tables' 34 variables (none of the transport's buffers) and prints nothing
  without a started job (S5); `oshcc -O2 -show` fails ("unrecognized
  command-line option '-show'") where `-show` first works (S4); `--static`
  slips past the `-static` refusal into "have you installed the static version
  of the osoe library?" (S4); `osoe-launch` never prints the job id, and the
  README does not say that `osoe-stat` with no argument lists jobs; nor how to
  run `osoe-stat` on each node of a Slurm job - `srun --jobid=JOB --overlap`
  rather than a login, which is what the README's `RemoveIPC` paragraph warns
  about.
- **The release checklist's 1.6 line** names as known non-conformances "the
  scans, the deprecated routines' profiling names, and partial active sets",
  all three of which design §7.4-§7.5 records as built.
- **The docs index** sends the reader to `docs/reference/OpenSHMEM-1.6.pdf`,
  which is not in the repository; `docs/reference/README.md`, which says
  where to get it, is the line to point at.

### Q5-7. The seam: one check above it still belongs below (after release; high)

*Observed in source.* The two headers are as the previous round found them,
and the release fixes added a backend hook rather than a leak: the AM
settings are checked through `osoe_transport_settings_check` (`init.c:313`).
What a porter must still touch is four lines above it: the OpenSHMEM layer
runs the io_uring check for every job of more than one node
(`init.c:288-302`, `base/ring-check`), which Part 6 made stronger (it now
enables and enters the ring), so a RoCE build would refuse a node whose
`kernel.io_uring_disabled` is set although it opens no ring. Moved behind the
hook it is the backend's; until then the README's "one could be added without
touching the OpenSHMEM layer" holds but for those lines. The AM symbols that
`node.c:720`, `:1103` and `rendezvous.c:257` call are the known exceptions
`cmake/CheckBoundary.cmake` lists. `transport.h:536-539` still says a wait
completes "every nonblocking get this PE has issued" where the AM text and the
code complete the context's own.

## The previous round's findings, checked as claims

| Finding | Fixed by | Holds? |
|---|---|---|
| R1 private paths, private page, fixture identity, spec examples' notice, SPDX | Part 5 | **In part.** Every claim of Part 5 reproduced (the hygiene audit, re-checked for the counts): no home path at `HEAD`, the docs index starts at the decisions, the fixtures scrubbed, the four examples carry the notice's three elements, SPDX on 515 of the 519 source-like files, the four being the examples. Residues: Q5-3, Q5-4 |
| R2 the floor pin fails the README's first command elsewhere | Part 1 | **Holds** (*measured*): `shmem.floor` passed with the pins off and says why in its output ("pins not checked: they are the project's regression guard ... set on its own machines", S1); `env.echo-uring`'s pins, which stay on, passed 3 of 3 idle and 5 of 5 beside two busy loops on the four cores (S2, S3) - the record saw them fail only under eight |
| R3 README passages: RDMA, team row, budget row, one progress thread | Parts 4, 6 | **Holds** (*observed in source*): XRC and DC both named with the later review's recommendation as recalled; collectives behind `internode.h` not out of reach; the team limits stated and marked temporary; the budget row exact; one thread a node under Limits, with the 1 Gbit/s caveat |
| R4 settings rows | Parts 1, 3, 4 | **Holds**: every PE reads `OSOE_MAX_THREADS` (`bootstrap.c:296-305`) and it is compared (`node.c:1096`; *measured*, A12); `OSOE_AM_BUFFER`'s floor and the wire's ranges stated; the minors of Q5-6 stand. One row is new and wrong: Q5-1 |
| R5 the README's `-K1` defeats its promise about `shmem_global_exit` | Part 2 | **Holds** (*measured*, G1-G4): under `srun -K1 --cpu-bind=none -N1 -n4`, a caller on PE 1 with a 300 ms handler returned 7 five times of five, the handler finished and all four PEs' buffered lines were written each time; PE 0 with 3 s, 7; status 0, 0; a 15 s handler cut by the grace at 10.09 s with the status kept, 9. Under `osoe-launch` on two logical nodes, with and without `-k`, 7, the caller the only nonzero rank (B13, B14) |
| R5a requirements, version, first example, leftovers, build time | Parts 1, 6 | **Holds** (*measured* but the build's serial time, *documented*): with the interpreter hidden, CMake names the models and the counted build as left out and configures; 0.1.0 from every tool's `--version`, `SHMEM_VERSION` ("osoe 0.1.0, OpenSHMEM 1.6"), `SHMEM_INFO`'s first line and the soname (`libosoe.so.0.1.0`, soname `libosoe.so.0`); the first example has `-K1 --cpu-bind=none`; `--gc` removed every leftover of three kinds of failed start under Slurm (G5-G7); the build's time and `-j` stated; the models under their own label |
| R7 the seam | - | **Holds as before**, with Q5-7 |
| R8 a start-up failure leaves the rest waiting silently | Part 3 | **Holds** (*measured*, A1-A15, G5-G7): see *Diagnosability* |

## Diagnosability

**"Loud, never hung" now holds at start-up too, as far as one node reaches;
after init it held in every case.** What each failure gave, with the elapsed
time of the job (*measured*; case labels are the raw outputs'):

| Failure | What the user is told | Who ends |
|---|---|---|
| A PE `_exit(0)`, SIGKILL, SIGSEGV, a helper thread's `exit`, a return without finalize - waiting or computing, on one node or two (B1-B3, B15, R14, R18, R19) | the noticing PE names the culprit's rank and pid in its first line ("local PE 1 (rank 3, pid ...) has ended without finalizing"); each other PE "ending: the job is aborting on behalf of PE 3; this PE was waiting in ..." | every PE, within about 0.1 s of the death; the launcher's status the signal's (137, 139) or 1 |
| A node stopped 8 s with 64 MiB of puts toward it (B4) | nothing - it is waited for; the quiet completed 7.75 s in | nobody; `osoe-stat` meanwhile shows the putter "in shmem_put (buffer turnover toward node 1)" and the stopped PEs as waiting in a barrier, not as stopped (as before) |
| A deadlock: a barrier against a wait, a lock queue, two PEs waiting for each other's signal across nodes, every PE waiting (B5, B7, R20, B18) | at `OSOE_DEADLOCK_SECONDS=2`, after 3.1-3.3 s, one report naming every PE's wait ("PE 3 in shmem_long_wait_until (statics +0x10040)", "PE 0 in shmem_set_lock (PE 1)") | every PE, status 1 |
| A PE computing 12 s outside a barrier (B8) | nothing; no false end | nobody |
| Detection off, the same hang (B6; under Slurm by its job id, G11) | `osoe-stat JOB` names each PE's wait and for how long; `--summary` the team's epoch | the launcher, or `scancel` |
| Collectives mismatched between nodes (B10) | "a child's partial result ... should be piece 0 (32 bytes) ... and is piece 0 (64 bytes) ...: PEs called different collectives" | every PE |
| Collectives mismatched within a node (B9); a divergent `shmem_malloc` (R17) | nothing; exit 0 - the first documented, the second not (Q5-6) | nobody |
| A bad setting on every PE, or on one (A6-A9, A12-A14) | each refusing PE names the variable and the range; its node-mates "another local PE failed during rendezvous" | that node, at once; other nodes say what they wait for until the launcher ends them |
| A setting checked after the join (R5, R5b) | the variable and its range | the whole job, in 0.06 s (Q5-6) |
| Settings that disagree between nodes, found at the rendezvous (B20, B21) | the variable, both values, "node 2 (or a node below it)"; the others "the rendezvous lost node 0 ... it may have refused the job; its own message says why" | every node, in 0.05-0.12 s |
| `/dev/shm` too small (A5, A5b) | the free and needed MiB, and three remedies | that node at once - the previous round's likeliest first-day hang |
| A task that never calls `shmem_init` (A1-A4, G6) | after 30 s (2 s under `OSOE_TEST_NOTICE_SECONDS`), every minute: the lead names the missing local id and rank; a missing lead is named by its node-mates; other nodes name the node and address they dial | the launcher, or `OSOE_NO_PROGRESS_TIMEOUT` (A15, which also names the missing rank) |
| Exhausted resources (B11, B12, R7, R16, R3) | teams: one line, nonzero return, as documented; threads: ends naming the variable; contexts: nonzero, silent, as documented; landing zone: backpressure, 0 wrong bytes in 1,000 gets of 1 KiB through a 1 KiB zone; descriptors: "Too many open files" (Q5-6) | as documented |
| API misuse (R2, R8, R9, R11, R12, R15) | a put to NULL, to the stack, to PE -1 or N, a call before init or after finalize, a collect into too small a destination: each named with the routine | every PE that did it |
| A failed start's leftovers under Slurm (G5-G7) | `osoe-stat` lists a dead segment ("0 live") and a tombstone ("a failed start's tombstone") and explains the latter | `osoe-stat --gc` removed all 7, 7 and 1 objects |

What is convincing that the start-up fixes are real rather than the tests'
artefacts: every case the previous round left hanging on one node now ends
that node within 0.1 s, under `osoe-launch -k`, which kills nothing; and the
three kinds of leftover under `srun` - the step killed by `-K1`, cancelled,
and a lead's tombstone - were each listed and removed by the installed
`osoe-stat`, the README's epilog line never needed.

## The README, read as its intended reader

The README now reads in the order a programmer who knows MPI meets osoe:
two minutes of OpenSHMEM, a ring, requirements, building, installing, running
under Slurm and alone, looking inside a job, and an index of what to do when
a job fails - then the design, the limits and the reference. Followed as
written in the VM, every command ran: the batch script under `sbatch` (G8);
the refusal of a program run straight from a batch script, by name (G9); the
smoke test from the installed prefix under `srun -K1` ("osoe-pacer failures=0
seconds=2.6", G10); the install put exactly the files the README lists
(`libosoe.so.0.1.0` and its links, the three headers and `mpp/`, `oshcc`,
`osoe-stat`, `osoe-pacer`, `lib/osoe/osoe-statics.ld`). Every setting's
default, range and *checked* mark matches the code (the settings check, about
34 variables) but `OSOE_AM_HUGEPAGES`'s behaviour (Q5-1). The README's own
recipe is the one command that misbehaves (Q5-2). The rest of what a stranger
would trip on is in Q5-6, a sentence or a message each.

## The repository

The licence is as the previous round measured it: `COPYING` and
`COPYING.LESSER` unchanged since `b0e0b53`; every SPDX line the one value,
`LGPL-3.0-or-later`; no conflicting notice; no third-party code without its
origin; no credential, key or token, and no e-mail address in the tree but a
benchmark's published contact in its captured output (the hygiene audit's
battery, each with a control; the counts re-checked). The docs index
no longer sends a stranger to a private page and says what was edited in the
evidence and why. The `docs/` history - 948 of the 1,370 files - reads as a
record: the index labels it history and says why it is kept. What remains is
Q5-3 and Q5-4, and the dangling pointers a stranger meets past the first
documents: `build/scratch/...` in 96 tracked files (one each in the design
notes and the runbook), and three mentions of the author's status page in
documents a stranger reads (`docs/decisions/README.md:130`,
`docs/release-checklist.md:4`, `docs/design/evidence/pi-cluster/README.md:6`),
none of them misleading.

## The team proposal and step D's brief, as they bear on this question

Nothing in them would make this question's answer worse. D's brief carries
what Part 3 taught about settings - `OSOE_TEAM_BLOCKS` read and validated by
every PE, compared among a node's PEs, a `SHMEM_INFO` line, a README row
marked *checked* among a node's PEs, the room check's sum - and names each new
failure (a dry pool naming the setting and the node, a message for a block
with no map entry ending the job naming the sender), with `osoe-stat` walking
the pool by name and generation. One small thing: its citations predate the
release fixes (`node.c:817-824`, which it gives for the `OSOE_MAX_TEAMS`
comparison, is now `create_control`'s header writes; the comparison is at
`node.c:1062`), which its own stop-and-ask on colliding with Part 3
anticipates. A gap D neither causes nor closes: the deadlock report groups a
node's PEs by routine and epoch (`deadlock.c:348-350`) and the "blocked in"
record carries no team, so PEs waiting in two different teams' collectives at
the same epoch print as one line - today with job-wide numbers, and after D;
D's `osoe-stat` work pairs epochs with blocks for the summary only.

## What is sound

- **Ending a job**: every death after init ended the whole job within about
  0.1 s, the culprit's rank and pid in the noticing PE's first line, on one
  node and two, waiting or computing (seven cases); no PE was left behind in
  any case of this run.
- **Proving a deadlock**: four shapes, on one node and two, each ended in
  3.1-3.3 s at a 2 s setting with a report that points at the culprit; no false
  end for a PE computing 12 s; with detection off, `osoe-stat` by Slurm job id
  shows every wait.
- **A stopped node** with 64 MiB in flight is waited for, not declared dead.
- **Start-up**: refusals end their node by name at once, the `/dev/shm`
  refusal included; every wait says what for, naming ranks, hosts and ports;
  `--gc` cleans every leftover kind under Slurm, tombstones included.
- **Global exit under Slurm**: the caller's status every time, its handlers
  finished, every PE's output flushed; the 10 s grace as documented.
- **The stranger's path**: the first command passes with the pins off and
  beside half the cores busy; the install, the smoke test, the batch script
  and the tools' versions are what the README says.
- **The README's tables**, every link and anchor (the hygiene audit), and its
  limits but for Q5-1 and Q5-6's first item.
- **The licence and the repository's content**, as measured before and
  re-measured for what changed.

## Sources

- Runs: scripts and raw outputs, kept outside the repository in the
  investigator's scratch directory, `q5-runs/` (its full path names the
  author's account, so it is in the hand-off to the lead rather than here):
  (`batchA.out` A1-A15, `batchB.out` B1-B21, `resources.out` R1-R20,
  `slurm.out` G1-G11, `stranger.out` S1-S5, `relaunch.out` and
  `relaunch-launch.out`, `install.log`; `diag/fail.c`, the failure-injection
  program, its modes chosen by `FAIL_MODE`; `scripts/`). The VM copy they ran
  in, `/tmp/q5r`, is removed.
- `build/scratch/release-fixes/review-briefs/readme-settings-check-08b3da0.md`
  and `repository-hygiene-audit-08b3da0.md`, verified where used.
- `README.md`, `docs/README.md`, `docs/release-checklist.md`,
  `docs/decisions/2026-10-02.md`, `docs/decisions/2026-10-02-release-fixes.md`,
  `docs/prompts/phase1-release-fixes-plan.md`,
  `docs/design/team-resources-proposal.md`,
  `docs/prompts/phase1-step7-slice3-prompt.md`, design §3.4, §6.2, §7.4-§7.5;
  `shmem/src/connection-area.c`, `node.c`, `init.c`, `abort.h`, `progress.c`,
  `transport.h`, `tools/stat/stat.c`, `tools/stat/segment.c`,
  `tools/wrapper/oshcc.in`, `cmake/CheckBoundary.cmake`.
