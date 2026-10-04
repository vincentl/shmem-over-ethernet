# Round four, question 2: is osoe correct, after the release fixes?

Review of `main` at `08b3da0`, run 2026-10-03 against
`docs/reviews/adversarial-review-prompt.md`, question 2 only, with the lead's
changes: the tree the release fixes left (Parts 1 to 6 of
`docs/decisions/2026-10-02-release-fixes.md`); the previous round's findings
(`phase1-release-correctness.md`, K1 to K6) checked as claims; Parts 2 and 3
(global exit, start-up) and Part 6's code read as new work. Labels:
*documented*, *observed in source*, *measured*, *recalled*. Every run was made
in the `osoe-load` VM (Debian 13, kernel 6.12, aarch64, 4 cores) from a copy of
the tree in its `/tmp`, under its lock; the models ran on the host (Python
3.14). The baseline and load runs were made by a Sonnet runner and their logs
read here; everything else was run and read in this session.

## Verdict

**Correct where the release fixes changed it, with one defect to fix before
release.** The global exit as rebuilt (Part 2) is sound within the limits it
states: one status rule on every path, waits that form no cycle and are all
bounded, an engine that keeps reading while it aborts, and a model whose
eighteen broken variants each fail by the property they name and whose eleven
expected-pass cases all pass with liveness (*measured*). The previous round's fixes in
this question hold: the "blocked in" record is right by C11's own argument and
its test fails when its loads are made plain; the silent-peer gate is now
tested and its test fails when the gate is removed; the README's model
sentence now names its exception and the memory models, though it still leaves
out the node's own start-up protocol, which has no model (The models). The
suite passes whole idle and under eight busy loops, 664
of 664 each; the newest work three times over under load, with one failure
that is the probe's own race; the debug and ThreadSanitizer builds of the
newest tests are clean.

The one defect worth fixing first is in Part 3: its protection against a
leftover of another launch by the same names covers the control segment and
not the job tables or the connection area, which a node-mate also opens by
name. A start by the same ids as one that failed - a requeued Slurm job without
the README's epilog - binds both leftovers, and then ends as a false
deadlock, hangs with the detector off, or refuses with a message about the
library's version (F1, *measured*). It is a few lines. The rest is minor or
after release: a lead that dies between making its segment and finishing it
leaves its node-mates waiting (F2), the relayed-goodbye fix's test sees its
defect one run in five (F3), `env.probe` races its own child (F4), and small
things (F5). For this question: release once F1 is fixed and the team
redesign has been through the same runs.

## Findings, ranked

### F1. A start by the same ids binds the job tables and connection area a failed start left (fix before release; high confidence; *measured*)

