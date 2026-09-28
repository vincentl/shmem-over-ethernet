# 2026-09-28, overnight: the scan pipelined, a deadlock proved, a vanished node found, and the tree made ready for review

The author's last answers of the night of 2026-09-27, and then, going to bed:
"Head towards getting to the adversarial review checkout. Keep testing and
looking for issues. Refine documentation. When you believe the project is in a
great state for a review and then release, stop." This record says what each
answer became, what was measured to settle what the answers left open, and
what was found on the way - a stale state in the new detector among it.

## What the answers asked for

| Item | The author | What was done |
|---|---|---|
| 1. Slurm binds by a CPU mask: reset it on the library's threads? | "can't we reset the cpu mask on all spawned background threads?" | built, measured, and made an option rather than the default (item 3) |
| 2. RemoveIPC | the library must never alter a system's configuration; document suggestions only | nothing in the library alters any; the runbook's advice to administrators stands; the `/proc` fallback for a name removed at start is not built (For the author, 3) |
| 3. Deadlock detection | "build it" | built (item 2) |
| 4. The scan at 1 MiB | "pipeline it" | built (item 1) |
| 5. A larger landing zone | its memory cost at scale? | answered at the time: per PE, so per node it grows with PEs a node, never with nodes - 256 KiB a PE is 64 MiB at 256 PEs; kept, and the refill recommended instead, not built (For the author, 4) |
| 6. The licence | "version 3 or any later version" | what every file's SPDX line already says |
| 7. The other session's commit | evaluate, recommend a disposition | its fourteen index rows are on main with six more (item 6); its worktree and branch are the author's to remove |
| Branches | clean up | 47 merged branches deleted that evening, and each of the night's worktrees and branches once it merged: left are `main` and the other session's |

## 1. The scan's pieces in flight (item 4)

`8deb4ec`, merged. A scan over a team spanning nodes sent its 4 KiB pieces one
at a time, each all the way up and down the tree before the next. Now each
member keeps D in flight - D the reductions' agreed `OSOE_COLLECTIVE_PIPELINE` -
by five rules: send piece q up only once piece q - D's down is taken (the
window); copy a down out before counting it; one counter per child position;
take a piece down only after taking it up, since the downs carry the totals of
the children before each; keep the source and the children's totals for the D
pieces in flight. `tools/modelcheck/scan_pipeline_model.py` checks it - eight
cases, scans back to back on one team and on two trees, D from 1 to 3 - and a
breakage of each rule fails (its first run's failures were the model's own
late-binding closures). The scans' area doubles at D = 2, to 2.6 MiB of address
space a PE, touched only by scans; each member publishes the most pieces it had
in flight (the PE slot, layout 25), and the test pins it at every leaf.

**On the Pi**, 4 × 4, a 1 MiB scan of longs, the slowest PE's median of 21,
every result checked:

| | scan | reduce, beside it |
|---|---|---|
| before, `3d5bbe1` | 139.3 ms (136.5 again) | 30.1 ms |
| D = 1 | 145.1 | 49.5 (the reductions at D = 1 too) |
| D = 2, the default | 83.5 | 29.9 |
| D = 4 | 59.0 | 29.6 |
| D = 8 | 47.9 | 30.4 |

The floor is about 36 ms: node 0's link carries 4 MiB each way.

**Verified** at `8deb4ec`: debug 574, release 597, asan 573 and tsan 368 tests;
the scan's, the pipeline's and the pacer's 126 tests thirty times over under
eight busy loops; two mutations - one piece too many in flight, and the root
taking a piece down before up - each failing nine of the twelve scan tests on
a wrong value (read, not assumed: a failure in 0.1 s could have been a crash).

## 2. A program's own deadlock ends the job, once it is proved (item 3)

`470044d`, with `b986ef9`, `ff4e96d` and `f561e2d`. The proposal (the
previous record, item 4) is built, changed where building it found it wrong:

- **A wait is counted at the sleep, not for the routine.** The proposal counted
  what the "blocked in" record counts, and a barrier's record spans its
  arrival: a PE counted as waiting could still post, after its node's sample,
  a message that wakes another node after that node's. So every sleeping loop
  - the epoch waits, the region and many-variable waits, the transport's seven
  - counts a thread from just before it first sleeps until the wait ends, and
  a thread between the two only looks and sleeps (`base/sweep.h`). The model's
  `coarse_waits` breakage is that bug, and it is found.
