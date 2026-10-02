# Task: the fixes before the first release, all but the teams

**Status: written on 2026-10-02 from `phase1-release-fixes-plan.md` (the plan),
which validated the round-three review's sixteen findings and answers the
review lead's brief, `phase1-release-fixes-prompt.md`. Where that brief and
the plan differ, the plan is right: it corrects three of the brief's fixes
(plan §1, points 3 to 5). This brief is what to build, where, and under which
decisions; it does not repeat the plan's descriptions of the findings, which
is why the plan is read first.** It covers everything the review found except
the team numbers (P1, C4's first row), which have a design proposal of their
own, `docs/design/team-resources-proposal.md`, and are built on separate
briefs - also before the tag, by the author's decision of 2026-10-02.

It is written for a session that starts cold: an Opus model, in this
repository, with the VMs described below. Nothing in it needs the
conversation that wrote it.

**Decisions this brief builds on.** The plan's §2 lists nine. Three are the
author's; the others are the plan's recommendations, which the author read
on 2026-10-02 and struck none of (`docs/decisions/2026-10-02.md` is the
record of that day). Build on them as written here, and say in the record
which were the author's and which the plan's.

| | Decision | Taken as |
|---|---|---|
| D1 | `shmem_global_exit` under a launcher that ends the step on a bad exit | **the author, 2026-10-02**: only the caller carries the status (Part 2) |
| D2 | how far the start-up fixes go before the tag | **the author, 2026-10-02** ("D2 seems fine to me"), the plan's middle option: every PE validates every setting; the segment before the refusals; the 30 s notice; the tests with `-k`; the sentences. A refusing node telling its neighbours is after the tag (Part 3) |
| D3 | team numbers | **the author, 2026-10-02**: numbers held per PE, then counters per team, both before the tag - not here |
| D4 | `shmem.floor`'s pins | checked only when asked (Part 1) |
| D5 | `OSOE_AGENT_SLICE_US` | keep 100; both sides documented (Part 4) |
| D6 | `OSOE_MAX_THREADS` | keep 16; compared PE by PE (Parts 1 and 3) |
| D7 | the README's first command | the models under a label of their own (Part 1) |
| D8 | the version | 0.1.0 (Part 1) |
| D9 | the evidence trees | scrub the paths to placeholders; SPDX lines on every file but the specification's examples (Part 5) |

The author's goals, the axes to judge the work by, are the project's usual
three and one for this work in particular:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks** - a constraint on
  the seams, not work here.
- **A stranger's first day**: every sentence of the README true of the code
  as tagged; every failure loud, naming its cause; nothing that waits in
  silence. The review was run as that stranger, and these fixes are what it
  tripped over.

## Read first

1. `CLAUDE.md`: how work is delegated, how names are spelled, how a rename is
   audited. The last of these governs Part 5.
2. The plan, `docs/prompts/phase1-release-fixes-plan.md`, whole. Each finding
   is described there with what happens, how it was checked, the options and
   the recommendation; §1 lists what the validation corrected; §2 the
   decisions above.
3. `docs/reviews/archive/phase1-release-review.md`, the synthesis, for the
   review's own words and ranking. The five deliverables beside it only where
   a part sends you.
4. The design notes: §3.15 (the error model: no wait gives up because time has
   passed; loud, never hung), §3.16 (the atomics rules - named orders only,
   no fences, no intrinsics), §6.2 (the rendezvous), §3.14 (the control
   segment and the "blocked in" record), §1's release point.
5. `docs/decisions/README.md`, whole: one line a decision in force. A change
   that touches one updates it in the same commit.
6. The code each part names, whole where the file is short: `abort.c`,
   `abort.h`, `listener.c`, `job.c`, `bootstrap.c`, `node.c`'s
   `create_control`, `open_control` and `osoe_node_join`, `fatal.h`.

## The state of the tree

- `main` is `b0e0b53` ("The night of 2026-09-28: the record, and the tree
  made ready for review"). Nothing from the review is committed. Untracked:
  the six review files `docs/reviews/archive/phase1-release-*.md`, the
  review lead's brief, the plan, the team proposal, and this file.
- Worktrees: `build/scratch/wt/review-r3`, the review's build of `b0e0b53`
  (detached; keep it until Part 6, then remove it);
  `.claude/worktrees/dreamy-brahmagupta-d9e7dd`, another session's, on an
  older branch (`prompts-index-steps-4-to-10`) - leave it, and ask the
  author before removing it.
- VMs (Lima, `limactl shell <name>`; the repository is mounted at its host
  path, design §8.5): `osoe`, the development VM, Slurm 24.11.5 installed
  (`srun` works on its one node, `-N1`), shared with other sessions;
  `osoe-load`, the review's fresh VM - the machine to verify a change on as a
  stranger would meet it, and to run the suite under load; `osoe-x86`,
  Rosetta - builds and one-node tests only, no io_uring. The last two were
  made for the review and go when Part 6 is done.
- Evidence on this machine, git-ignored, under `build/scratch/`:
  `review-r3/reports/` (the investigators), `review-r3/refutation/` (the
  refuters, and `lead/` with the plan's own reruns and their logs),
  `review-r3/atomics-audit/` (the blocked-record litmus),
  `review-r3/newest-work/` (the stopped-bulk experiment),
  `review-r3/trust-kernel/` (the hello fuzzer), `global-exit-zero/` (D1
  measured: the patch, the scripts, the logs), `teams-limit/`.
- The host disk is at 96 %, about 19 GiB free. A worktree with four preset
  builds takes several GiB: remove each part's worktree once its branch is
  recorded, and check `df -h .` before a long run.
- The Pi cluster was rebuilt on 2026-09-28 and holds no tree. Nothing here
  needs it; the Pi runbook belongs to the release point's conditions, after
  the teams.

## How to work

- **A branch and a worktree a part**, `build/scratch/wt/<branch>`, made with
  `git worktree add`. Never edit a worktree a verification is building from:
  the VM builds from the mounted tree, so an edit mid-run tests a mixture.
- **Commits** on the part's branch, in the tree's style: a subject that is a
  sentence (`git log --format=%s -20`), a body that says what was verified
  and where. **Merging to `main`**: Part 0 goes to `main` directly, it is the
  record. For Parts 1 to 5, the author's rule of 2026-10-02: "merge into
  main at green checkpoints" - a part merges when its verification is green
  and its record is written, and the record says so. No push and no tag: the
  tag is the release point's, after the teams.
- **The README is true of the tree at every commit** (the author,
  2026-10-02: "README should be updated before each commit to reflect commit
  reality"). Each part's branch carries the README sentences for what it
  changes, so the merge describes itself; Part 4 lists them by part. A
  sentence that later work will replace - the team row until the redesign
  lands - may say so: "temporary", "work in progress". The same holds for the
  design notes and the decisions index where a part touches what they state.
- **Verification of a part**: the four presets (`debug`, `release`, `asan`,
  `tsan`) and `tier3` where the part touches anything between nodes; every
  new test run against the mutation it was written for, and against the
  tree without it, so that it is seen to fail; timing-sensitive tests also
  under load in `osoe-load` (eight busy loops; the review's baseline, logs in
  `build/scratch/review-r3/load/`, was 596 of 598 there, two timeouts:
  `strict-c11` and `shmem.reply`). The models
  run in the `release` preset and three take 11 to 18 minutes each; run
  those in the background from the main session, not from a subagent, and
  not beside the `env.*` tests.
- **The VM is shared.** Before a run: `vmstat 5 2` for idle, and look for
  another session's scripts. Every top-level run script takes
  `exec 9>/tmp/osoe-vm.lock; flock -w 10800 9` so that runs queue rather than
  overlap. Stop a run by its PIDs (`ps -eo pid,pgid,args | grep '[p]attern'`,
  never `pkill -f` through `limactl shell`, which matches its own wrapper),
  and wipe the tree's `build/` after a kill. `git` is not installed in the
  VM: commit and restore from the host. One shell call is capped at ten
  minutes.
- **Subagents** (CLAUDE.md): Sonnet for the mechanical parts - the sweep of
  settings in Part 3, the README's line-by-line check in Part 4, Part 5
  whole, the checklist passes. Keep the protocol work in the main session:
  Part 2's model, Part 3's listener and segment order. A subagent's report is
  evidence, not a verdict. If subagents fail on a usage limit, do the work in
  the main session rather than wait.
- **Models first.** A part that changes what a modelled protocol does changes
  the model first, with a broken variant that must fail: `abort_exit_model.py`
  and `abort_model.py` for Part 2, `rendezvous_model.py` if Part 3's listener
  change alters what the model abstracts.
- **Names are words** (CLAUDE.md): `notice`, `tombstone`, `unconfirmed`, not
  contractions. File names kebab-case, identifiers snake_case.
- **The team code is not touched here**: `team.c`, `internal-region.h`,
  `internode-generic.c`, the team blocks of `node.h` and `create_control`,
  and the README's team rows beyond the one sentence Part 4 names. The team
  work will change `create_control` and bump `OSOE_NODE_LAYOUT` too; keep
  Part 3's edits to `node.c` local and expect a rebase against it. Whoever
  merges second takes the next layout number.

## Part 0: the record goes in first

Commit, to `main`, as they are: the six review files, the review lead's brief
(`docs/prompts/phase1-release-fixes-prompt.md`), the plan, the team proposal
(`docs/design/team-resources-proposal.md`), the day's record
(`docs/decisions/2026-10-02.md`) with the decisions index's lines for it
(already edited in the working tree), the first team brief
(`docs/prompts/phase1-step7-slice3-prompt.md`), the session's opening prompt
(`docs/prompts/task-first-release.md`), and this brief. One edit before the
commit, D9's: `phase1-release-readiness.md` quotes the author's home path
three times, and would be the 62nd tracked file carrying it; write it
`/Users/<author>` there. Prove the audit before trusting it: the grep that
finds the path must report three lines in that file before the edit and none
after.

`docs/prompts/README.md` is already edited in the working tree: its "Open
now" paragraph names this brief and the team brief, and its table has rows
for the review lead's brief, the plan and both; commit it with the rest, and
fill this brief's row in as the parts land.

## Part 1: the small code fixes

One branch, `release-small-fixes`. Each item has one sensible fix; the plan's
§4 says why.

1. **The C11 forms of `shmem_signal_add` and `shmem_signal_set` (C1, plan
   §4.13).** `shmem.h:182-185` declares the three-argument functions and the
   `shmem_ctx_` names, with no macro choosing by count. Add the two macros in
   the style of `shmem_sync`'s (`shmem.h:1593-1598`: a parenthesised
   `(shmem_signal_add)(...)` for the three-argument case, so the function is
   called through its own macro's name) and `shmem_atomic_add`'s
   (`shmem.h:1097-1100`, the `OSOE_PICK4` dispatch with a complaint
   identifier for a wrong count). Check how the header gives C++ the
   two-form routines (`shmem_sync` again) and do the same. Tests: a call of
   each four-argument form in an existing signal test, so it is compiled,
   linked and run; both families added to the `shmem.generic-arity.*` list
   (`shmem/tests/CMakeLists.txt:1241-1254`), with their complaint; the
   headers' C++ test. No new function, so `shmem.exports` and the
   `pshmem_`/`shmem_` pairing are unchanged - check them anyway.

2. **The "blocked in" record's routine name as four atomic words (K1, plan
   §4.11).** `node.h:180`, `char blocked_routine[OSOE_BLOCKED_ROUTINE_MAX]`
   between the two seq_cst bumps of `blocked_gen`, copied with `memcpy` by
   the writer (`metrics.c:565-584`) and by three readers: `deadlock.c:302-317`,
   the PE's own agent (`metrics.c`, after `publish`), and `osoe-stat`
   (`tools/stat/segment.c:494-527`; its generic string read at `:475-476`
   must not be the path this field takes). Make it `_Atomic uint64_t [4]`:
   the writer packs the name into four words and stores each
   `memory_order_release`; a reader loads each `memory_order_acquire` and
   unpacks. The bytes in the segment do not move - same offset, same 32
   bytes - so the layout number need not change; say so in the comment at
   `node.h:165-175`, which explains the protocol, and why a plain array was
   wrong (the review's K1: on aarch64 nothing orders a reader's plain loads
   before its second load of the generation). Put the reader in one place
   the three share (`blocked.h` is the header `deadlock.c`'s comment names;
   check `tools/stat` can include it) rather than fixing three copies. The
   litmus, `build/scratch/review-r3/atomics-audit/blocked-litmus.c`, becomes
   a test under `shmem/tests/` that runs the library's own writer and reader
   rather than a copy of them, bounded to a few seconds, and asserts no torn
   record; its mutation is the plain `memcpy` back in the writer, which must
   fail on aarch64 (the litmus accepted 28,000 to 218,000 torn records in 60
   to 90 million reads; it cannot fail on x86, say so in the test's comment).
   `blocked_record_model.py` passes before and after, since it runs under SC
   and TSO; its header comment says that is why it did not see this.

3. **A test for the silent-peer check's gate (K5, plan §4.12).** The check at
   `wire.c:302` reads how long the kernel has acknowledged nothing only while
   something is unacknowledged. The test is the investigator's experiment,
   `build/scratch/review-r3/newest-work/stopped-bulk.c` and `.sh`: 64 MiB put
   toward a node whose PEs are stopped (SIGSTOP) for at least 15 s, with
   `OSOE_KEEPALIVE=1,1,3`, on the map `0,0,1`. With the gate the job completes
   after the node is continued; without it (the mutation: return
   `tcpi_last_ack_recv` unconditionally) the job ends at about 4 s, declaring
   the node gone. Fifteen seconds is the floor: at 10 s the mutation survived
   three runs of three. About 20 s added to the suite; label it `tier2`, give
   it a timeout, and run it in the sanitizer presets too.

4. **The buffer-shrink notice once a node (P3, plan §4.2).**
   `connection-area.c:204-211` prints in every PE; 128,000 lines at 500 nodes
   of 256. Print it from the node's lead only, as `print_info` is
   (`init.c:229`). A test counts the lines against the node count.

5. **`shmem.floor`'s pins only when asked (R2, D4, plan §4.1).**
   `test-floor.c:81-83` and `:300-329`: the three pins (`FASTEST`,
   `INTERNAL`, `STREAM`) are checked when `PINNED` - a release build - and
   the stream's margin is thin on other machines (0.0999 against 0.10 beside
   a `-j4` run; 0.103 idle in the review's VM). Check them only when
   `OSOE_TEST_FLOOR_PINS=1` is in the environment as well, and print the
   ratios always, with a line saying the pins were not checked and how to
   ask for them. The project's own verification sets it: add it to the
   `ENVIRONMENT` of the test through a CMake option (`OSOE_TEST_PINS`,
   default `OFF`) so that the four-preset runs this brief describes set
   `-DOSOE_TEST_PINS=ON` and a stranger's `cmake --workflow` does not. If
   `env.echo-uring`'s pin proves thin on `osoe-load` too, give it the same
   switch and record it; the review did not flag it.

6. **Python optional for the stress tester's counted build (R5a, plan §4.5).**
   `tools/pacer/CMakeLists.txt:37` makes `Python3` `REQUIRED`. Make it as the
   models' is (`tools/modelcheck/CMakeLists.txt`): found or not, with a
   `STATUS` message; without it the counted targets
   (`osoe-pacer-counted`, `osoe-pacer-start-pes-counted`) and the
   `tools.pacer.counted.*` tests are not registered. Verify with
   `-DCMAKE_DISABLE_FIND_PACKAGE_Python3=ON`: the build completes and the
   suite passes without those tests and the models.

7. **The models under a label of their own (D7, plan §4.5).** The 24 models
   are 55 of the 61 minutes `cmake --workflow --preset release` takes
   (`model.abort` 18, `model.abort_exit` 17, `model.tree_barrier` 11). Give
   them the label `model` beside `tier1`, let the `release` test preset
   exclude that label, and add a `models` test preset (configure preset
   `release`) that runs only it. The project's verification is then the four
   presets plus `models`; the README's first command runs the suite without
   them, and the README says what it runs, how long (measure it in
   `osoe-load`), and how to run the models. Nothing about which models run
   changes: `OSOE_MULTIPLE_MODELS` stays as it is.

8. **A version (R5a, D8, plan §4.5).** `project(osoe VERSION 0.0.0)` at
   `CMakeLists.txt:5` becomes `0.1.0`, and that one value reaches everything
   that names a version: the line `SHMEM_VERSION=1` prints at start-up
   (`init.c:226-228`, "osoe 0.1.0, OpenSHMEM 1.6"), the first line of
   `SHMEM_INFO`'s text, every installed tool's `--version` (`osoe-launch`,
   `osoe-stat`, `osoe-pacer`, `osoe-bench`, `oshcc`), and the shared
   library's `VERSION` and `SOVERSION` properties if it is built shared. Pass
   it as a compile definition from `PROJECT_VERSION` rather than generating
   `shmem.h`, and leave `SHMEM_VENDOR_STRING` as "osoe", which is a name, as
   the other implementations' are. A test compares `SHMEM_VERSION=1`'s output
   with the value CMake knows.

9. **`OSOE_MAX_THREADS`, what the words say (C3, plan §4.3).** The limit is on
   live threads that have ever called the library - a thread's slot is freed
   when it exits (`metrics.c:487-538`) - not on threads "in the library at
   once". Fix the two texts in the code: `SHMEM_INFO` (`init.c:95-97`) and
   the fatal (`metrics.c:537`) - and the README's two places in the same
   branch (Part 4, "With Part 1"). The check PE by PE is Part 3's.

10. **`osoe-stat --gc` and the names a failed start leaves (R5a, plan §4.5).**
    Under `srun`, a start that fails leaves `.conn` and `.job` objects in
    `/dev/shm` that `osoe-stat --gc` does not know (`stat.c:984`; the names
    are made in `job.c` and `connection-area.c`). Teach it both. A test makes
    a failed start, runs `--gc`, and finds nothing left. The README's epilog
    line comes with it (Part 4, "With Part 1").

Verification: the four presets and `models` in `osoe`; items 2, 3 and 5 under
load in `osoe-load`; item 6's configure without Python; item 7's time
measured. Record what each test's mutation did.

## Part 2: `shmem_global_exit` - only the caller carries the status (D1)

Branch `global-exit-status`. The author's decision, after the plan's §3.1 C
was measured: every PE other than the caller leaves a global exit as it does
today - at once, its standard I/O flushed, `_exit` - but with status 0, so
that a launcher which ends the step at the first bad exit (`srun -K1`,
`KillOnBadExit`) sees none before the caller's own, which comes after its
handlers and its flush. `srun` returns the highest exit code of all tasks,
which is then the caller's. Nothing waits for anything and the abort
protocol's interleavings are unchanged.

1. **The model first.** `tools/modelcheck/abort_exit_model.py`'s STATUS
   property ("every process ends with a status some caller gave") is
   restated: a process that called `shmem_global_exit` ends with a status
   some caller gave; a process that did not ends with 0 when what it read was
   a global exit, and with the failure status when it was a fatal. Add the
   broken variants that the new property must catch: a non-caller leaving
   with the caller's status (today's behaviour) and a caller leaving with 0.
   `abort_model.py` the same if it states a status. Both take 17 to 18
   minutes in the VM: background jobs from the main session.

2. **The code**: the two lines, measured already in
   `build/scratch/global-exit-zero/abort-c-experiment.diff` - in
   `osoe_abort_end` (`abort.c:93-113`), `grace_status` is the status only
   when the disposition is not a global exit or this process called it
   (`called`), and the non-caller's `_exit(status)` becomes `_exit(0)`. The
   `alarm` fallback, which ends by signal, is unchanged. Restate the comments
   at `abort.c:101-107`, `abort.h:12-27` and `init.c:447-455` so they say
   what is now true, and why: the status is the job's and the caller's, not
   each task's.

3. **`osoe-launch` reports status as `srun` does** (`launch.c:517`, today
   `failed ? 1 : 0`): the highest exit code of any rank; 128 plus the signal
   for a rank killed by a signal the launcher did not itself send; 1 for the
   launcher's own findings - a timeout, leftovers - when no rank reported
   more. Say so in its `--help` and in the README's `osoe-launch` section.
   Audit the tests for one that depends on the status being exactly 1
   (`shmem/tests/*.sh` and `CMakeLists.txt`'s `WILL_FAIL`s) before changing
   it.

4. **The tests.** `shmem/tests/global-exit.sh` requires every rank to leave
   with STATUS; now the caller does and every other rank leaves with 0 - for
   a nonzero STATUS, exactly one "exited with status" line, the caller's;
   for STATUS 0 none; the fatal cases unchanged. Two new cases: (a) without
   `-k`, so the launcher kills at the first bad exit, where the launcher's
   own status must be STATUS and no rank killed - this is the review's
   scenario and passes only with the change; (b) the caller with an `atexit`
   handler that takes a second (a `handlers` mode with a delay), under (a).
   The `shmem.global-exit.*` registrations (`shmem/tests/CMakeLists.txt`,
   from `:1932`) are the place to add them.

5. **Under Slurm, by hand**, in `osoe`: rerun
   `build/scratch/review-r3/refutation/lead/run-slurm.sh` and
   `build/scratch/global-exit-zero/run.sh` on the branch - handlers of 0 ms,
   200 ms, 1 s and 5 s under `srun -K1`, the caller at PE 0 and at PE 3, and
   status 0 - and record the table in the decisions file. The review's
   measurement without the change was 143 at 200 ms; the plan's with it was 7
   at every handler length. This run joins the release checklist (Part 5,
   item 3), since no ctest runs under Slurm.

6. **An adversarial read** by a fresh subagent, given the diff and the plan's
   §3.1: find a path on which a non-caller leaves nonzero on a global exit, a
   caller leaves 0 with a nonzero status, or the agent-driven end
   (`osoe_agent_speaks`, the model's "its agent, by exit") differs from the
   main thread's.

7. **The sentences** travel with this branch (Part 4, "With Part 2"): the
   README's "How a job ends", the 10 s the caller's `exit` has, the
   `WaitTime` limit, `osoe-launch`'s status; design §3.15; the decisions
   index.

## Part 3: start-up that hangs in silence (R8, N1, N2; D2)

Branch `startup-loud`. The plan's §3.2 describes the three defects and the
options; this is the middle option for (a), the only fix for (b), and all
three parts of (c). Everything here keeps §3.15's rule: no wait gives up
because time has passed. A notice is not an end.

### 3a. A failure before the node's segment exists

1. **Every PE validates every setting.** Two settings are validated by the
   lead alone: `OSOE_MAX_THREADS` (`metrics.c:455-465`, called only from
   `create_control`, `node.c:512`; the others copy the lead's count at
   `node.c:888`) and the whole-extent check of `OSOE_GET_LANDING_BYTES`
   (`node.c:549`, after `osoe_am_config_read` at `:541`). A bad value given
   to every PE then ends the lead by name and leaves its node-mates waiting.
   Move both to where every PE reads its environment (`osoe_bootstrap_read`,
   as `OSOE_MAX_CONTEXTS` is at `bootstrap.c:268-272`), carry the values in
   the bootstrap, and compare them PE by PE in `open_control` as
   `OSOE_MAX_CONTEXTS` is (`node.c:840-847`), ending by name on a mismatch
   (D6). Then sweep: a Sonnet subagent lists every `OSOE_*` and `SHMEM_*`
   setting the library reads, where it is validated, and whether a node's
   PEs are compared on it, from `bootstrap.c`, `connection-area.c`,
   `metrics.c`, `node.c` and `init.c`; the investigators' tables are in
   `build/scratch/review-r3/reports/diagnosability.md` and
   `refutation/startup.md`. Any other setting validated by one PE only gets
   the same treatment. The README's rows mark the compared ones *checked*
   (Part 4, "With Part 3").

2. **The segment exists before anything can refuse.** The lead checks
   `/dev/shm` for room (`node.c:611-628`) before it creates the control
   segment (`:631`), so that refusal - the likeliest first-day failure - has
   nowhere to be published and the node's other PEs wait forever. Reorder:
   create the segment and fill its header, then check the room, refusing
   through `fail_rendezvous` (which publishes `failed`, `node.c:164-185`)
   instead of `osoe_shm_short`'s `osoe_fatal`. The waiters already look at
   `failed` on every sweep of both of `open_control`'s loops (`:784-804`),
   so they end with "another local PE failed during rendezvous" as they do
   for a failure after `ready`. Find what the lead's fatal does with the
   segment's name today - `osoe_shm_orphaned` is `st_nlink == 0`, so an
   unlinked segment sends a waiter back to waiting by name - and leave the
   name linked with `failed` set: it is then a failed job's leftover like any
   other, which `osoe-stat --gc` removes and the README already describes.
   `OSOE_SHM_CAPACITY_CHECK=0` keeps its warning.

3. **A PE that refuses its own environment tells its node before it goes.**
   A refusal in `osoe_bootstrap_read` happens before `osoe_node_join`, with
   no segment to write to. Once the PE's identity is parsed - job, step,
   node, session, local id - install a hook the fatal path runs
   (`osoe_fatal_then`'s shape, `fatal.h:24`): the lead creates the control
   segment as a tombstone, nothing but a header large enough for
   `open_control` to map (at least `sizeof(struct osoe_node_header)`), with
   `failed` set and `ready` never set; any other PE opens the segment by
   name, waiting at most the abort grace for the lead to create it, sets
   `failed` and wakes as `publish_failure` does. Order the bootstrap so
   identity is read first; a refusal of the identity itself stays as it is -
   nothing can be told - and the notice covers the others.

4. **The notice.** Every start-up wait that has lasted 30 s prints one, and
   again each minute, saying what it waits for; it ends nothing. The waits
   are the ones that honour `OSOE_NO_PROGRESS_TIMEOUT`: every call of
   `osoe_stall_watch_stalled` between `shmem_init`'s first line and its
   return (`open_control`'s two loops, `wait_count`, `osoe_listener_accept`,
   `osoe_listener_connect_port`, the mesh connects at `listener.c:550-598`,
   `job.c`'s follow, `rendezvous.c`). Put the "is a notice due" question in
   the stall watch, so each site adds one branch and its own text:
   - a lead waiting for its node's PEs names the local ids and ranks still
     missing and the phase they have not reached (`describe_missing` exists);
   - a PE waiting for its lead names the lead's rank and whether the segment
     exists yet;
   - a lead waiting for another node names that node's number, its host-list
     entry, the address dialled, the port or ports, and the last error
     (`dial`, `listener.c:409-433`, throws the `connect` error away today:
     keep it, with when it was last tried) - "Connection refused", "timed
     out", "no answer to the hello", "waiting for it to call". This is N1's
     whole fix.
   - **One line a node a minute, not one a PE.** The lead prints for every
     wait it is in; among the others, only local id 1 prints, and only while
     no segment exists for the lead to speak through. At 256 PEs a node a
     line from every waiter is P3's defect again.
   The 30 s is settable for the tests through a test hook in the style of
   `OSOE_TEST_PERTURB`, not a documented setting.

5. **Tests with `-k`** (option 5). The refusal tests
   (`shmem.job.disagree.*`, `shmem/tests/CMakeLists.txt:303-399`;
   `shmem.job.secret*`, `:406-423`) launch without `-k`, so `osoe-launch`
   kills the survivors and the suite cannot see the hang. Run them with `-k`
   and a timeout, and require: on a one-node map, every PE of the node to
   end by itself, the refuser naming the variable and the others "another
   local PE failed"; on several nodes, the surviving node to print the
   notice naming the node that never called, before the launcher's timeout
   ends it. Add the cases the plan's table lists (`/dev/shm` refused, with a
   heap larger than its free space; `OSOE_MAX_THREADS=0` on every PE; one
   PE's own environment bad; a PE that exits before `shmem_init`), and a
   mutation for each new mechanism: the room check moved back before the
   segment must hang the node-mates again; the tombstone removed must leave
   them waiting.

### 3b. A node that cannot reach a peer (N1)

The notice of 3a.4, with the peer's host-list entry, address, ports and last
error. Nothing gives up. The rendezvous listener's bind is **not** changed
(the brief's option; the plan's §3.2b says why not before the tag); instead
the README gains the sentence that a node's name must resolve to the same
address on every node (Part 4, "With Part 3"). The investigator measured
the three causes in network namespaces (`build/scratch/review-r3/reports/`):
a tier 3 test of one of them - a wrong address for a peer - checks the
notice names it.

### 3c. The rendezvous proves the secret before it acts (N2)

`osoe_listener_accept` (`listener.c:334-404`) today: a wrong count is fatal
at `:371-375` and a node claim out of range at `:378-380`, before the
confirmation is read at `:389` with no bound and the proof checked at `:395`;
and the loop serves one caller at a time. With a secret set, a host that can
reach the port and knows what `squeue` shows (the session number is a counter
from 0, `init.c:274`) can end a node's start-up with one hello, or stall it
with one hello and silence.

1. **Prove first.** With `b->secured`, check the counts and the node claim
   after the confirmation's proof, so that an unproved caller is refused
   without being acted on. Without a secret nothing can be proved and the
   order stays. A genuine mislaunch still ends naming both sides; consider an
   answer verdict that says why, so the caller names the mismatch too rather
   than "the rendezvous lost node N" - either is acceptable, the test
   requires both nodes to end naming something true.

2. **An unconfirmed caller must not hold the loop.** Not by a bound on the
   confirmation read: a caller that has been answered writes its
   confirmation and carries on (`listener.c:573-576`), and if the answerer
   gave up meanwhile the healthy caller ends the job (plan §1, point 5).
   Instead keep callers that have not finished their three steps in a small
   set polled beside the listening socket: the hello read keeps its 0.1 s
   bound, which is safe because an unanswered caller retries; a confirmation
   read has none; a full set answers `NOT_YET`, which callers already retry.
   About 70 lines. The fuzzer that found the order,
   `build/scratch/review-r3/trust-kernel/fuzz_hello.py` and `drive_hello.sh`,
   becomes the test: a silent caller and a wrong-count hello from a host
   without the secret, and the real peer still joins. Ask whether
   `rendezvous_model.py` abstracts the listener as one caller at a time; if
   the set lets two callers' confirmations interleave, the model gets that
   case.

3. **A "Trust" paragraph** in the README (Part 4): a private cluster network
   is assumed; the secret protects joining the job, not its availability and
   not the traffic after the handshake; without it any host that can reach
   the ports can join.

Verification: four presets, `tier3` (the start-up tests over namespaces, the
N1 case), the refusal tests under load, and an adversarial read of the
listener change and the segment order by a fresh subagent, given the diff and
§3.15.

## Part 4: the README and the records

Not a phase after the code. The README is true of the tree at every commit
(the author's rule, "How to work"), so the sentences below travel with the
part that makes them true, in that part's branch, written against the code
as it is in that branch and checked before the merge. What is left for a
branch of its own, `release-readme`, is the documentation-only items - the
ones no code part makes true or false - and one last read of the whole README
as the stranger, after Parts 1 to 3 have merged. A Sonnet subagent can draft
each item against the code and report the line it rests on; the main session
reads the result. Line numbers are `b0e0b53`'s and will have moved.

**With Part 1.**

- The first command's time, what it runs, and the models' command (item 7);
  Python as optional, for the models and the counted stress build (item 6);
  the version and how to see it (item 8).
- Limits (`:334-342`) and the settings row (`:518`): `OSOE_MAX_THREADS`
  counts live threads that have called the library, a slot freed when the
  thread exits (item 9; the row's *checked* mark waits for Part 3).
- Limits, P3: `OSOE_AM_BUFFER_BUDGET` cannot shrink at the defaults - 1,025
  nodes fit and 1,026 are refused; raise the budget or lower the chunk; the
  notice prints once a node (item 4).
- `shmem.floor` (R2, item 5): prints its ratios; the pins are the project's,
  checked with `OSOE_TEST_FLOOR_PINS=1` on the machines they were set on.
- Model checking (`:467-474`, K6, beside item 2): under which memory models
  (sequential consistency and x86-TSO; the targets are aarch64), with the
  blocked record as the example of what that misses, and the deprecated
  active-set protocol as the one without a model in the tree. Design §3.14
  on the record's four words.
- The `osoe-stat --gc` sentence and the epilog line for Slurm,
  `rm -f /dev/shm/osoe.${SLURM_JOB_ID}.*`, beside `:609-610` (item 10).

**With Part 2.**

- "How a job ends" (`:612-620`): the job ends with the caller's status, every
  other PE leaving with 0; the caller's `exit` has
  `OSOE_ABORT_GRACE_SECONDS`, 10 s, after which the library ends it with
  stdout flushed (the plan's §1 point 2); a site with `WaitTime` or `srun -W`
  terminates the rest that many seconds after the first task leaves; beside
  `-K1` (`:173`), why it is now safe. The `osoe-launch` section on its
  status. Design §3.15 the same; the decisions index's "Global exit" line
  already says *decided* and loses that word when this merges.

**With Part 3.**

- The first `srun` example (`:70`) gets `-K1 --cpu-bind=none`, which the
  README requires ninety lines later.
- "Every node must resolve the names" (`:169-170`): and a node's name must
  resolve to the same address on every node (3b).
- Start-up (3a, 3c): which failures the library ends by itself and which need
  the launcher (a PE that never reached the library; another node, until
  option 4); the notice; the "Trust" paragraph; "a job that disagrees ends
  naming the variable" (`:497-500`, `:351-352`) made exact. The rows compared
  PE by PE marked *checked*, `OSOE_MAX_THREADS` among them. Design §6.2 on
  which failures end the job, §3.15 on the notice; the start-up row's
  "⌈log₈ n⌉ levels" plus the mesh's n - 1 connections a lead.

**Part 4's own**, documentation only.

- `env.probe` (`:98-100`): "nothing else will work" is wrong for a one-node
  job, which needs no io_uring and runs.
- Limits, P2: a node's message rate and bandwidth between nodes are one
  thread's whatever its PE count; "line rate" was measured at 1 Gbit/s; the
  sharded threads of design §3.12 are not built; the per-message cost of one
  thread serving 256 PEs is unmeasured; the loopback figures the plan's §4.2
  gives.
- Limits, C4's seven (plan §4.4's table, every row but the first):
  `shmem_team_create_ctx` ignores `num_contexts`; `SHMEM_SYMMETRIC_SIZE=0`
  ends the job; `shmem_init_thread` never returns nonzero; the cache routines
  gone from the specification after 1.4, kept as no-ops (`:422-423` reworded);
  mismatched collectives among a node's PEs give wrong data and exit 0, named
  only between nodes ("loud, never hung" says where loud stops); exit without
  `shmem_finalize` ends the job with status 1; a wait woken by a plain store
  through `shmem_ptr` wakes at the 100 ms sweep. And the first row's one
  sentence, marked temporary as the author allows, true until the team work
  replaces it: every split claims a job-wide number, a split made on each
  node spends one per node, and a number costs about 200 KiB a PE, not 160
  (`:516`).
- Three settings rows (plan §1, point 10): `OSOE_WIRE_BYTES` and
  `OSOE_WIRE_BUFFERS` get their range, `OSOE_AM_BUFFER` its floor,
  `OSOE_MAX_THREADS` its note.
- RDMA (`:444-451`, R3): XRC and DC both named, DC as the later review's
  recommendation from recalled limits; in-network reduction is below the
  seam, which `internode.h:137-145` offers, not out of reach.
- Architecture (X1, X2): tested on aarch64; x86-64 builds and passes its
  one-node tests, the network path untested there; every node of a job must
  have the same architecture.
- Pi numbers (P9, P10, D5): the 180 µs barrier quoted with its binding
  (Slurm's default; 196-208 µs unbound, 4-17 % more on small collectives),
  the trade stated beside the `--cpu-bind=none` advice; the
  `OSOE_AGENT_SLICE_US` row carries both sides of the plan's §4.10 table.
- Every number a later run changed: the SOS count against the last sweep
  (`docs/pi-cluster-results.md`).
- **The records.** `docs/pi-cluster-results.md:27-29` ("every line within
  5 %") and `:533` (the slice's "no difference") corrected against their raw
  files and the plan's table; `docs/decisions/README.md`'s `OSOE_AGENT_CPUS`
  line, which says the default is `node` where the code and the README say
  `task`; design §4.11's default for `OSOE_MAX_THREADS` (16, not the CPU
  count); §3.15's bullet on the detector and a signal handler (it does end
  such a job); P4 (the intranode claim is measured to 16 PEs a node) and P6
  (`shmem_collect` reduces all P lengths through the tree first) where those
  claims are made.

Verification: a subagent checks every changed sentence against the line of
code or the record it names, and reports the ones it could not find - for
each part's sentences before that part merges, and once more for the whole.

## Part 5: the repository (R1, D9)

Branch `repository-scrub`. Mechanical, and the kind of change a green suite
cannot check: CLAUDE.md's rename rules apply - longest pattern first, and the
audit command shown to find something before its empty result is believed.
This brief does not spell the path, so as not to carry it.

1. **The home path**: 61 tracked files, 218 lines, all under `docs/` -
   review and design evidence, three finished briefs, three review prompts;
   four also carry an older path under the desktop. The repository's own path
   becomes `<repo>`; a path under the home outside the repository becomes
   `~`. The audit is the readiness review's `git grep` (its R1 section): 218
   lines before, none after but the placeholders this brief and the plan
   already use. A line in `docs/README.md` says the evidence was edited so,
   and when.
2. **The status page**: `docs/README.md:3` sends the reader to a page only the
   author can open; `docs/decisions/2026-09-26-overnight.md:164` names it
   too. The index's opening points into the tree instead (the decisions
   index for where the project stands and what waits on the author); the
   record's line stays, as history, with a note that the page is private.
3. **The release checklist**, which the status page held, becomes
   `docs/release-checklist.md`: the release point's six conditions (design §1,
   "Release point"), the decisions index's lines on it (`:14`, `:123`: the
   hour of `mix`), and what this brief adds - the `srun -K1` global-exit run
   (Part 2, item 5), the pins with `OSOE_TEST_FLOOR_PINS=1` on the author's
   machines, the `models` preset, the SOS sweep past 61 PEs once the teams
   are rebuilt. The move is the author's decision ("yes, move checklist",
   2026-10-02, item 5 of that day's record); the page's own items are the
   author's to add, so mark the file a draft until he has read it.
4. **The fixtures**: four of `shmem/tests/slurm-fixtures/*.env` carry the
   development VM's address and five its host name. They are captured
   environments, and `test-bootstrap.c` may check those values: replace them
   only where the tests do not depend on them, and otherwise say in
   `slurm-fixtures/README.md` whose machine they name.
5. **The specification's examples** kept as evidence - four, the
   repo-hygiene report names them - get the notice the specification's
   permission asks for, in the form the other kept examples carry. Not an
   SPDX line: they are not the project's.
6. **SPDX lines** on `dev/lima/create.sh`, `dev/lima/osoe.yaml` and the 154
   evidence programs under `docs/design/evidence/`, so that the README's
   "every source file" is true; the audit is
   `git grep -L SPDX-License-Identifier` over the source extensions, which
   must name 156 files before and the specification's examples alone after.

Verification: the audits, before and after, in the commit message; the four
presets once, since the fixtures and `dev/lima` are built or read by tests.

## Part 6: the whole

On `main` with Parts 1 to 5 merged: the four presets, `models` and `tier3` in
`osoe`; the suite under eight busy loops in `osoe-load`; and the stranger's
path in `osoe-load` from a clean clone - the README's build command with and
without Python, `cmake --install`, `ring.c` built with the installed `oshcc`
and run with `srun -K1 --cpu-bind=none -N1 -n4`, the global-exit example
under `srun -K1`. Then remove `build/scratch/wt/review-r3`, the `osoe-load`
and `osoe-x86` VMs, and every worktree this brief made.

## Not in this brief

- **The teams**: `docs/design/team-resources-proposal.md`, two steps on
  briefs of their own, before the tag - step D's is
  `phase1-step7-slice3-prompt.md`, step E's follows D's merge. Their work
  will touch `create_control`, `node.h`, `internode.h`'s `team` argument and
  the engine.
- **The release point's conditions** after the teams: the SOS sweep (now
  past 61 PEs), the Pi runbook end to end (the tree copied to the rebuilt
  cluster first), an hour of `mix`, the tag, the release note.
- **After the tag**, rated so by the review or the plan: a refusing node
  telling its neighbours in the rendezvous tree (3a's option 4,
  `rendezvous_model.py`); the rendezvous listener bound to every address (N4);
  a model for `active-set.c`; the x86-64 network path and an architecture
  check in the rendezvous; sharded progress threads (design §3.12).

## Stop and ask

- Part 2's property cannot be restated as written: some path has a non-caller
  leaving nonzero on a global exit, or the model finds an interleaving the
  two-line change exposes.
- Part 3c's set cannot satisfy both constraints - no answered caller dropped
  for time, no caller holding the loop - without a bound §3.15 forbids.
- A setting that only the lead can validate, because it needs what the lead
  alone knows.
- A `-k` refusal test reveals a hang the designs above do not cover: say
  which, with the launcher's output.
- A pin or a test that fails idle on `osoe-load` after a change.
- Anything that would change `internode.h`, the wire format, the control
  segment's layout number beyond Part 3's, or a modelled protocol other than
  the two named.
- The scrub meets a path in a tracked file that a placeholder would make
  misleading - a command the reader is meant to run as written.
- The host disk under 10 GiB free.

## The record

- A dated decisions file, `docs/decisions/2026-10-<dd>-release-fixes.md`, in
  the form of the ones beside it: each of D1 to D9 as taken and by whom;
  what each part built, found and measured (the `srun -K1` table; the
  default build's time with and without the models; the stopped-bulk test's
  timings with and without the gate; the litmus's counts); every choice made
  without the author and how to undo it. The decisions index gets its lines
  in the same commits: global exit's status; the models' label; the version;
  the pins; `OSOE_MAX_THREADS` compared; the scrub's placeholders.
- The design notes where Part 4 says.
- `docs/prompts/README.md`'s rows filled in; when the last part has merged,
  the review lead's brief, the plan and this file move to `done/`.