- **Location**: `shmem/src/job.c:209-265` (`follow`) and
  `shmem/src/connection-area.c:278-445` (the non-lead's open). Part 3 taught
  the control segment to tell a leftover of another launch by the same names
  (`node.h`'s `launch`, layout 28; `node.c:996-1033`), after its adversarial
  read found node-mates reading a leftover's `failed` 4 times in 10. The job
  tables (`….job.<session>`) and the connection area (`….conn.<session>`) have
  the same exposure and no such check: a non-lead opens each by name as soon
  as it is sized and `ready`, checks magic, layout and sizes, and uses it.
- **Scenario**: a start fails after a node's lead has made its job tables and
  connection area and before every PE of that node has opened them. That is
  what `srun -K1` does to the survivors of a start-up failure: the previous
  round found each node's `.conn.0` and `.job.0` left in 8 of 8 tries
  (`phase1-release-readiness.md:192-204`). A later start by the same ids then
  begins - a requeued Slurm job, which keeps its job id and starts its steps
  from 0 again (*documented*, `bootstrap.c:112-122`), on a node without the
  README's epilog; or a hand-set `OSOE_JOB_ID`. Its non-leads reach `follow`
  while their lead is still in the TCP rendezvous, find the leftover tables
  ready and take them, then take the leftover connection area the same way.
  Each increments the leftover's `opened` count and so removes its name, and
  the lead later creates fresh objects without a word. The non-lead's internode
  traffic then goes into a connection area no engine reads.
- **Evidence** (*measured*, release build, map `0,0,1,1`, a fixed `OSOE_JOB_ID`
  given to every rank as `start-same-ids.sh` does; leftovers made by
  cancelling `shmem_test_stalled-start` as `start-leftovers.sh` does, which
  leaves node 0's `.job.0` and `.conn.0`, both ready):

  | Leftovers | Second start by the same ids | Result |
  |---|---|---|
  | none (control) | `osoe-pacer --quick`; `shmem_test_internode` | pass (`failures=0`, 3.4 s); pass |
  | both | `osoe-pacer --quick` | ends after 10 s as a **deadlock**, "PE 1 in shmem_atomic", every other PE in a team sync |
  | both | `shmem_test_internode`, 4 runs | each ends after 10-12 s as a **deadlock**, "PE 1 in shmem_quiet", the others in a barrier |
  | both | the same, `OSOE_DEADLOCK_SECONDS=0` | **hangs** to the launcher's 30 s timeout, silent |
  | tables only | `shmem_test_internode`, 3 runs | fails at once: the non-lead used the old tables, and the new object's name outlived init (`test-internode.c:355`'s invariant) |
  | area only | `shmem_test_internode`, 3 + 3 runs | pass: the lead, a step ahead, reclaims it first |
  | both, from a three-node start | a two-node start, 2 runs | ends: "PE 1: /osoe.….0.0.job.0 is not this job's tables in this version (magic 0x626f6a656f736f layout 8)" |

- **Why it matters**: the README says a later start with the same ids "takes
  over what a process now gone left there" (`README.md:952-954`, and `:978`).
  It does for the control segment; for these two objects it misdiagnoses
  itself - a deadlock, naming the PE whose traffic was lost, or a version
  mismatch - or hangs, against "loud, never hung" and "failures name what
  failed". Its precondition is rare - a requeue onto a host whose node index is
  unchanged, with no epilog - which is why it is not rated higher; it is rated
  fix before release because it is the same defect Part 3 chose to fix before
  release, the fix is small, and the README's sentence is false meanwhile.
- **Fix**: what Part 3 did for the control segment - the launch in both
  headers, and a non-lead that waits past an object of another launch until its
  lead reclaims it - or, simpler, a non-lead that opens neither object until
  this launch's control segment, which it already holds, says its lead has made
  them (a word beside `lead_agent_running`). A test: `start-leftovers.sh`'s
  leftovers, then a start by the same ids running `shmem_test_internode`.

### F2. A lead that dies between making its segment and finishing it leaves its node-mates waiting (minor; high confidence; *measured*)

- **Location**: `shmem/src/node.c:1003-1047`, `open_control`'s second loop,
  the wait for the lead's `ready`. Part 3's adversarial read gave the node's
  count waits a check of the lead's pid (`lead_ended`, `node.c:359-363`, called
  from `wait_count`); this wait, which comes first, has none.
- **Scenario**: the lead dies by a signal after `create_control` has made and
  sized the segment and before it stores `ready` (`node.c:1285`) - while it
  creates and maps its own heap. Its node-mates have mapped the segment and
  wait on `ready`; `failed` never comes, the name is never removed, and they
  wait until the launcher ends them.
- **Evidence** (*measured*): an `LD_PRELOAD` shim that raises SIGKILL when a
  process creates a name containing `.heap.`, given to rank 0 alone, three PEs
  on one node, `osoe-launch -k -t 25`, `OSOE_TEST_NOTICE_SECONDS=3`: rank 0
  killed by signal 9; ranks 1 and 2 alive at the launcher's timeout, PE 1
  saying four times "has waited N s for the local lead, rank 0, to finish the
  node's control segment ..., which it has created; still waiting".
- **Why it matters**: the README says the lead's node-mates "end by themselves
  if its process ends" (`README.md:1017-1018`). Here they are loud but never
  end, and the notice says the lead is still at work. Under `srun -K1`, which
  the README requires, the lead's death ends the step anyway, and the window
  is a heap's creation. Fix: `lead_ended` at this loop's sweep too, with the
  pid loaded atomically there - the lead writes `creator_pid` before `ready`
  (`node.c:809`), so a plain read before `ready` would race.

### F3. The relayed-goodbye fix's test sees the defect one run in five (after release; high confidence; *measured*)

- **Location**: `shmem.global-exit.kills.held-other-node.0-2-2-1`
  (`shmem/tests/CMakeLists.txt:2474-2487`), the only guard of the high finding
  of Part 2's second adversarial read: a node told of a global exit by a third
  ended its stream to the carrier's node at once, which took that for the
  node's end, and `srun -K1` killed its PEs (now `keep_stream`,
  `progress.c:898-900`).