- **A sample is two halves with a pass between.** The engine cannot see what
  its PEs posted and it has not yet taken - a collective's arrivals - so a
  sample reads the PEs' words, then `/proc`, lets the engine pass, then reads
  `/proc`, asks the engine, and reads the words last; nothing may move in
  between. The model's `no_pass` breakage is the one-step sample, and it is
  found.
- **`/proc` says whether each thread sleeps**, since a thread woken and not yet
  run still counts as waiting; and that a PE has no thread the words do not
  count. A thread awake in a sample - a sweep's, for microseconds - is looked at
  again a millisecond later, for a few milliseconds, rather than failing the
  round: under four tests at once the first build took 9.7 s to find a
  two-node deadlock at `OSOE_DEADLOCK_SECONDS=1`, the second 2.3.
- **The root asks every node directly**, rather than up the collective tree as
  proposed: a round is a message to and from each node, twice, at most once a
  second and only while the root's node looks quiet. It is O(N) at the root a
  round where the tree would make it O(log N); at hundreds of nodes that is
  hundreds of small messages a second in a job that looks deadlocked, and the
  tree is the refinement when a job that large runs (design §3.15).
- **Two settings, not three.** `OSOE_DEADLOCK_SECONDS`, default 10, 0 off; the
  proposal's "report only" is not built, since a proved deadlock cannot move
  and a report without an end leaves the job to the launcher's timeout (For
  the author, 1).
- **A sample is patient for 5 ms, and rounds back off** (`ff4e96d`). Read
  again: a threaded program can keep, on every PE, a thread waiting in the
  library while another computes, and a count of waits alone then sees every
  node quiet. The root began a round every second, and each node's sample,
  finding a thread awake, looked again every millisecond for up to a sweep
  interval with its agent asleep between: a tenth of the node's progress
  engine, in a job that was not deadlocked. Now a half waits 5 ms at most and
  each failed round doubles the wait before the next, up to
  `OSOE_DEADLOCK_SECONDS`, until the root's node is seen to move. A test,
  `busy`, holds every PE in that shape, on one node and two.
- **After an adversarial read** (`f561e2d`). A reviewer given the code and
  the argument, and told to find a way it ends a live job, found none a
  realistic program reaches, and four corner paths. Closed: io_uring's
  workers are left out by the kernel's `PF_IO_WORKER` flag, not their `iou-`
  names, which a program's thread may take (the helper test's thread is now
  so named, and told apart by name both helper tests are wrongly declared);
  and the premise that a waiter finds a wakeless change within a sweep is
  checked - each PE's sweep count must rise between a node's two samples by
  its waiting threads - though no test can hold a timer back on demand, so
  that one is argued: a timer slack of three seconds still fired within one.
  Named, as outside what OpenSHMEM defines: `siglongjmp` out of a wait,
  cancellation in one, a forked child that waits. And three small ones: a
  writer waiting for room stops being counted before it nudges the lead; a
  sample asks for acknowledgements only once every PE has a thread waiting;
  the backoff resets when the root's node moves between the collections.
- **Its state starts afresh at every init** (`b986ef9`). Read again after the
  first commit: a probe that reached a node as it closed left it answering, and
  after a second `shmem_init` its first turn sent the old round's report to a
  root that took it for a broken protocol and ended the job. Now nothing
  survives an init, and a message the protocol never sends is dropped rather
  than fatal - a detector that misses a deadlock costs a hang, one that ends a
  live job costs the job. A test declares in a second session.

`deadlock_model.py`: three nodes, eight scripts, and seven breakages, each
declaring a job that could still move. `test-deadlock.c`: five deadlocks ended
in about 2.3 s at one second - waits on one node and on two, a second session,
a collective that does not match on two maps - and seven jobs that only look
deadlocked run to their ends: the last PE computing, asleep outside the
library, or waiting for a thread of its own, and every PE waiting while a
thread of its own computes. Tier 3 runs a deadlock and a near-miss over its
three shaped links. Off, a deadlock runs to the launcher's timeout; a setting
it cannot read is refused by name.
ThreadSanitizer runs a thread of its own in every process, which the detector
counts as a thread it cannot see, so the four must-declare tests stay out of
the tsan preset. Checked, not assumed: a PE built with tsan has three threads
where one built without has two, and a deadlock at one second ran undeclared to
the launcher's 15 s timeout.

