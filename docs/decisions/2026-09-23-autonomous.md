# Decisions taken without the author, 2026-09-23

The author was away for eight hours from about 09:45 EDT and left the work to run
on its own: merge to main, continue to later slices, review every test with
subagents, keep a record of every decision point passed. This is that record.
Each entry says what was decided, why, and how to undo it. The most
consequential are first within each section; the sections are in the order the
work met them.

There is no git remote, so every merge here is local and nothing was pushed.

## Where things stand, and what needs you

*The author reviewed every entry on their return the same evening; each question
below carries their answer, and "The author's review" at the end records every
entry's.*

On main: step 3 slice 2 (36b05fe), the osoe-stat branch (a0cded6), the test
review (fd7c07e), step 4 slice 1 (92309a7), the fixes its review found
(9d9251e) and those a third review then found (567d02f), with this log and the
second slice's brief among them; and, after the review, the heap-base shift
(d5ca388), the fork.L4 guarantee (8f55f43) and the `conn` rename (9b8c985).
Nothing was pushed; there is no remote.

Decisions to confirm or reverse, most consequential first:

1. The abort crosses nodes as an abortive goodbye rather than along the tree
   (12). This changes §3.15's "flood" design, on the strength of a model.
   **Confirmed.**
2. Keepalive's values, 60/10/6, and no `TCP_USER_TIMEOUT` (17). **Confirmed.**
3. The development watchdog: retire it, or have the ending agent name the
   routine its PE is blocked in (16). **Both: retire it, and the ending agent
   names the routine, in every build.**
4. Pidfds from every agent's loop rather than the lead's ring (16), and
   whether the ring version is worth a new seam function (20). **Confirmed;
   the ring version is not built now.**
5. `OSOE_LIVENESS=0`, a second off-switch beside `OSOE_WATCHDOG`, for the
   tests of the watchdog and the sweep (18). **Confirmed until the watchdog
   goes, then dropped with it.**
6. Parts A, C and D verified together rather than one by one (15).
   **Confirmed.**
7. Slurm: where to capture fixtures, and building the Slurm path they need,
   which does not exist yet (19, 20). **The path in the next slice; fixtures
   captured from Slurm installed in the VM.**
8. Merging the second slice's Part A alone, or waiting for the rest. Its
   branch (`83a3fbd`) is on today's main, after the third review's fixes,
   and verified there. **Merged alone (d5ca388).**
9. What the review left as designed rather than fixed (22): `exit` from the
   agent runs handlers beside a live main thread, and a fatal ends at once
   rather than through the agent. **`exit` on the caller's PE only, `fflush`
   and `_exit` on every other; a fatal as it is until on-demand
   connections.**
10. The third review's open finding (23): a peer's send error on the ending
    node's reset can end it before its engine reads the goodbye, which its
    kernel holds. Closing it changes how the wire treats an error in either
    direction. **A small slice of its own, next, modelled first.**

After the merge, two adversarial reviews found four bugs in step 4 slice 1,
and verifying their fixes found a fifth. All five are fixed, verified and
merged (22). A third review, of those fixes, found that glibc 2.41's `exit`
lock left a process whose main thread was inside `exit` to end at the grace,
unflushed, and four smaller things; those are fixed and merged too
(`567d02f`), and one transport finding is left for you (23).

Found on the way and left as suggested tasks: `progress.c` names every
`struct osoe_connection *` parameter `conn`, which CLAUDE.md's naming rule
forbids (the new code followed the file rather than start a second style);
and `shmem.fork.L4` flaked under load in tsan, since before step 4 (21) - on
main, the review's first fix turned out to have closed it; branch
`fork-l4-flake` makes that order deliberate. Both were done by sessions of
their own and merged after the author's review.

## Phase 1 step 3 slice 2

**1. The brief's stop threshold was hit, and the work went on** (commit
`a9f5bf2`, taken by the first session of the slice and kept by the second).
The brief said to stop if a put and quiet still cost more than about 1.5× the
floor's round trip idle once `poll_lead` was gone. It measured 2.06×. The 1.5×
came from a scratch build that spun (`sched_yield`) instead of sleeping, which
§3.11 forbids. So the threshold measured the forbidden variant, not the
design. A put and quiet now costs one library round trip, the same four wakes
as a get (2.05× since slice 1). The spin was measured as §3.11 asks before
revisiting "no spinning": 1.14× idle, but 4–7 ms per put and quiet and about
10 MiB/s of stream beside 16 busy loops. §3.11 records the measurement.
*To reverse:* a spin needs §3.11 and §3.16 changed first; the measurement says
not to.