- **Evidence** (*measured*): with the shutdown put back
  (`if (1 || !p->abort_keeps_stream)`), the test failed 4 runs of 20 - the
  record's own figure. As built it passed every run made here (idle, under
  load, three repeats). A regression of the defect passes a suite run four
  times in five.
- **Why**: the defect needs node 2 to hear from node 1 before node 0, and the
  carrier to leave before node 2's held PE; the test leaves both to timing. The
  model has two nodes and relays nothing, so nothing else covers it.
- **Fix**: make the relay certain - a test hook that delays the carrier's
  node's goodbye to node 2 until node 1's has gone - so that the mutant fails
  every run; or give `abort_exit_model.py` a third node in one small case.

### F4. `env.probe`'s check of reopening an unlinked object races its own child (minor; high confidence; *measured* once, mechanism *observed in source*)

- **Location**: `tools/probe/probe.c:178-219`. The parent stores 1, forks,
  sleeps a fixed 50 ms and stores 2; the child, after `munmap`, `close`, an
  `open` of `/proc/<parent>/fd/<n>` and an `mmap`, requires its first load to
  read exactly 1 (`:203`), and exits 4, "map or read", otherwise.
- **Evidence**: under eight busy loops the runner's newest-tests run failed
  `env.probe` once, `FAIL unlinked shm object reopened via /proc child_exit=4`
  (1 of 8 executions of the test; 0 of 815 direct runs, idle and loaded). A
  child slower than 50 ms to its first load reads 2.
- **Why it matters**: Part 6 tells a site to run `env.probe` on every node, and
  this false "PROBE FAILED" says `osoe-stat` cannot read a running job. Fix:
  accept 1 or 2 at the first load, or store 2 only after the child says it has
  mapped.

### F5. Minor

- **The model tests check pass or fail, not which property fails**
  (`tools/modelcheck/engine.py:645-662`, `report`; *observed in source*). An
  expected-failure variant passes the suite whatever it violates - STUCK in
  place of NO_KILL, say - which is how a check stops being reached without a
  test noticing, the defect the project has found twice. Today every one of
  `abort_exit_model.py`'s eighteen expected failures fails by the property it
  names (*measured*, below); the guard is a person reading the output. A
  `CASES` entry could carry its expected message and `report` compare it.
- **`launch` is read while the lead may be writing it** (`node.c:1006`,
  `:275-277`): when a node-mate's last word sets `failed` in the lead's segment
  before the lead has written its header (`node.c:817`), another node-mate
  reads the plain `launch` with no happens-before to its write - a §3.16
  violation by the letter, benign in effect (a zero read as another launch's
  costs a 10 ms detour, then `failed` ends the PE). An atomic field, loaded
  with acquire, removes it.
- **Carried over from the previous round's K4, unfixed and unplanned**:
  `transport.h:470` still tells a second backend "Intranode and AM: one
  acquire load" for `osoe_transport_load`, which is `seq_cst` in the code,
  whose comment says why acquire loses wakes (`transport-am.c:1931-1955`) - the
  one a porter would copy; `reply.h:239`'s relaxed store; §3.16's table still
  listing the wire's `produced` and `consumed` (`shmem-ethernet-design.md:2310`,
  `:2327`).

## The previous round's findings, as fixed

