# Finish phase 1, step 2, first slice: the in-process AM backend

You are continuing implementation work on **osoe** (OpenSHMEM Over Ethernet) on
branch `phase1-step2-slice1`. A previous session built everything below the
§4.10 boundary and proved it works; what is left is joining it to the SHMEM
layer. **Read `docs/prompts/done/phase1-step2-slice1-prompt.md` first** — it is the
slice's brief and every rule in it still applies. This document says what has
been done, what was decided and why, and what is left.

Read `CLAUDE.md` too. It gained two sections this session — the naming standard
and the rule about mechanical renames — and they are followed throughout.

## State

Branch `phase1-step2-slice1`, clean tree, **119 tests, all four presets green**
in the VM (`limactl shell osoe cmake --workflow --preset debug|release|asan|tsan`).
The instance is named `osoe`; §8.5 and `dev/lima/create.sh` use that name.

Eighteen commits, newest first:

| Commit | What |
|---|---|
| `1cb1770` | The internode phase of a barrier, and `osoe_transport_sync` closed |
| `ed4fbc6` | The job-wide rendezvous and the one locality function |
| `2541a33` | The progress loop: the first bytes across a node boundary |
| `f761fae` | AM framing, the handler table and the stream parser |
| `951747a` | Landing zones at control-segment layout 6 |
| `eafbb6b` | Deferred reply queues and the per-connection credit |
| `4ff4650` | Review documents named for the work they reviewed |
| `630a41e` | Finish the names, and fix a rename that compiled while wrong |
| `b947f5b` | Spell out the pre-existing abbreviations |
| `0374e1a` | Spell out this slice's names, and kebab every file name |
| `8d7e846` | The naming standard: names are words |
| `445b31c` | The connection area, with the AM sizing policy |
| `99b66cd` | The segment structure and the reservation protocol |
| `364fa66` | The working agreement on delegating to subagents |
| `3f82b2f` | The node-pair wire: a byte stream, not a message queue |
| `d503dbb` | Models of the wire and the reply credits, before the code |
| `aa4fc27` | `osoe_transport_set_range` and its allocator call site |
| `80bb3d0` | The §8.1 logical node map, derived by the launcher |

**Read those commit messages.** They carry the reasoning, and most of them
record a defect that a green test suite did not catch — which is the part worth
knowing before writing more tests.

## What exists, layer by layer

Everything below the §4.10 boundary is built and independently tested.

| File | What it is |
|---|---|
| `base/node-map.{h,c}` | §8.1's logical node map. `osoe-launch` reads it and derives each rank's `OSOE_NODEID`, `OSOE_STEP_NUM_NODES`, `OSOE_LOCALID`, `OSOE_GTIDS` — the library never sees the map |
| `shmem/src/job.{h,c}` | §6.2's job-wide rendezvous, simulated in shared memory, and `osoe_is_local()`, the one locality function of §8.1 |
| `shmem/src/wire.{h,c}` | The node-pair byte stream that stands in for the socket, with deterministic short writes |
| `shmem/src/connection.{h,c}` | §3.4's segment header and §3.5's reservation protocol |
| `shmem/src/connection-area.{h,c}` | The per-node AM shared object and the sizing policy |
| `shmem/src/reply.{h,c}` | §3.8's descriptor pool, control and bulk queues, and the per-connection credit |
| `shmem/src/am.{h,c}` | §3.13's handler table and §3.3's stream parser |
| `shmem/src/progress.{h,c}` | The progress loop: receive, apply, drain, send |
| `shmem/src/rmw.h` | One definition of an rmw, shared by the intranode backend and the AM handler |

`tools/modelcheck/wire_model.py` and `credit_model.py` run as tier-1 tests
(`model.wire`, `model.credit`); the evidence is in
`docs/design/evidence/am-backend/`.

## Decisions taken this session, and where they are recorded

- **`osoe_transport_sync` is closed: no.** §10 scheduled the decision for the
  step that writes its only caller, and the code disagreed with the estimate.
  The internode phase addresses a *node* where §4.10 addresses a PE, and its
  release wakes futex waiters on an epoch counter where §4.10's notify wakes
  watch waiters on a range. Recorded in §10 and §5.3, and beside the code in
  `progress.c`.
- **The connection area is a separate object from the control segment**, and
  that keeps §10's "does the AM data path stay hot in an RDMA build" open rather
  than settling it by accident. §3.4.
- **AM sizing** (`OSOE_AM_CHUNK`, `OSOE_AM_BUFFER`, `OSOE_AM_BUFFER_BUDGET`,
  `OSOE_AM_HUGEPAGES`), with the floor as the default and huge pages off — the
  opposite of §6.2's rule for heaps, because connection buffers are touched
  sparsely. §3.4.
- **`OSOE_TP_MORE` defers the wake, not the seal**, and both backends ignore it.
  §4.10 and `transport.h`.
- **The node map is the launcher's input, not the library's.** §8.1, §6.2.
- **Names are words.** `CLAUDE.md`, and the whole tree renamed to match.
- Control segment is at **layout 6** (landing zones). `tools/stat` gained
  `OSOE_SEGMENT_HAS`, because its old length check made §3.14's append-only rule
  one-directional.

## What is left, in the order to do it

### 1. Open `shmem_init` to a multi-node job

The door-opener, and the design is already worked out. It must land with the
internode barrier wired in, because a multi-node job that initialises but whose
`barrier_all` only synchronises the local node is silently wrong.

- `session_start()` in `init.c`: after `osoe_node_join`, call `osoe_job_join`,
  then (when `node_count > 1`) open the connection area, open a wire to each
  peer, and `osoe_progress_init` + `osoe_progress_attach`. Reverse it in
  `session_end()`. Remove the refusal at the top of `session_start`.