**2. "Waiters per wake word" is counted from the blocked records, not kept by
the job** (`8e8e927`). The brief asked for it through §3.14, and the first
session had not done it. A per-connection counter would cost two locked adds
per sleep on a line the agent writes on every acknowledgement. Instead each
internode wait now names the word it sleeps on (four new detail kinds), and
`osoe-stat` counts per connection: the job pays nothing. This costs a layout
bump, 13 → 14. *Knock-on:* the other session's `osoe-stat` fix
(`stat-watch-word-layouts`) names 13 as its newest layout; it was told, and
whichever lands second raises it to 14. *To reverse:* revert `8e8e927`.

**3. The floor pins are not the ones the brief named** (part B). The brief said
to pin the fastest trips against the floor, measured in pairs, as §8.4 found
for `echo-uring`. Measured here:
- the fastest trip cannot see the stand-in this slice removed: `poll_lead`'s
  20 µs sleep is about an idle round trip, so a put and quiet's fastest trip
  with it measured 2.1–2.7× against 1.9–2.2× without;
- a stream at the 1 MiB buffer the first session's draft used cannot see it
  either (0.35–0.64× against 0.52–0.69×).

So three pins, each the median of seven pairs: the fastest trip within 3× the
floor's (kept, loose, as the brief asked); a put and quiet and a fetch-add
within 1.5× a get, which have the same wakes and so catch a polling wait at
any load; and the stream at the *default* buffer at least 0.1× the floor's,
where every other put waits for a turnover. Also: the four round trips are
interleaved within each round. Timed one kind after another, the fastest-trip
ratio moved 1.7–4.1×, where interleaving held it at 1.4–2.2×. *To reverse:*
the constants are three `#define`s in `shmem/tests/test-floor.c`.

**4. The floor is measured inside the test, not by running `echo-uring`.**
§1's floor is "a trivial io_uring TCP echo on the same link". The test runs one
between its own two processes, dialled at node 1's host-list address, so it
crosses exactly the link the library does: loopback at tier 2, the shaped link
at tier 3. Running the `echo-uring` binary instead would need its own launch
per pair and could not interleave with the library's trips.

**5. Tier 3 is its own preset, run through `sudo -n`** (part C). The brief left
the registration open. The `tier3` preset (`OSOE_TIER3=ON`) registers the
tests; the four standard presets never touch the network state. The
launcher enters each namespace as root and then drops to the invoking user
with that user's groups, so the job never runs as root. *To reverse:* delete
the preset and the `if(OSOE_TIER3)` block.

**6. Tier 3's subset is larger than the brief's list.** The brief named the
internode maps, the tree and the differential's multi-node maps. Added: the
node-pair wire and the progress engine (the lowest-level TCP framing, which
is what tier 3 is for), the AM barrier, reply starvation (a guard on full
buffers, the waits this slice rewrote), the rendezvous test, the floor, and
the uneven internode map `0,0,0,1,2,2`, the one where three PEs of a node
contend for two reply credits and so reach the credit queue. The
differential runs all four poles, including the one-node map, which is its
reference. 13 tests × 3 shapes, about 3 minutes without hangs.

**7. The one thing tier 3 found was fixed, not stopped on.** The brief said to
stop if tier 3 found "a defect that loopback hid and that is a design
question". It found a test-only mesh path that dialled 127.0.0.1 for every
peer. That is a bug below the boundary, not a design question, so it was
fixed: the path now dials each peer's host-list address, and the resolver
moved from `rendezvous.c` to `listener.c`.

**8. The other session's `osoe-stat` work was coordinated, not merged
blind.** That session (branch `stat-watch-word-layouts`) had uncommitted work
in progress. It was sent a message saying layout 14 exists and asking it to
commit on its branch rather than merge into main. Its work is merged only
once committed, with its newest layout raised to 14.