| | Finding | What the fixes did | Holds? |
|---|---|---|---|
| K1 | the "blocked in" name read torn | four atomic words, release stores and acquire loads, one loader shared by both readers (`blocked.c`, `base/atomic-name.h`); `shmem.blocked-record` | **Yes.** `blocked.c`'s two arguments are C11's own: a later publication's release store read by an acquire field load orders that publication's odd bump before the second generation load; an even bump read by the first load orders its stores before the field loads. osoe-stat's reader uses acquire loads and the same loader (`tools/stat/segment.c:477-557`); nothing else touches the field. With the readers' loads made plain the test failed 3 runs of 3 (*measured*: 156 torn of 243 million accepted by the library's reader, 0 by osoe-stat's), as the record says. The writer's half and osoe-stat's path rest on the argument alone, which the README says. |
| K2 | detector: §3.15 wrong on the signal handler; no test sees a second collection | the design text corrected (`shmem-ethernet-design.md:1869-1878`) | The text, yes. The one-collection gap was rated after release and remains. |
| K3 | `shmem.reply` spins and times out under load | waiters watch with loads, then sleep on the credit's word | Yes, as a test: it passed in the whole suite under eight busy loops. Its own record says the mutant it guards is caught 58 runs of 90 under load - an idle guard, which its comment says. |
| K5 | nothing tests the silent-peer gate | `shmem.stopped-bulk.0-0-1` | **Yes**: with the gate removed (`wire.c:322`) the test failed 2 runs of 2, the job ending "node 1 stopped answering: its kernel has acknowledged nothing sent to it for 4 s" (*measured*). |
| K6 | "every protocol checked exhaustively" | the README names the active-set exception and the memory models (`README.md:758-777`) | Yes, but for the protocol Part 3 grew (below). |
| K4 | minor atomics notes | not planned | Unchanged (F5). |

## The new work

**Global exit (Part 2): sound as built, within its stated limits.** Read in
full: `abort.c`, `liveness.c`'s `osoe_liveness_outlast`, the engine's
`keep_stream`, `osoe_progress_peers_ended` and `osoe_progress_host_abort`, the
agent's loop, `init.c`'s exit handler, `fatal.c`. What convinced me:

- One status rule, computed once from the word and `called`
  (`abort.c:130-132`), on every path that ends a process on an abort: the
  agent, the grace thread, a fatal that loses (`fatal.c:94-97`), an exit
  handler that finds the word (`init.c:207-229`). `called` is a release before
  the declaration's CAS and an acquire after the agent's acquire of the word:
  message passing, rightly not `seq_cst`.
- The waits form no cycle: the carrier waits for its node; a lead for its node
  but the carrier; only the carrier's node's lead waits for other nodes, and
  never for one whose goodbye named another caller (`progress.c:1351-1361`).
  Every wait is bounded, takes no lock and allocates nothing; the grace
  thread's plain statics are published to it by `pthread_create`; two threads
  waiting at once each hold their own pidfds.
- An aborting engine keeps receiving (`progress.c:1212-1216`), so a peer's end
  of stream is seen; an error counts as an end; a reset after a goodbye is not
  a fatal (`progress.c:609-612`).
- The model matches the C on every rule I compared - who waits for whom, the
  exclusion, the statuses, the stand-aside, the connecting lead - and models
  neither the bounds nor a third node; the tests take those (F3).
- osoe-launch counts a rank it killed itself as 0 where `srun -K1` counts 143;
  the tests compensate by requiring no "killed by signal" line
  (`global-exit.sh:63`, `:99`), so they see what `srun` would.

**Start-up (Part 3): sound but for F1 and F2.** The last word reaches the node
on every path I traced (`tell_refusal`: the PE's own segment, a tombstone, or
the lead's segment within 10 s); the lead never overwrites `failed` (`ftruncate`
zeroed it, `node.c:807`); the control segment tells another launch's leftover by
its `launch`, read after the acquire that published it except in F5's case; the
`/dev/shm` refusal goes through the segment; every setting is read by every PE;
the notices end nothing (`base/stall-watch.c`). The heaps and statics cannot be
taken from another launch: each PE creates its own, reclaiming a leftover, before
it counts itself joined, and nobody opens another's before the count. The
listener's set of sixteen keeps an answered caller until its confirmation,
reads exactly the hello and the confirmation, and returns only callers of the
current purpose; for a healthy job I found no way to lose or double-count a
caller. Its availability against a stranger is the trust question; the
README's Trust paragraph states it (`README.md:1028-1037`).