**The whole suite at the detector's most eager**, `OSOE_DEADLOCK_SECONDS=1`
exported: in debug, 589 of 591 passed, and of the two, one was a real deadlock
the test holds to the launcher's timeout (`exit-without-finalize.start_pes`,
which the detector now ends first), and the other the §4.10 boundary check,
which found `deadlock.c` in no layer's list - classified as runtime. In
release, 614 of 615: the same deadlock alone, and its report exactly what a
reader needs - PEs 0 and 2 in `shmem_barrier_all` at epoch 2, PE 1 in
`shmem_finalize`.

**Verified** at `470044d`: debug 591, release 615, asan 590, tsan 381; the
detector's and the pacer's 111 tests ten times over under eight busy loops, the
pacer's at one second, every run passing; two mutations, each caught - `/proc`
never consulted, which the helper tests saw declared wrongly, and a region
wait never counted, which the four that must declare saw never declared. The
tree that merges is verified again whole (Verification).

## 3. The progress thread under Slurm's binding (item 1)

`187c569`, from the evening: every agent asks for a 100 µs EEVDF slice
(`OSOE_AGENT_SLICE_US`). Three `osoe-bench` runs of each setting, alternating,
settled the one worry left, a 6% slower small put in a single pair of runs: it
was noise, and every separated difference favoured the slice - a 64 KiB
reduction 2,388 against 3,155 µs, alltoall 9% faster - while a bound mailbox
ran 62,900 messages a second against 43,300.

`5f99dd3`: **the CPU mask, as the author asked** - every PE publishes the CPUs
its task was bound to (layout 27), and each agent may widen itself to their
union, the job's own cores on the node and never another's, with nothing of
the system touched. Measured on the Pi, 16 PEs bound one a core:

| a mailbox, 60 s | messages a second |
|---|---|
| neither | 42,900 |
| the agent widened | 51,300 |
| the slice | 57,500 |
| both | 58,500 |
| unbound, either | 259,000 |

and `osoe-bench`, two runs each alternating, with the slice: widened, every
small collective was slower in both runs - an 8-byte reduce 223 against 200 µs,
a broadcast 221 against 197, a barrier 191 against 178, a 64 KiB reduce 2,819
against 2,385. So the widening is `OSOE_AGENT_CPUS=node`, off by default: for
a kernel before 6.12, which ignores the slice, running a program bound by
point-to-point messages. The agent is now named `osoe-agent`. A first soak had
started on the widened build as the default; it was stopped after 48 s and
started again on `5f99dd3`.

## 4. A vanished node with data in flight (found by tier 3)

`d645545`. The whole of tier 3, run for the first time on the detector's
tree, failed `tier3.*.vanished-node` on all three shapes: node 1's link was
cut, node 1 found node 0 gone by keepalive, and node 0 never found node 1 in
the minute the test allows. Node 0's PEs all waited in a barrier, so its lead,
the detector's root, looked quiet, began a round a second later and probed
node 1 - data in flight toward a vanished node, which TCP keepalive does not
probe, so only the kernel's retransmission limit could end it, about fifteen
minutes. With the detector off the test passed, as it always had: the
earlier verifications ran tier 3 in subsets that left it out.

The design had accepted those fifteen minutes for data the program itself
sent (§3.15). Now the lead asks the kernel, of the data, what keepalive asks
of an idle connection: once a keepalive interval, `TCP_INFO` on each
connection, and a peer whose kernel has acknowledged nothing for keepalive's
whole budget while segments wait ends the job, naming it. A peer merely
stopped is not found, as keepalive does not find one - its kernel
acknowledges what arrives, and a full receive buffer is probed through a zero
window with nothing outstanding; `shmem.stopped-node.keepalive`, a node
stopped for 6 s against a 4 s budget, still completes its quiet. The nine
vanished-node tests pass in about 6 s each, three of them new: data put
toward the node after its cut, the case that took fifteen minutes.

## 5. The soak

The release checklist's hour of `mix` (the author's answer of the night
before), on `5f99dd3`, 4 × 4 on the Pi cluster, Slurm's default binding, the
detector on at its default: 3,608 s, `failures=0`,
504 MB sent by the library, and no deadlock declared. A second hour, on
`f561e2d` - with the silent-peer check, which must never take a busy node for
a vanished one - 3,603 s, `failures=0`, 605 MB sent, and nothing declared
deadlocked or vanished. Both hours count as evidence, not as the checklist's:
the checklist's hour runs on the tree that is tagged, after the review.

## 6. The documentation, for the review

- **The README**: the detector (a limit replaced by what osoe now does, and
  the programs that should turn it off), the scan's pipelining (a limit gone),
  the progress thread's two settings with what binding costs, a vanished
  node found whether or not data was in flight, and the tests.