**8a. The `osoe-stat` branch is merged although its own brief said to ask
first.** The other session committed `b229f8e` on `stat-watch-word-layouts` and
passed on that its brief (from the author, earlier) said "do not merge into main;
ask before merging". The author's later instruction to this session was to keep
track of that work "and incorporate its work product as appropriate", with
merges to main authorized. Read together, the later instruction governs, so it
is merged, as a merge commit of its own after slice 2 so that one
`git revert -m 1` removes it. The follow-up it needs, raising its newest
decodable layout to 14, is a separate commit. *To reverse:* revert both.

## The test review

All 229 registered tests were reviewed by four subagents in parallel (tools
and models, tier-1 units, multi-node, single-node), each classifying every
test as deprecated, core or active, and judging scale, runtime and
parameters. **No test was found deprecated**: every source a test compiles is
still in the library, and nothing was written against the step-2
shared-memory wire. What changed is listed in the test-review commit; the
decisions behind it:

**9. Every model runs in one preset, release** (it already held for the four
long ones). A model is Python and gives the same verdict whichever preset
built the library, so running it four times bought nothing. This takes
about 150 s of CPU out of each of debug, asan and tsan.

**10. The three models of `SHMEM_THREAD_MULTIPLE` are opt-in**
(`OSOE_MULTIPLE_MODELS=ON`). `get_completion`'s design cases, `dirty_set` and
`watch_list` check §4.11, which is designed and not scheduled for phase 1;
together they cost about 18 minutes of every release run (get_completion
alone 14). `get_completion`'s one case of what phase 1 ships (§4.6's
equality wait) still runs. Turn the option on when §4.11 is scheduled, or
when `engine.py` changes (`model.selftest` covers the engine's own
reductions, not these state spaces).

**10a. `echo-uring`'s pin was flaky, and the fix is interleaving, not a
looser pin.** The other session reported the fastest-trip pin failing at
3–4.7× about one run in ten idle. Loosening it to 5× would have kept it
green and made it nearly blind (a 50 µs pause measured 13×). The same cause
as the floor test's noise, a CPU asleep for one block of trips and not the
other, has the same fix: the two kinds alternate within each trial. The
first form of the fix forked a server process per connection so both could
be open at once, and the load loop failed it once in 30 (2.3x): under 16
busy loops one of a pair's two servers ran badly placed for whole trials.
One server thread now serves both connections in the client's order, so the
two kinds share every scheduling decision on both ends; 30 of 30 under load
after that.

**10b. `shmem.hang-walk` walks until it lands, not after a 3 s sleep.** The
first attempt waited for every other node's `own_arrived`, which failed: a
node's own-arrived flag resets once its arrival has gone up the tree. Polling
the walk until it ends at the culprit, bounded inside the launcher's 12 s,
keeps both assertions (it finds the culprit, in at most four visits) without
the timing bet.

**10c. The capacity script skips rather than capping the node count.** The
reviewer proposed capping the node count at 16 for large machines. With the
buffer at its 1 GiB maximum, that makes the test unable to exceed a large
`/dev/shm`, so it would fail for the wrong reason. Past 33 nodes the 64 GiB
budget refuses first, and the test now says so and skips.

## Step 4, first slice

**11. Step 4's first slice was briefed without the author**
(`docs/prompts/done/phase1-step4-slice1-prompt.md`): the abort crossing nodes,
every PE ending when its node aborts, liveness without the watchdog, and
keepalive. The Slurm fixtures and the heap-base shift are left for the next
slice. The brief follows §3.15 and §7.1 as written.

**12. The abort crosses nodes as an abortive goodbye, not along the tree.**
§3.15 said the calling PE's lead "floods an abort AM to every node". The brief
first said to send it along the world team's tree, which is logarithmic and
fits the scaling goal. The model (`tools/modelcheck/abort_model.py`) says
that loses the status. The eager mesh already carries a death in one hop, as
an end of stream without a goodbye, which a peer takes for a crash. A peer
that reads a dying node's end of stream before the tree's message reaches it
exits with a failure where `shmem_global_exit`'s caller gave a status, which
§9.1.6 forbids. An abortive goodbye, the last segment on every open
connection, is read before the end of stream it precedes. It costs
O(open connections) a node: N on the eager mesh, which is what a normal
goodbye already costs, and it will shrink with on-demand connections. Also
found by the model: an abort during the close needs no abortive goodbye,
since after the final barrier only a fatal can start one and a fatal's
status is a failure anyway.