**Part 6's code: sound.** The agent's start waits without a bound, sweeping for
the abort word, the local PEs' liveness the agent's once `running` is set; the
io_uring check enables, enters, probes and registers as the agent's first pass
does (`base/ring-check.c`). The deadlock detector's logic is unchanged but for
the record's reader; no new wait or post path is counted or uncounted by the
fixes, and its collection still needs an answer from every node, so a node
still starting cannot be part of a declaration.

**The atomics (§3.16).** Every atomic the fixes added names `acquire`,
`release` or `seq_cst`, as the rule for its shape requires; none is relaxed,
consume or a fence (*observed in source*, every added line of the diff
`b0e0b53..08b3da0` under `shmem/src`, `base`, `tools/stat`, `tools/launch`).
New plain fields read across processes - `launch`, `creator_pid`, the
tombstone's header, `pid` - are written before the release that publishes them
and read after the acquire, but for F5's `launch`.

## The models

- **`abort_exit_model.py`**, rebuilt by Part 2 and never recorded as output (the
  record's prose is its evidence): each of its eighteen expected-failure cases
  fails by the property its variant names - NO_KILL for `called_carries` (both),
  `no_carrier_wait` with kills, `no_lead_wait`, `no_remote_wait` and the `both`
  residual; JOB_STATUS for `caller_zero`; STATUS for `status_everywhere`,
  `caller_exits`, `pidfd_fatal`, `no_init_wait`; ONE_END for `finalize_returns`;
  STUCK for `wake_first`, `exit_everywhere`, `wait_every_node`, `no_exclusion`,
  `no_carrier_wait` without help, and the known `caller_exiting` (*measured*).
  All eleven passing cases pass with liveness; the largest, two callers on one
  node, at 9,598,056 states in 419 s, the record's "9.6 million".
- **`blocked_record_model.py`**: its output is identical, state counts included,
  to the committed `docs/design/evidence/blocked-record/blocked_record_model.out`;
  its four breakages fail; its docstring now says what it cannot see (aarch64).
- **`abort_model.py`** changed only in its docstring. The other models and the
  six of `SHMEM_THREAD_MULTIPLE` were not re-run: their code and the protocols
  under them are unchanged since the previous round checked every one, and the
  record ran all thirty on this tree on 2026-10-03.
- **Protocols without a model, and whether they need one.** The node's own
  start-up over `/dev/shm` - `ready`, `joined`, `mapped`, `failed`, and now the
  last word, the tombstone, `launch`, `lead_ended`, and the job tables and
  connection area opened by name - has none: `rendezvous_model.py` is the
  leads' TCP tree. The README's "every lock-free and futex protocol but one"
  (`README.md:758-768`) leaves it out, and F1 and F2 are both in it. It needs a
  rule more than a model - every object a node-mate opens by name is its own,
  or is announced by this launch's control segment - and the README's sentence
  should name it. The listener's set is single-threaded socket code and needs
  none. The relay of a goodbye through a third node needs a case (F3).

## On the team redesign (kept apart: not built yet)

Read: `docs/design/team-resources-proposal.md` §3 and §6, and
`docs/prompts/phase1-step7-slice3-prompt.md` Parts 1 and 2. The argument for
giving a number back ("Why giving a number back is safe") holds as far as I
could push it: the free mask leaves a number out while a block is mapped under
the PE's name; two teams that share a number on a node have different leads
and so different names; a message of the old team cannot reach a node after
that node's members have left it; the 32-bit generation in the 64-bit handoff
cannot wrap in practice. One gap, which the brief would let through:

- **A forward still queued in the engine when its block is reset.** Today a
  barrier forward's descriptor carries the team and the epoch, and the message
  is built from the descriptor alone when the control queue drains
  (`progress.c:686-712`, `forward` at `:258` and `:310`): self-contained. D
  adds the receiving lead's local id to the message (Part 1 item 7) without
  saying when the sender works it out. Worked out at drain time from the
  block's state, a RELEASE for the old team still queued toward a child -
  that connection's buffer full, the child slow - would be built from the new
  team's state if the block were reset meanwhile. That is reachable: the new
  split's parent need not include the child's node, so the split, the block's
  reuse and the new team's first handoff can all happen while the forward
  waits. The proposal's "late message" argument covers a RELEASE "still
  travelling", not one still queued. **Recommendation**: the forward's
  descriptor carries the message as it will be sent - number, receiving lead's
  local id, epoch - computed when it is queued (a barrier forward leaves the
  descriptor's `target_pe`, `length`, `offset` and `token` unused), the drain
  reading nothing of the block; and
  `team_block_model.py` gains the case of a forward queued across the reset.

## Runs

| Run | Result |
|---|---|
| release, idle, `-j4`, models out (runner) | 664/664, 133 s |
| release, eight busy loops (runner) | 664/664, 185 s; `shmem.strict-c11` 85 s of its 600 |
| the newest work - `global-exit`, `start-up`, `refuse`, `leftovers`, `stat`, `blocked-record`, `stopped`, `reply`, `probe`, `io-uring`, `deadlock`, `launch`, `am-size`, `fatal`, `launcher-signals`, `version`, `agent`: 159 tests - `--repeat until-fail:3` under eight loops (runner) | 158/159: `env.probe` (F4); again whole, 159/159 |
| the same 159, debug build, idle (runner) | 159/159 |
| the same, ThreadSanitizer build, idle (runner) | 144/144 (15 not registered under tsan); no report in a `-V` rerun |
| `abort_exit_model.py`, every case, host | as above |
| `blocked_record_model.py`, host | identical to the committed evidence |
| F1's experiments | as tabled |
| F2's shim | as described |
| mutants, one build: the relayed goodbye's shutdown put back; the silent-peer gate removed; the name readers' loads plain | `kills.held-other-node.0-2-2-1` failed 4 of 20; `stopped-bulk.0-0-1` 2 of 2; `blocked-record` 3 of 3 |
| `osoe-pacer mix`, 8 PEs on the cyclic map `0,1,2,3,0,1,2,3`, 150 s, seed 3 | `failures=0` in 158.7 s; the team periods moved data between node-mates this time (`ptr_s` nonzero, atomics all local), which block maps never do (the previous round) |

**For the lead to run** (longer than ten minutes): the `models` preset whole,
and `-DOSOE_MULTIPLE_MODELS=ON`'s six, on the tree the teams leave - both are
the record's runs to repeat, not doubts about this tree.

## Sources

`shmem/src/abort.c`, `abort.h`, `liveness.c`, `progress.c`, `progress-host.c`,
`agent.c`, `init.c`, `fatal.c`, `node.c`, `node.h`, `job.c`,
`connection-area.c`, `listener.c`, `bootstrap.c`, `blocked.c`, `wire.c`,
`shm-object.c`, `transport-am.c`, `transport.h`; `base/atomic-name.h`,
`base/stall-watch.c`, `base/ring-check.c`; `tools/stat/segment.c`,
`tools/launch/launch.c`, `tools/probe/probe.c`; `tools/modelcheck/engine.py`,
`abort_exit_model.py`, `abort_model.py`, `blocked_record_model.py`,
`rendezvous_model.py`; `shmem/tests/global-exit.sh`, `start-same-ids.sh`,
`start-leftovers.sh`, `test-stalled-start.c`, `CMakeLists.txt`; `README.md`;
design §3.15, §3.16; `docs/decisions/2026-10-02.md`,
`2026-10-02-release-fixes.md`; `docs/prompts/phase1-release-fixes-plan.md`;
the previous round's `phase1-release-correctness.md`,
`phase1-release-readiness.md`, `phase1-release-review.md`. The runner's logs and
this session's scripts are outside the repository, in the reviewer's scratch
directory; the VM copies are removed.
