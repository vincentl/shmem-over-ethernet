# Phase 1, step 2, slice 1 — diagnosability and deferral safety

Question 6 of the second adversarial review. Reviewed tree: `main` at `7ff9343`
(the slice is merge `d542c63`). Everything marked *measured* was run in the Lima
VM `osoe` (4 cores, shared with other review agents — all timings **measured
under contention**) in a private copy at `build/scratch/review-diagnosability/`,
`debug` preset. Programs, scripts and logs are under
`docs/reviews/evidence/phase1-step2-slice1/diagnosability/`
(`programs/hang.c`, `programs/run-experiment.sh`, `programs/run-sigkill-all.sh`,
`programs/lost-wake.patch`, `programs/fuzz-stat.py`, `logs/*.log`).

This review was cut short by a budget limit. Part A (experiments) is complete
except for the `osoe-stat` fuzz, which ran 57 of 300 rounds. Part B (deferrals)
rests on targeted reading and greps, not on a full read of `wire.c`, `team.c`
or §4.11; each row says what it rests on. See "Not investigated".

## Verdict

**The claim "a hung job can be diagnosed from outside without cooperation from
the job" is true for the intranode half of the library and false for the half
this slice added.**

Where a PE blocks in a *SHMEM-layer* wait — a barrier, `wait_until` — `osoe-stat`
is genuinely good: it names the routine, the epoch or the watched offset, how
long, shows a sweep count that proves the sleeper is alive, and the node dump
(`team.world.arrived`, `arrival_posted` versus `released`) identifies *the node
that has not arrived* and then *the PE that has not called*. I found the culprit
in experiments 1 and 2 with `osoe-stat` alone, and the tool never needed the job.
The §3.14 mechanism — frozen directory, table by name, no shared headers — is
real and works.

Where a PE blocks *below the §4.10 boundary* — an internode `quiet`, get, AMO,
a full buffer, a missing credit — the PE publishes **no "blocked in" record, no
sweep count, and the AM layer has not one metric in the table**. In experiments
3–5 every victim displayed `blocked in: -` with `sweeps 0` for the whole 12–20 s
it was stuck; an operator sees PEs that are "not blocked" and must infer a hang
from counters that stop moving between two reads. Nothing on a victim's node
names the node it is waiting for, until the PE dies of `OSOE_BOOTSTRAP_TIMEOUT`
and says so on stderr. The culprit node can be found only by visiting every
node and reading `agent.loops` twice.

And one experiment produced the most useful result of the six: **removing the
wake from the internode barrier release made barriers 200× slower, and nothing
noticed** — all 22 internode/multinode/barrier/differential tests pass, every
`invariant.*` counter stays zero, and `osoe-stat` flags nothing (finding D-1).
The invariant-counter oracle covers the §4.9 watch path only, although §3.14
says "or any other wait".

Deferrals: nothing I examined is *structurally foreclosed*. Four are *costlier
than the design admits*: data-carrying collectives (the internode phase is a
barrier-shaped star rooted at node 0, not the §5.2 tree), nonblocking gets,
teams beyond two (per-team state is named fields, not arrays), and step 3's
idle detection (the "two equal reads mean stalled" heartbeat only works while
the lead polls).

## A. Hang-experiment log

Every job: `osoe-launch -n 6 -m <map> -- hang <mode> <culprit>`; the operator
commands were `osoe-stat --list`, `--counters --nonzero <segment>` twice 3 s
apart, `--threads`, `--dump`. "Culprit?" is whether I could name the culprit PE
or node **from `osoe-stat` output alone**, before any abort message.