**13. Part B's choices.** The agent ends its PE's process, and any other
thread that finds the abort waits for it, because a lead's main thread
exiting first would take the only thread that can tell the network. The
abort word becomes one 64-bit word - how the job ends, the exit status's low
eight bits (all `exit` passes on) and the rank - installed by one CAS from
zero (layout 15), so the first declarer wins and a fatal cannot overwrite a
global exit's status. A first draft claimed the word, wrote the rank and the
status beside it, then published; a declarer killed between claim and
publish would have left every reader waiting forever, so it was replaced
before it was committed. `exit` from the agent gets a 10 s grace before
`_exit`, which is teardown, not a wait that gives up. `shmem_finalize` and
the library's atexit handlers stand aside during an abort's exit, since
`exit` runs them and one of them is a barrier.

**14. Two paths that turned a global exit into a failure, found reading
Part B before committing it and pinned by a model.** The development
watchdog, sweeping, could read the word before a global exit was declared
and then find a sibling that exit had already ended: it called that a death,
and the PE left with a failure - on a lead, before its agent could tell the
other nodes. It now reads the word again before calling any death a fatal.
And a fatal whose own declaration loses to a global exit now ends as that
exit does, since what went wrong may be the exit's own doing. The brief
asked for the exit ordering to be modelled; `abort_exit_model.py` does, and
shows either defence alone is enough for the watchdog's case, and neither is
not. It also showed a wake a declarer can miss: its own agent can end its
process before it has woken the rest. The lead's agent, woken first, now
wakes every local agent again when it starts the abort (O(L) once per
node).

**15. Parts A, C and D were verified together, at the head of the series.**
Each commit is built warning-free on its own, and each part's mutation fails
its tests. But the four presets, the load loop and tier 3 ran once, on the
tree with all three. The VM is the bottleneck, and one run each would have
cost about three more hours of the eight. The run's only failures were tests
whose premise Part C had changed (entry 18), so nothing needed bisecting. To
undo: re-run `presets-tree.sh` at each commit.

**16. Part C watches from every agent's loop, not the lead's ring.** The
brief said the lead's pidfds go on its ring and non-leads check the lead with
`kill(pid, 0)` at a library wait's sweep. A single-node job has no ring, and a
computing PE never reaches a library wait's sweep. The agent's own loop,
whose every wait ends by a sweep, serves both on every job. It uses pidfds
for the lead too, since a pidfd cannot be fooled by a recycled pid. The cost
is up to one sweep interval (100 ms) of latency where a ring completion would
have none; the multi-node lead can take that refinement later. The model
also showed the check makes an abort prompt, not only a crash visible (entry
14). The development watchdog stays in the debug presets. Its one remaining
value is its message, which names the routine the PE was waiting in, and the
agents now usually find a death first and end their PEs before any sweep
speaks, so that message appears only when the watchdog wins the race. Two
ways on, for the author: retire the watchdog, or have the ending agent name
the routine its PE is blocked in (its blocked record already says).

**17. Keepalive is 60 s idle, 10 s between probes, 6 probes, and
`TCP_USER_TIMEOUT` is not set.** A vanished node is found one to two minutes
after its connections go quiet. A stopped node is never found dead, because
its kernel answers the probes. `TCP_USER_TIMEOUT` would bound unacknowledged
data, and Linux applies it to zero-window probing too, so it would declare a
stopped peer with a full receive buffer dead: a fatal timeout. With data in
flight, the kernel's `tcp_retries2` (about 15 minutes) is the bound. The values
are a judgement for the author to confirm: shorter finds a vanished node
sooner and turns a longer network partition into a failed job.
`OSOE_KEEPALIVE` overrides them.