- `agent.c`: the **lead's** agent runs `osoe_progress_poll` each iteration and
  sleeps only when it returns false, keeping the `OSOE_SWEEP_NS` deadline. Every
  other agent is unchanged (§3.12).
- `team.{h,c}`: add `id` (0 world, 1 shared), `local_members` and `internode`.
  **The arrival target is `(k+1) · local_members`, not `(k+1) · members`** — on
  one node the two coincide, which is why the current code works.
- `barrier.c`: in `sync_team`, when the team is internode, the member whose
  increment reaches the target does *not* bump `released` itself; it asks the
  lead to send the arrival. The release arrives at the progress thread, which
  bumps `released` and wakes every local waiter (§5.3).
- **The arriver cannot call `osoe_progress_arrive` itself.** Only the lead holds
  the progress state, and only the lead touches a connection's send path
  (§3.12). Add a per-team word to the node header — control segment **layout
  7** — in which the arriver leaves the epoch and then wakes the lead's agent;
  the agent consumes it and performs the arrival. A partially written version of
  this is described in `1cb1770`'s discussion but was reverted; nothing of it is
  in the tree.
- `osoe_transport_direct` must return NULL for a PE that is not local, by
  consulting `osoe_is_local()`. That is what makes `shmem_ptr` return NULL
  across logical nodes (§8.1).

### 2. The data path behind §4.10

put, get and rmw dispatching on locality; §4.6's dirty set per context;
`fence`, `quiet` and `pe_quiet` with ack-only segments; `put_signal` as a put
followed by a fenced rmw. `progress.c` already applies all of these on the
receiving side and `osoe_progress_acked()` already reports what a peer has
applied, which is what `quiet` waits on.

### 3. Force-remote and the differential test

`OSOE_FORCE_REMOTE` is already parsed and already consulted inside
`osoe_is_local()`. What is left is the test: the same program over both paths,
results diffed, over §8.1's four poles. **Make it cheap to run**; it is the
reason this step exists in this order, and it is the gate for the review below.

### Then

Summarise the slice, and **ask before merging into `main`**. After that, an
adversarial review is planned against the state of the tree at that point —
design, implementation, and the plan for steps 3 and 4. Name its files
`phase1-step2-slice1-*` in `docs/reviews/` (`CLAUDE.md`), and write a short
"what changed since the last review" note with it: the design doc has grown from
about 1,700 lines at the first review to about 4,400.

## Testing lessons from this session, which cost real time to learn

These are not general advice. Each one is a specific mistake that produced a
green test suite over broken code, in this codebase, this week.

- **A test with one PE per node proves nothing about rules that exist because
  PEs share things.** §3.5's buffers are shared by every local PE, so a single
  writer never exercises the seal bit, the one-reservable rule, or "sendable
  only when every reservation has been committed". Three mutations passed a
  one-PE-per-node test of the progress loop and were caught at two.
- **A two-node test cannot tell a correct barrier from one that releases on the
  first arrival.** The root's own arrival and the single message it receives are
  the first and the last. Use three.
- **A contention test that does not contend proves nothing.** The credit test
  caught a racy take 0 times in 6 runs with three credits and 6 of 6 with one,
  because only then is the exhausted state common.
- **Check that an audit command can find something you know is there** before
  believing an empty result. A doubled-substitution audit reported a clean tree
  because its pattern matched nothing at all, and 74 corrupted identifiers
  shipped in a commit that passed four presets.
- **Verify that a mutation you expect to fail actually does.** The one time an
  expected failure passed, it was because the code had an extra mechanism that
  made a design rule redundant — which was the real defect.
- **Run the whole suite repeatedly, not the single test.** A false assertion in
  the progress test failed about one run in twenty in the full suite and passed
  8 of 8 in isolation.
- **Publication ordering belongs to a model, not a test.** A defect where a
  reader published `consumed` before copying the bytes out was invisible to a
  single-process test and caught by a two-process test in 4 runs of 8; the model
  reaches it in every interleaving.
- Two shared-memory objects were unlinked by whichever process finished first,
  leaving a later opener waiting for something that could never appear
  (`arrivals_close()`, and the job segment). §6.2's rule — keep the name until
  the last user leaves — is the fix, and it applies to any new one.
- `osoe-launch` is asan-instrumented and long-lived: state added to it must be
  freed on every exit path, or fifty tier-2 tests fail at once pointing at the
  launcher rather than at the change.

## Rules, unchanged and load-bearing

Everything in the slice brief still applies. The ones most often in play here:

- **§3.16's atomics.** ISO C11 only, every shared protocol word an
  `atomic_*_explicit` naming one of the four orders; loads `acquire`, CAS and
  `fetch_*` `seq_cst`, single-writer publication `release`, both stores of a
  two-sided protocol `seq_cst`.
- **Model-check a new concurrent protocol before implementing it**, and break
  every rule the design states to show the failure.
- **Break the mechanism on purpose** when a test asserts that something blocks,
  wakes or is ordered, and record it in the commit message.
- **The design doc stays true.** A decision the doc does not make, or one it
  makes wrongly, is corrected in the same commit with its rationale.
- **Every commit builds with no warnings and passes every test in all four
  presets in the VM.**
- **Blockers are raised, not worked around.**
- **`docs/reviews/` and `docs/prompts/` are records.** The reviews may be
  corrected when a rename makes them wrong; the prompts are the briefs as they
  were given and are never rewritten — `docs/prompts/README.md` records the two
  known driftings instead.