| # | Scenario (map) | What `osoe-stat` showed | Culprit? | Ended by itself? | What was missing |
|---|---|---|---|---|---|
| 1 | PE 3 never reaches `shmem_barrier_all` (`0,0,1,1,2,2`) | Five PEs `shmem_barrier_all(+0x3) for N s`, sweeps rising 59→89; PE 3 `-`, sweeps 0, `shmem.barrier.calls` 2 against 3. Node dump: node 1 `team.world.arrived 5`, `arrival_posted 2 == released 2`; nodes 0 and 2 `arrived 6`, `arrival_posted 3`. | **Yes**, PE and node — but only by reading all three nodes' segments and knowing that `arrival_posted == released` means "this node". | No. Launcher timeout. (By design: nobody is dead.) | The root (node 0) knows how many nodes have arrived (`progress.h:106-109`) and it is private to the progress thread. One node-scope metric there — arrivals counted / participants, ideally the lowest node not yet arrived — turns a 500-node search into two hops. The `epoch` column shows 2 for victims and culprit alike, so it does not discriminate; the detail does, but is printed as `(+0x3)`, an offset's format for an epoch (D-7). |
| 2 | PE 3 in `shmem_long_wait_until` on a cell nobody writes; others at a barrier | PE 3 `shmem_long_wait_until(+0) for 8.0 s`, `pe.watch (decoded) heap +0..+0x8`, sweeps rising; others in `shmem_barrier_all(+0x3)`. | **Yes.** The odd PE out is visible on its node. | No. | Who was *supposed* to write is unknowable (an API property, see Lessons). The comparison and value waited for are not recorded; cheap to add to `detail`. |
| 3 | SIGSTOP non-lead PE 3 while PEs on nodes 0 and 2 `putmem`+`quiet`+`getmem` to it | **Nothing hung.** Counters kept rising on all writers (put.calls 24997→32418 in 3 s). The lead of node 1 applies puts to, and serves gets from, the stopped PE's heap. | n/a — no victims. `osoe-stat --threads` does not show process state, so a stopped PE looks like an idle one. | Launcher timeout. | `/proc/<pid>/status` `State:` beside the CPU time `--threads` already reads: one field, and "T (stopped)" is the whole diagnosis in #3 and #4. |
| 4 | SIGSTOP **lead** PE 2 of node 1; same traffic to PE 3 | Victims (PEs 0,1,4,5): `blocked in: -`, **sweeps 0**, counters frozen between reads, `transport.wait.ns` not updated (it is added on exit). Node 1: lead's `agent.loops` 50206 → 50206 while the non-lead agent went 100 → 131. | **Victims' nodes: no.** Nothing there names node 1. **Node 1: yes**, if you read twice and know that PE 0's agent block is the progress engine. | **Yes, after `OSOE_BOOTSTRAP_TIMEOUT`**: `osoe: PE 5: node 1 has not acknowledged this PE's writes after 20 s` + control segment path. Names the culprit *node*; good message. | A blocked record from the four AM loops (D-2), and per-connection state in the table (D-3): oldest unacknowledged sequence and its age would name node 1 from any victim. |
| 4b | SIGSTOP lead PE 3 of node 1 (`0,0,0,1,1,1`); non-leads 4,5 and node 0 exchange put/quiet/get | As #4. Non-lead PEs 4 and 5 on the stalled node are victims too and look identical to remote victims. | As #4. | Yes: PEs 0,1,5 `node N has not acknowledged…`; PEs 2,4 **`no get reply after 20 s`** — names neither the target PE nor the node (D-6). All non-lead PEs' messages reached stderr. | As #4. |
| 5a | SIGKILL non-lead PE 3 under the #3 traffic (`-k`) | PE 3 `[pid is gone]`; everyone else running normally. | **Yes** (`[pid is gone]`). | **No — the job kept writing into the dead PE's heap** through node 1's lead until the launcher timeout. The watchdog never ran: no PE was in a sweeping wait. | Expected before step 4, but worth stating: RMA traffic alone never notices a dead non-lead target. |
| 5b | SIGKILL PE 3 while others sit in a barrier (`-k`) | Node 1: `ABORTING on behalf of PE 3`, both PEs `[pid is gone]`, `node.aborting 1 / abort_pe 3`. Nodes 0, 2: five PEs in `shmem_barrier_all(+0x3)`, nothing else. | **Yes on node 1; no on nodes 0 and 2.** | Node 1 only: `osoe: PE 2: shmem_barrier_all: local PE 1 (rank 3, pid 643085) has died; …development watchdog…` within one sweep — an excellent message. Nodes 0 and 2 hung to the launcher timeout (documented: no cross-node detection). | — |
| 5c | SIGKILL launcher and all PEs; then `osoe-stat --gc`; then a job with the same job id | Left: 2 control segments, 2 `conn` areas, `job`, `wire` (heaps already unlinked). `--list` reports `2 PEs, 0 live`. `--gc` removes the two control segments and **prints an error for, and leaves, the other four objects.** | — | — | **The next job with that id fails**: `cannot create connection area /dev/shm/osoe.77700123.0.1.conn.0: File exists; probably left by a crashed job…` (a good message) — and that failed job then leaks its own control segments *and a 256 MB heap* (D-4). |
| 5d | SIGKILL lead PE 3 (`0,0,0,1,1,1`, `-k`) under the #4b traffic | PE 3 `[pid is gone]`; all five others `-`, sweeps 0. | **Yes** (`[pid is gone]`) on node 1. | Yes, after the timeout (12 s here, **60 s by default**), with the #4b messages. The watchdog did not fire: none of the victims' loops sweep. | As #4. |
| 6 | **Lost wake**: `osoe_epoch_wake_all` removed from the internode release (`progress-host.c:115`, `lost-wake.patch`); 30 barriers | No hang — the sweep rescues every barrier. 30 barriers: **3.00 s against 0.015 s** healthy. `osoe-stat`: every `invariant.*` zero; `pe.sweeps 32` beside `epoch 32`; `shmem.barrier.ns / calls` ≈ 100 ms = `OSOE_SWEEP_NS`. | **No.** Nothing is flagged; the two tell-tales need the operator to already suspect a lost wake. | n/a | `invariant.missed_wake` at every sweeping wait, not only the watch (D-1). |

Confirmations of sibling findings, from these runs: **(a)** confirmed *measured*
(#3–#5d: `-`, sweeps 0, no abort check — in #5d PEs sat 12 s beside a corpse the
watchdog would have named); **(b)** confirmed (*observed in source*: no reader
of `progress.h:109-114` outside `progress.c`; *measured*: 89 metrics, none
`wire.`, `connection.`, `reply.` or `credit.`); **(c)** neither confirmed nor
contradicted — I did see the idle lead spin at ≈3,000 `agent.loops`/s with
`agent.wakeups` constant (#1), but did not isolate ack echo; **(d)** not
exercised — `hang.c` uses `shmem_init`, and every `osoe_fatal` in my runs exited
promptly with status 1; **(e)** confirmed *observed in source* (`team.c:171,
190, 227, 237`).

## Findings

### D-1. A removed wake in the internode barrier release is invisible to the tests, the invariant counters and `osoe-stat`
- **Severity:** design change needed. **Confidence:** high.
- **Location:** `base/epoch.c:25-37` (`osoe_epoch_wait_eq`); `shmem/src/progress-host.c:115`; `shmem/src/wait.c:107` (the only `OSOE_C_INVARIANT_MISSED_WAKE` site); `shmem/src/transport-am.c:335-351` (`await_reply`, same shape); design §3.14 catalogue 6.
- **Failure scenario:** delete `osoe_epoch_wake_all(released)` in `release()`. Every multi-node barrier now completes only when its waiters' 100 ms sweep expires.
- **Evidence:** *measured* — `logs/6-lost-wake.job.log`: 30 barriers 3.002 s against 0.015 s control. *measured* — `logs/6-lost-wake-ctest.log`: `ctest -R "internode|multinode|barrier|differential"` on the mutated tree, **22/22 passed**. *measured* — `logs/6-lost-wake.log`: no `invariant` line in `--counters --nonzero`. *documented* — §3.14: "Deadline expiries after which the awaited condition was already true: a missed wake, in the watch slots (§4.9) **or any other wait**". *observed in source* — on `ETIMEDOUT` `osoe_epoch_wait_eq` calls `osoe_sweep_expired` and loops; it never asks whether the value already equals the target. The project's own standard (review prompt, ground rules) is that a test asserting a wake must fail when the wake is broken; here no test and no oracle does. Run once per configuration, not three times: the result is a deterministic 100 ms × 30, not a flake.
- **Fix direction:** in every sweeping wait, when the futex wait times out *and* the reloaded predicate is already true, count `invariant.missed_wake`. `invariants.h` then catches this mutation for free. `await_reply` and the agent's idle wait want the same.

### D-2. PEs blocked in the AM backend look "not blocked" and their liveness counter never moves
- **Severity:** design change needed. **Confidence:** high. (Sibling finding (a); this adds the operator's view.)
- **Location:** `shmem/src/transport-am.c:175-193, 305-321, 335-351, 601-610`; callers of `osoe_blocked_enter` are only `barrier.c:82,96` and `wait.c:141,343`.
- **Failure scenario:** experiment 4. For 20 s four PEs on two nodes are stuck in `shmem_quiet`; `osoe-stat` prints `blocked in: -`, `sweeps 0`. §3.14 says a PE with no record "is also not blocked". An operator triaging 500 nodes by "who is blocked, in what" skips exactly the victims that would lead to the culprit.
- **Evidence:** *measured* — `logs/4-sigstop-lead.log`, `4b-…`, `5d-…`. *observed in source* — the four loops call neither `osoe_blocked_enter` nor `osoe_sweep_expired`.
- **Fix direction:** publish `routine` + **target node** in `detail` before the first sleep of each loop (the §3.14 cost rule is respected: the path is already sleeping), and call `osoe_sweep_expired` each 100 ms so `pe.sweeps` rises and the abort word is honoured.

### D-3. The AM layer has no presence in the metric table, so the culprit node cannot be named from a victim's node
- **Severity:** design change needed. **Confidence:** high. (Sibling (b).)
- **Location:** `shmem/src/progress.h:74-87, 106-114`; `shmem/src/metrics.c:33-186`; §3.14 catalogue 1 "Per connection".
- **Failure scenario:** experiments 4/4b/5d before the timeout: nothing on nodes 0 or 2 says "node 1". §3.14 promises per-connection sequence numbers sent/acked/applied, in-flight age, credits, queue depths; none exists, and §3.14's "Per-peer detail only in progress-thread blocks" has no write site.
- **Evidence:** *observed in source*, *measured* (89 metrics listed in any `*.dump.log`).
- **One record that would have answered every internode experiment:** per connection, `{peer node, next_seq, acked, applied, age of oldest unacknowledged segment}`. "Oldest unacked 19 s, peer node 1" on any victim node names the culprit in one read.
- **Note for step 3:** the metric scopes are node / PE / thread (`segment.c:169-192`). A per-connection record needs a fourth scope, which an *old* `osoe-stat` will refuse by `default: return NULL` — graceful, but it means the scope enum should be extended before tools are in the field.

### D-4. `osoe-stat --gc` does not know about three of the four kinds of object a job leaves, and a same-id job then fails and leaks more
- **Severity:** design change needed (small). **Confidence:** high.
- **Location:** `tools/stat/stat.c:623-668` (`gc_one`), `tools/stat/segment.c:307-330` (`osoe_segment_list` filters only `.heap.`); design §3.14 "`--gc`".
- **Failure scenario / evidence:** *measured*, `logs/5c-sigkill-all.log`. After SIGKILL: `--gc` prints four "is not an osoe control segment" errors and leaves `conn`, `job`, `wire`. A second job with the same job and step id dies at init on `File exists`; its survivors are SIGTERMed and leave two control segments and `osoe.77700123.0.0.0.heap.0` (256 MB).
- **Why it matters:** under Slurm job ids do not repeat, so the refusal is rare; the *leak* is not — and the connection area is the large object at scale. §3.14's fallback (`rm /dev/shm/osoe.<jobid>.*` in an epilog) works, but `--gc` claims the job.
- **Fix direction:** record the names (or a name prefix) of every object the node owns in the control segment, as `pe.heap_name` already does, and let `--gc` remove by recorded name; make `--list` skip, not complain about, non-control objects. Also *observed in source*: `list_segments` holds 64 names (`stat.c:675-678`); with one `wire` object per peer today, a busy node's later control segments fall off the list.

### D-5. There is no way to find "the one node that has not arrived" without visiting every node
- **Severity:** design change needed at the stated scale; clarification at the Pi-cluster scale. **Confidence:** medium-high.
- **Location:** `tools/stat/` (node-local by construction); `progress.h:106-109`; §3.14.
- **Arithmetic:** in experiment 1 at 500 × 256, 499 node segments read `arrival_posted == released + 1` with 256 PEs each in `shmem_barrier_all`, and one reads `arrival_posted == released` with one PE showing `-`. `osoe-stat` has JSON output, so `srun --overlap --jobid … osoe-stat -j` (*recalled* Slurm usage, unverified) plus a script works, but every site writes that script.
- **What it needs, in order of value:** (1) the root's per-team arrival count and lowest missing node as node-scope metrics — node 0 then answers "who" directly; (2) a `--summary` mode printing **one line per node** (PEs blocked by routine and detail, PEs not blocked, `arrival_posted`/`released`, lead heartbeat delta over a short interval) so that `sort | uniq -c` isolates the odd node; (3) the hostname in the node header — a segment names `node 1`, not a machine.

### D-6. Several internode fatal messages omit the peer
- **Severity:** minor. **Confidence:** high.
- **Evidence (*observed in source*, 19 call sites sampled):** *name everything needed:* `watchdog.c:67,78` (routine, local id, rank, pid), `transport-am.c:607` (node, seconds), `transport-am.c:224,386,427` (node and PE), `progress-host.c:112` (team, epoch, cause), `progress.c:204,216` (team, both epochs), `barrier.c:63` (team, epoch, cause), `bootstrap.c:106,113,121` and `metrics.c:222` (variable and offending value), `transport-am.c:407` (bytes, PE, landed). *Do not:* `transport-am.c:348` "no %s reply after %llu s" — no target PE, no node, no routine (*measured*, #4b); `transport-am.c:317` "no reply credit for this connection" — which connection; `transport-am.c:189` "no room for a %u-byte message" — toward which node; `transport-am.c:98` out of memory — fine. None below §4.10 names the API routine; the watchdog does because a `routine` string is threaded to it.
- `osoe_fatal` (`fatal.c:21-44`) prefixes the PE rank and appends the control segment path — *measured* to arrive whole and from non-lead PEs (#4b: PEs 1, 2, 4, 5). It carries no hostname; the node id is recoverable from the segment name.

### D-7. Smaller observations
- **`OSOE_BOOTSTRAP_TIMEOUT` is also the data-path abort timer** (`transport-am.c:181, 309, 338, 601`; `bootstrap.c:118-123`). *Measured*: a lead SIGSTOPped for 20 s (a debugger attach, a checkpoint) kills every PE that was talking to its node. A variable named for bootstrap should not silently govern `shmem_quiet`. **Clarification needed.**
- **The heartbeat rule will stop being true at step 3.** §3.14: "Two reads showing the same values mean the lead is stalled." *Measured*: it works today because the idle lead spins (≈3,000 loops/s, 240 ms CPU in 9 s, #1). Once the lead sleeps in `io_uring_enter`, equal reads mean *idle or stalled*. It needs "work pending" state beside it (sealed-but-unsent buffers, which D-3's record gives). **Clarification needed.**
- **`blocked.detail` is untyped** (`blocked.h:17-19`) and `osoe-stat` always prints it as `(+0x…)`: an epoch appears as `shmem_barrier_all(+0x3)`. **Minor.**
- **`osoe-stat` trusts the directory's counts.** *Measured* (partial fuzz, `logs/fuzz-stat.log` is empty because the harness was OOM-killed; reproduced by hand on two kept cases): `block_count = 4278190096` makes `--threads` run silently for >10 s and the JSON/dump modes emit enough output to exhaust a 7.7 GB capture; `local_count = 16777218` makes `--counters` print metric-table bytes as PE rows 2–7, because `record()` (`segment.c:169-197`) bounds a record against the *file*, not against its *section*. **No signal was observed** in 57 rounds × 5 modes (19 each of directory damage, table damage with a recomputed checksum, truncation). **Minor**, confidence medium — the fuzz is incomplete. Fix: clamp each count to `(segment_bytes - offset) / element_bytes` and to the next section's offset.

## B. Deferral table

Ratings: **safe** / **costlier** (safe but costlier than stated) / **foreclosed**.
Nothing examined is foreclosed.

| Deferred item | Rating | Evidence and the hidden assumption, if any | Basis |
|---|---|---|---|
| Real sockets + io_uring (step 3) | **costlier** | The interface is socket-shaped where it matters: `osoe_wire_send`/`recv` return a byte count and the engine tracks a partial send (`wire.h:125-128`; `progress.h:80-81` `sent`/`sending_bytes`). What does not survive: (1) the wake — `osoe_wire_seq`/`osoe_wire_wait` (`wire.h:134-142`) is a futex word in the shared ring, and the lead's idle wait must become one ring waiting on sockets *and* `agent_wake`; (2) setup and teardown are words in a shared header (`wire.c:239` `ready`, `wire.c:301` `leaving`) — connect/accept ordering, half-close and `ECONNRESET` have no analogue yet; (3) the three 20 µs `nanosleep` polls (`transport-am.c:184, 313, 604`) are documented as "step 3 is where it becomes a wait" but need a wake word the lead bumps on ack/credit, which does not exist; (4) the heartbeat (D-7). | `wire.h` and call sites only; `wire.c` not read in full. Confidence medium-low. |
| pidfd liveness, abort propagation, `shmem_global_exit` (step 4) | **safe**, after D-2 | The abort word, `abort_pe`, `abort_status` and `osoe_node_declare_abort` exist (`metrics.c:156-158`, `watchdog.c:77`), every PE has an agent thread to call `exit`, and the handler table can carry an abort AM. But §3.15's "every library wait also checks the abort word" is false for the four AM loops — *measured*, #5d. Sibling (d) (`exit()` → atexit finalize) is on this path and must be settled first. | Source + experiments. |
| Symmetric statics (§4.3 phase 1.5) | **safe** | A region is already a parameter of `resolve` and `notify` (`progress.h:52-59`), of the wire messages (`uint16_t region`), and of the watch word (`osoe-stat` decodes `heap +0..+0x8`); the enum reserves the slot (`transport.h:69-70`). Assumes-two-regions sites found: `node.h:323` (`heap_of`, a per-local-PE array of heap bases only) and `pe.heap_name`/`--gc` (one object per PE). | Greps; allocator and `symmetric.c` not read. Confidence medium. |
| Collectives beyond barrier/sync (§5.5, §5.8) | **costlier** | The internode phase is **barrier-shaped and a star**: every node's lead sends one arrival to node 0 (`progress-host.c` `#define OSOE_COLLECTIVE_ROOT 0u`, `participants()` = `node_count`), node 0 counts (`progress.c:207-221`, `652-667`) and sends every release. At 4,096 nodes that is 4,095 inbound and 4,095 outbound messages through one lead per barrier — O(nodes) at the root where §5.2 promises a tree. Messages carry `(team, epoch)` and no payload. Hidden assumption: **one in-flight epoch per team** (`progress.c:213-217` aborts on two) — correct for blocking collectives, and the same thing §5.4 says nonblocking collectives break. Reusable: the per-team handoff word, the `release` host callback, `osoe_epoch`. To be written: payload, accumulator, tree, and per-epoch rather than per-team root state. | `progress.c`, `progress-host.c` excerpts. Confidence medium-high on "star", measured nothing. |
| Nonblocking RMA and AMOs | **costlier** (gets, fetching AMOs); **safe** (puts) | Documented at `node.h:274-277`: one landing slot per PE, "enough while gets and fetching AMOs are blocking". `await_reply` matches one `token` and one `ready` flag (`transport-am.c:335-351`); the copy from landing zone to the private `dest` happens in the *blocked caller*, so an nbi get needs a per-operation record (dest, length, token) that outlives the call and a completion pass in `quiet`. `landing_count` is in the frozen directory as "local_count" (`node.h:161`) — a separate field, so growing it is an append, not a break. None of `shmem_*_nbi` is declared in `shmem/include/shmem.h` (*measured*: grep finds nothing), so a program using them fails to build rather than misbehaving. | Source. |
| Locks | **safe**, cost unstated | Nothing exists: no `set_lock` anywhere under `shmem/` (*measured* grep). A distributed queue can live in the lock's own symmetric `long` (64 bits here: two 20-bit PE ids fit for 1M PEs) driven by AMOs at the lock's home PE, with the grant as a deferred reply — compatible with §3.7 "may enqueue, never reserve". The waiter then needs a reply slot or a watch, and so inherits the one-landing-slot and one-watch limits below. | Reasoning, not code. Confidence low. |
| `SHMEM_THREAD_MULTIPLE` (§4.11) | **safe** — §4.11 lists what I found | Per-PE singletons: `static struct osoe_transport_watch the_watch; /* one wait at a time */` (`watch.c:39`); one `watch_rng`/`watch_seq` per PE slot; one landing slot per PE (`node.h:274`); `static struct osoe_transport_context default_context` holding the dirty set (`transport-am.c:73`); and the **"blocked in" record, one per PE slot** (`metrics.c:297-315`) — its generation protocol is single-writer, and two threads bumping `blocked_gen` concurrently restore even parity mid-write, which a reader accepts as valid. §4.11 item 5 moves the record into the thread block (*documented*), and counter blocks are already per-thread with TLS claim (`metrics.c:232-282`), so the home exists. `osoe_metrics_table`'s lazy `static bool built` (`metrics.c:192-206`) is non-atomic; harmless if first called in init. | Greps over five files; §4.11 skimmed by grep only. |
| Teams beyond the two predefined | **costlier** | §5.7 has no allocator or split protocol, and the code hard-codes two in more places than the enum: `static struct osoe_team world_team, shared_team` and identity comparisons `t == &shared_team` (`team.c:31, 80, 90`); six **named** epoch fields in the node header (`world_arrived … shared_copied`, `metrics.c:136-141`) and two named per-PE epochs (`pe.world_epoch`, `pe.shared_epoch`); `release()` is an if/else over two teams (`progress-host.c:100-110`); `participants()` aborts for any team but world (`progress-host.c:127-133`); metric names embed the team (`team.world.arrived`). Only `arrival[OSOE_TEAMS]` and `arrivals[OSOE_TEAMS]` are arrays. The self-describing table has **no way to say "N records of team scope"** — a fourth scope again (D-3). Split and `ctx_create` are stubs returning nonzero (`team.c:171, 190, 227, 237`), which is the conforming loud form. | Source. |
| Contexts | **safe** | `pshmem_ctx_create` returns 1 and `SHMEM_CTX_INVALID` (`team.c:222-228`); one `default_context` owns the dirty set. A context is a dirty set plus options, and the boundary already passes `osoe_transport_context *`. | Source. |
| §7.5 typed RMA cross product | **safe**, mechanical | Every typed put reduces to `putmem` at the boundary (`transport-am.c:466-467` counts bytes, not types); `test-amo-types.c` shows the pattern for AMOs. Not examined further. | Light. |
| §3.14 flight recorder | **safe** | Per-progress-thread ring in the control segment; the directory is append-only and `OSOE_SEGMENT_HAS` makes the append two-directional. Needs a record *kind* the table cannot yet describe (a ring, not a scalar) — same family as the missing connection and team scopes. | Design text + `segment.h:78-92`. |