**18. `OSOE_LIVENESS=0` exists for the tests of what else finds a death.**
Part C's pidfds find a local death in every build, so they race the
development watchdog to it. The agent then ends its PE itself, before a
waiting PE's sweep can say which routine it was in. Six tests failed in the
first combined run: the watchdog's four (whose message they check),
`shmem.watchdog.off` (a job that was meant to hang), and `shmem.unswept-wait`,
which exists to show that an internode quiet sweeps. Loosening their
patterns to accept the pidfds' message would have stopped them testing the
watchdog and the sweep at all. Instead they set `OSOE_LIVENESS=0` and test
exactly what they tested. `shmem.watchdog.liveness` is the same job with the
watchdog off and the pidfds on, and `shmem.dead-pe` covers the pidfds
themselves. The switch is a second off-switch beside `OSOE_WATCHDOG`; drop
both if the watchdog is retired.

**19. Step 4's second slice is briefed, and its Slurm part waits for the
author** (`docs/prompts/done/phase1-step4-slice2-prompt.md`). It covers the
heap-base shift, the Slurm fixtures, and the multi-node lead's pidfds on its
ring. The fixtures must be *captured* from `srun`, and the VM has no Slurm.
Debian's `slurm-wlm` is available, but installing and running Slurm daemons
in the author's VM is their call, as is a real cluster, or fixtures written
from the documentation and marked as such. The brief says to stop there and
ask.

**20. Two premises of the second slice's brief were false, and both parts
wait for the author.** The brief is left as it was written. Part B assumed a
Slurm path to feed fixtures into. There is none: `osoe_bootstrap_read`
refuses a Slurm environment ("launch under Slurm is not yet supported"), and
reads only the `OSOE_` names the launcher sets. So Part B is first the Slurm
path, §6.1's table read with the prefix replaced, and only then the fixtures
that test it, which still need a Slurm to capture from. Part C assumed the
lead's pidfds could simply go on its ring. The ring is the AM backend's,
below §4.10, and `liveness.c` is above it, so arming a pidfd there needs a
new seam function, and an RDMA backend would need its own. That is a seam
decision, and what it buys is the ≤ 100 ms a sweep-interval poll can take.
Part A, the heap-base shift, needed neither and is done on branch
`phase1-step4-slice2` (`83a3fbd`, on main after the third review's fixes
and verified there), not merged. It found two one-base
assumptions, a test comparing addresses and `shmem_align` deciding from the
PE's own base; the whole of tier 2 passes shifted.

**21. `shmem.fork.L4` flakes under load in tsan, and did before step 4.**
Found running tier 2 shifted: the test checks that the control segment's
name is gone when `shmem_init` returns, and under load a non-lead can return
before its lead has removed it. It fails the same way without the shift and
on the tree before step 4, so it was left as a suggested task rather than
fixed inside the heap shift.

*Resolved since, on branch `fork-l4-flake`, merged after the author's review
(8f55f43).* The review's first fix
(22) had already closed it on main: init's wait for the lead's agent holds every
non-lead until a store the lead makes after removing the names. Beside 16 busy
loops in tsan, 30 runs each: 8 failures on the tree before that fix, 0 on main,
and 6 on main with only that wait removed. The order was an accident of where
the wait went, so the branch says it at both sites and in §6.2 and §3.16, and
the test checks the heaps' names too.

**22. The merged slice was reviewed adversarially, and four of the findings
were bugs, fixed and merged (`9d9251e`).** Two reviewers read
it, since the same author wrote the brief, the code and both models. Each
finding was checked against the code before it was acted on.

- A non-lead could return from `shmem_init` before its lead's agent ran, so
  a global exit straight after init told no node.
- A goodbye counted once the kernel took it, and a reset at the process's end
  could drop it.
- `shmem_finalize` stood aside by returning on any thread, into a second
  `exit`.
- The pidfds started too late to see a death during init.

Verifying the fixes found a fifth. The atexit handler and `shmem_global_exit`
read the lifecycle mutex's count without holding it, and an exit straight
after init raced them under tsan.

A test pins each fix, except the first. An exit straight after init passed
with the init wait removed, even across eight nodes at tier 3, because a
lead's mesh setup takes about a millisecond. The exit model pins it instead,
with a case in which the lead's agent starts late.

The exit model could not see the third: it made the agent's end one step,
the claim together with exit itself, so no other thread ever saw the gap
between them. It now has the gap and fails the variant.