- **The design notes**: §3.15's detector and the silent-peer check, §3.11's
  slice and CPUs as built, and §10's small-message item answered by the Pi;
  the Pi's results (`docs/pi-cluster-results.md`) have tonight's section.
- **The runbook's SOS section**: its "what to expect" still listed the threaded
  programs and `shmem_team_context` as failures; they pass since step 8.
- **The prompts index** (`docs/prompts/README.md`): every finished brief now has
  a row - the other session's fourteen, step 4 slice 2 to step 10, and six more
  drafted by a subagent from the records and checked against them: step 5 slice
  7, step 8 slice 3 and the stress tester's four phases.
- **The review's prompt**, `docs/reviews/adversarial-review-prompt.md`, is now
  round three's: the library as its author means to publish it, judged on the
  README's claims, five questions and a verdict. The design round's prompt is
  kept as `docs/reviews/archive/design-review-prompt.md`.
- **A correction.** The record of 2026-09-26, evening, item 2, says "no test can
  show B1 on the VM's x86-64". The VM is aarch64, like the Pi: nothing here has
  yet run on x86-64, which the review's prompt now asks about.

## 7. How the work went wrong once

The detector's four-preset run builds from its worktree, and the next change
- the CPU mask - was begun in that same worktree on a new branch while the run
went on: its release, asan and tsan builds would have tested a mix. Caught
before any result was read; the run, its orphaned `presets-tree.sh` and a
nested strict-C11 build were stopped by PID, the work moved to a worktree of
its own, `build/` wiped, and the run started again. The rule is now in the
VM-runs memory: a run building from a tree owns it until it ends.

## Verification

At `f561e2d`, the tree that merges - the scan, the slice, the detector with
its three fixes, the agents' CPUs, the silent-peer check, and the README's
lines for each: debug 598, release 622, asan 597 and tsan 387 tests; tier 3,
103 of 103, every shape's vanished-node tests, the three new, and the
detector's; the release suite at `OSOE_DEADLOCK_SECONDS=1`, 621 of 622, the one
the real deadlock of item 2, ended with the same report; the detector's, the
agents', the scan's, the stopped-node and the pacer's 139 tests ten times over
under eight busy loops, the pacer's at one second, every run passing; and
mutations, each shown in its build: the widening made the default (the `task`
test fails), made a no-op (both `node` tests), io_uring's workers told apart
by name (both helper tests), and the silent-peer check turned off (all nine
vanished-node tests, the plain ones by the detector's probe). One survives, as
expected: the detector not started afresh at init, which the second-session
test cannot see, since the race the reset closes needs a probe to cross a
close. The SOS sweep: 159 programs built and exited 0 on both maps, and the
same four as before did not - two use SOS's own extension, and osoe stops the
other two by name, each on both maps.

At `ff4e96d`, before the last two: the four presets whole, debug 598, release
622, asan 597 and tsan 387, and tier 3, 94 of 100 - the six vanished-node
tests of item 4.

## For the author

1. **The detector is on by default, at 10 s.** You said "build it"; the default
   was my proposal. Off is `OSOE_DEADLOCK_SECONDS=0`, and the undo is the
   default in `deadlock.c`. "Report only", in the proposal, is not built: say if
   you want it - a flag that prints and does not end.
2. **`OSOE_AGENT_CPUS` stays `task` by default**, against my own answer last
   night that I would add the widening: measured, it cost the collectives more
   than it bought beside the slice.
3. **RemoveIPC**: nothing is built; the runbook's advice to administrators
   stands. The cheapest defence left is to detect a name removed at start and
   exit saying why; the `/proc` route would let such a job start normally.
4. **The landing zone**: kept at 256 KiB; refilling it as replies land, rather
   than letting it drain, is the recommendation, not built.
5. **The silent-peer check is a design change** (§3.15): a vanished node with
   data in flight is found within keepalive's budget, where the kernel took
   fifteen minutes. Undo: `silent_check_start` in `progress-host.c` leaves the
   budget 0 - but then the detector must not probe, or its probe hides a
   vanished node again.
6. **The other session's worktree and branch** (`prompts-index-steps-4-to-10`,
   `.claude/worktrees/dreamy-brahmagupta-d9e7dd`): its rows are on main; both
   are yours to remove.

## Next

The review. Its prompt names what it should read first; the release point's
open conditions are the SOS sweep and the Pi runbook end to end on the tree
that is tagged, and a release note of the known limits.