**The hidden-assumption hunt.** Equivalents of §5.4's equality-compared epochs:
(1) *one in-flight epoch per team at the root* (`progress.c:213-217`);
(2) *one landing slot, one token, the caller is blocked so its `dest` is live*
(`node.h:274`, `transport-am.c:335`); (3) *one watch per PE* (`watch.c:39`);
(4) *one blocked record per PE with a single-writer generation*
(`metrics.c:297`); (5) *the root is node 0 and every node participates*
(`progress-host.c:127-135`); (6) *an idle lead spins, so an unchanged heartbeat
means stalled* (§3.14). (1)–(4) are documented at their sites; (5) and (6) are
not called out as assumptions anywhere I found.

## What is sound

- **The self-describing segment works as designed.** `osoe-stat` found every value by name, decoded the watch word only for its own layout, and the `OSOE_SEGMENT_HAS` per-field test makes append-only two-directional (`segment.h:78-92`, `segment.c:121-137`). *Measured:* through ~285 runs over damaged copies it never died on a signal; offsets are bounds- and alignment-checked before an atomic load (`segment.c:205-215`).
- **The "blocked in" record and `pe.sweeps` are the right pair** for SHMEM-layer waits. Asleep-but-alive (sweeps rising), not-in-the-library (`-`, sweeps 0) and dead (`[pid is gone]`) were distinguishable at a glance in #1, #2, #5b.
- **`arrival_posted` against `released`** (`metrics.c:142-151`) did exactly what its comment promises: it separated "this node has not arrived" from "the network has not answered" in #1.
- **Counters are really per-thread and unshared:** TLS block pointer, CAS claim, 512-byte blocks (16 × 512 for 2 PEs), a thread-local orphan on exhaustion and the fact recorded in the PE slot (`metrics.c:232-282`). `--threads` showed agent and application blocks with kernel CPU time and involuntary switches read from `/proc`.
- **The watchdog's message is a model** of what a fatal should say, and the abort word it sets is visible in `osoe-stat` as `ABORTING on behalf of PE 3`.
- **`osoe_fatal`'s single write** delivered whole, uninterleaved lines from five PEs dying together, each with the segment path to inspect, including from non-lead PEs.
- **A stopped or dead *non-lead* PE does not stall its peers' RMA** (#3, #5a) — a real property of the lead-applies-everything design, and a reason the lead is the single point whose health must be observable (D-3).