Kept rather than fixed:

- `exit` from the agent runs the process's handlers and destructors while
  its main thread still runs. The alternative, `fflush` then `_exit`, skips
  the handlers §9.1.6's "normal termination" implies. The author may prefer
  it.
- A fatal still ends at once rather than through the agent. That is harmless
  on the full mesh and must change with on-demand connections.

The notes had overclaimed in three places: the one-thread rule, the pidfd's
part in a prompt abort, and a paused VM. All three now say what is built.

**23. A third reviewer read the fixes (`11faa12`), and found one more bug
that matters, fixed and merged here, and one it takes a transport change to
close, left for the author.** Each finding was checked against the code, and
the first against the VM's glibc, before anything was changed.

- *`exit` holds a lock for good, since glibc 2.41* (BZ 31997; Debian 13's
  glibc is 2.41). A second thread's `exit` then waits forever: shown in the VM
  with a two-thread program, which also showed that a handler calling `exit`
  again on its own thread ends the process with the second status, handlers
  run and stdio flushed. A PE whose main thread had returned from `main` —
  in start_pes's finalize, or in the handler for a program that left without
  finalizing — when a global exit came left its agent's `exit(status)`
  waiting on it until the grace, 10 s, and the grace's `_exit` lost its
  buffered output. **Fixed:** the library's atexit handlers mark their thread
  inside `exit`; that thread waits for the agent's claim, which follows the
  goodbyes, then calls `exit(status)` again itself, and the agent, seeing the
  mark, leaves `exit` to it. The exit model gains the case (`exiting`) with
  exit's lock in it, and fails both ways of getting it wrong: the thread
  waiting like any other (STUCK) and not waiting for the claim (STATUS: the
  lead's process ends before its goodbye). Tests: `shmem.global-exit.start-pes`
  and `.returned`, which end in about 0 and 1.3 s, and at 10 s with the fix
  undone. *Beside it:* the grace thread now flushes stdout before its `_exit`
  when no thread holds it, so that a program's own handler in a library wait
  — which nothing marks, and which still ends at the grace — keeps its
  output. *To reverse:* revert the commit; the alternative is `fflush` then
  `_exit` on the marked thread, which skips the handlers still to run.
- *The goodbye reaches the peer's kernel, not its engine.* Once the ending
  node resets the connection, a peer still streaming to it gets a send error,
  stops re-arming its receive and ends on "the connection failed", with the
  goodbye unread in its kernel whenever its ring had no buffer free for it.
  **Not fixed** — it wants the wire to keep receiving after a send error until
  the receive side ends, which changes how the ring and the engine treat an
  error on either direction, and it has not been seen in any run (the
  reviewer's reproduction is two wire buffers or a starved CPU). Scoped: the
  wire has one `error` for both directions (`wire.h`), set by a failed receive
  and a failed send alike (`ring.c`), and it stops the receive's re-arming,
  ends the node in `receive()`, and ends it again in `osoe_wire_send`, which
  every flush calls. So the change is a send error that is a reset
  (`EPIPE`, `ECONNRESET`) stopping the sends without ending the node, the
  receive re-armed regardless, and the node judged when the receive side ends
  — by the goodbye, read or not, and with the send's error in the message if
  not; any other send error ending the node as now. That is a protocol
  change of the engine's, which wants `abort_model.py` to carry a send error
  first. The design notes say so where they describe the wait. *Question for
  the author* (10 above).
- *The connection area's first wait never swept*, so a lead dying before
  sizing the area left its siblings there in a release build, while the
  notes said init's waits all swept. **Fixed**, and the notes now say which
  waits check: those after the node's rendezvous. A lead dying inside that
  rendezvous is still the launcher's to find.
- *A reset leaves `SIOCOUTQ` above 0 for good*, so a peer that reset the
  connection held the goodbye's wait for its whole grace. **Fixed:** a closed
  connection counts as told. The notes now give the bound as it is, twice the
  grace: the goodbyes', then exit's.
- *`shmem.global-exit.fatal.nonlead` could not fail on a lost goodbye*, since
  an end of stream without one also ends a node with the failure. **Fixed:**
  where the caller's lead speaks for it, every end must name the caller and no
  node may report a lost goodbye; with no goodbye sent at all it now fails.
  An immediate exit now requires the caller's own line, which its agent's
  `exit` flushes.
- *`shmem.dead-pe.lead-in-init` assumed a lead through its node's rendezvous
  within one second.* The alarm is now at 3 s, node 1 starts 8 s late, and the
  launcher's limit is 12 s.
- *Kept:* neither of the second review's first two fixes is pinned by a test
  in the four presets — the exit model pins the first, tier 3 the second —
  and the grace's `alarm` fallback, for a process that cannot start a thread,
  bounds nothing if the program handles or blocks `SIGALRM`, which the
  comment now says.

**What step 4 builds on** (surveyed 2026-09-23). The node's abort word
(`node.h`: `aborting`, `abort_pe`, `abort_status`) is set by
`osoe_node_declare_abort` from every fatal, from `init.c`'s atexit handler
and from the watchdog. It is read at every sweep of every library wait, in
every build (`watchdog.c`), and a PE that finds it set leaves through
`_exit(EXIT_FAILURE)`. The agent's own wait (`osoe_progress_host_wait`) ends
every sweep interval but never reads it. `shmem_global_exit` is not declared
anywhere. The AM kinds are `PUT`, `GET_REQUEST`, `GET_REPLY`, `RMW_REQUEST`,
`RMW_REPLY`, `BARRIER_ARRIVE` and `BARRIER_RELEASE` (`am.h`), dispatched in
`progress.c`'s `complete()`; the goodbye is a segment flag, not a kind. Only
`TCP_NODELAY` is set on the node-pair sockets (`wire.c`). Each PE publishes
its pid in its slot before the release store of its phase; `pidfd_open` is
not used anywhere yet, though the second review's portability evidence
proves `pidfd_open` polled on a ring beside a futex wait.

## The author's review, the same evening

The author went through every entry on their return. Each is confirmed as built
unless this section says otherwise.

- **Confirmed as built:** 1–15, 16's pidfds from every agent's loop, 17, and
  23's smaller fixes and what it kept. Entry 12 (question 1), the abortive
  goodbye, among them.
- **16's watchdog, with 18 (questions 3 and 5):** retire the development
  watchdog, and have the ending agent name the routine its PE was blocked in
  — its blocked record (§3.14) says — in every build. `OSOE_LIVENESS=0` stays
  until then and goes with `OSOE_WATCHDOG`, and the tests that exist to tell
  the two apart are reworked. A later slice.
- **20's Part C (question 4):** the lead's pidfds stay polled at its agent's
  sweep. The ring version needs a seam function and buys at most a sweep
  interval, so it is not built now; only if sub-100 ms death detection ever
  matters.
- **19 and 20's Part B (question 7):** the Slurm path is the next slice's,
  and its fixtures are captured from Slurm installed in the VM — `slurm-wlm`
  and munge, one `slurmd` for single-node captures, a container cluster for
  multi-node ones. The second slice's brief now says so.
- **20's Part A (question 8):** merged ahead of the rest of its slice
  (d5ca388).
- **21, and the rename:** both sessions' branches merged (8f55f43, 9b8c985).
  The three merges, each verified alone, were verified together on main.
- **22 (question 9):** a global exit ends the caller's PE with
  `exit(status)` and every other PE with `fflush(NULL)` then `_exit(status)`,
  so that no handler or destructor runs beside a main thread that is still
  computing; a follow-up. With it, 23's hand-off to a thread inside `exit` is
  to be removed if it has become redundant. A fatal keeps ending at once until
  on-demand connections, which must send it through the agent's goodbye.
- **23's open finding (question 10):** a small slice of its own, next:
  `abort_model.py` carries a send error first; then the wire's errors are
  split by direction, a reset's send error stops the sends without ending the
  node, and the node is judged when its receive side ends.

The work this leaves, in the author's order: the question-10 slice; the
exit follow-up (22); the rest of step 4's second slice, the Slurm path and
its captures; the watchdog's retirement; and, with on-demand connections,
fatals through the goodbye.

## How this record is kept

Entries are appended as decisions are taken, and committed with the work they
explain.