## Lessons for a successor API

- **`shmem_wait_until` has no notion of who will satisfy it**, so no tool can ever name the culprit of experiment 2 — only the victim. A wait that names its expected writer (or a team) would make that hang as diagnosable as a barrier.
- **Collectives identified only by call order** ("the k-th collective on this team") force epoch counting and make "PEs called different numbers of collectives" detectable only as divergence after the fact. An explicit operation tag or sequence argument would let a hung barrier report *which* call site each PE is at.
- **One-sided operations with no completion object** push all failure to `quiet`, which is why the victims of #4 die in `shmem_quiet` with a message about a node rather than about the put that could not be delivered.
- **An unbounded, untyped `long` lock word and PE-global state (one default context, implicit world team)** are what make `THREAD_MULTIPLE` a retrofit; a successor should make the context the only handle and give it its own diagnostics record.
- **The specification says nothing about failure**, so every abort path, timeout and environment variable here is invented. A successor should define a minimal error model (peer failure is reported, to whom, by when) so that diagnosability is an interface and not a per-implementation investment.

## Not investigated

- `wire.c`, `connection.c`, `reply.c`, `team.c` and §4.11 were not read in full; deferral ratings marked medium/low confidence rest on headers, call sites and greps.
- The `osoe-stat` fuzz ran 57 of 300 rounds; the torn-read path of the blocked record was not attacked (its model in `docs/design/evidence/blocked-record/` was not re-run).
- A *different layout version* was tested only through `test-segment.c`'s existence, which I did not run or mutate.
- Sibling (c) (ack echo makes idle look busy) and (d) (`exit()` → atexit finalize) were not reproduced.
- Experiments 1–5 were run once each (they are deterministic hangs, not timing results); only the lost-wake timing has a control run. No experiment was run under the `release` preset, where the watchdog is off and #5b would hang on every node.
- The §3.14 cost rule "timing only on slow paths" was not audited beyond noting that `osoe_barrier_all` reads the clock twice on every call (`barrier.c:92-100`), sleeping or not.
