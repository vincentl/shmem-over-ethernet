# Implement phase 1, step 3, first slice: a real socket, and io_uring under it

You are starting **§7.1 step 3** of **osoe** (OpenSHMEM Over Ethernet). Step 2 is
complete and merged (`a50c829`): an AM backend that carries every internode
operation over a *shared-memory stand-in* for a wire, a k-ary collective tree
behind `internode.h`, and a progress engine whose work discovery, metrics and
diagnosability were built in slice 3. **This step replaces the stand-in with a
socket and an io_uring ring, and nothing above the transport boundary should
notice.**

That last clause is the measurement of this slice. §4.10 and `internode.h` were
built so that a second transport replaces `transport-am.c` and its files without
touching the SHMEM layer; `shmem.boundary` enforces it. If this slice ends with
edits above the boundary that were not forced by a defect, say so plainly in the
final summary — that is a finding about the seam, and worth more than a clean
diff.

The author's goals, which are the axes to judge your own work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node**, with no serial
  bottleneck where a cheap alternative exists.
- **A path to RoCE at ~10⁶ PEs (4,096 nodes × 256) that no interface blocks.**
- The Ethernet path is graded at *hundreds of nodes × 256 PPN*; the seam and the
  inherited data structures are graded at 4,096 × 256.

Machine setup: "Setting up on a new machine" in `phase1-step2-slice1-prompt.md`.
Meaning of phase/step/slice: `docs/prompts/README.md`.

## Read first

1. `CLAUDE.md`.
2. Design §3.3 (io_uring: sole issuer, multishot receive, stream parsing, one
   in-flight send, the two ring bounds, `TCP_NODELAY`), §3.10 (the idle loop and
   why step 2 polls), §3.2, §3.4, §3.9, §6.2 (the socket rendezvous, its port
   rule and its failure handling), §8.4 (the floor harness), §3.15.
3. §10's still-open entries: the 8-byte round trip and small-message rate, the
   `echo-uring` latency assumption, and "authenticating the step-3 handshake".
4. `shmem/src/wire.[ch]` — the stand-in this slice replaces, and in particular
   what its contract promises the engine (byte stream, split injection,
   shutdown handshake). `progress.c`, `progress-host.c`, `transport-am.c`,
   `connection-area.[ch]`, `am.[ch]`.
5. `tools/probe` — every kernel assumption this step depends on is already
   checked there; run it first on any machine.
6. `docs/reviews/phase1-step2-slice1-review.md` findings D2, D3, D12 and the
   `algorithmic-scaling` report's finding 9 (the eager full mesh), for what this
   step is expected to inherit rather than repeat.

**`docs/reviews/` is a committed record: read it, never edit it.**

## What step 2 left for this step, deliberately

Each of these is recorded in the design notes as step 3's work. None is a defect
to be rediscovered:

- **Receiving polls every wire, every pass** (§3.10). The shared-memory wire
  cannot say when bytes arrive, so the lead looks at all N − 1 connections per
  pass: O(N) per pass, and the 250 µs `OSOE_PROGRESS_SLEEP_NS` interval that
  costs one poll per tree hop — which is why the slice-3 barrier measurement got
  *slower* with depth on the stand-in (§5.3). Completions delete both. The
  **send** side already discovers work in O(active) through the dirty-connection
  bitmap and the claimed `lead_sleeping` wake (§3.10); keep that, and make the
  receive side its equal rather than replacing it.
- **The rank → node map is O(P) entries broadcast O(N·P) bytes** from node 0
  (§6.2). The range-compressed map and a tree-shaped broadcast belong with the
  socket rendezvous, which is this step.
- **The mesh is eager**: every lead opens a wire to every peer at init
  (algorithmic-scaling 9). At 4,096 nodes that is 8.4 M connections job-wide and
  a listen backlog no one has sized. Nothing in slice 3 made laziness harder —
  keep it that way, and say in §6.2 what on-demand would need, even if you do
  not build it.
- `osoe_transport_set_range` is a no-op on AM. §4.10 says step 3's backend is
  the first that *acts* on it. Decide whether that is this slice.

## Order of work

Judgement calls are the point of the ordering: each part should be able to fail
and be corrected before the next depends on it.

### Part A — the floor, before the transport

`echo-uring` (§8.4): one connection, io_uring, ping-pong an 8 byte payload, and
a `--stream` mode for bandwidth. Roughly 200 lines, registered as a test, with
its constants pinned after the first run so a regression is a failure. It is the
smallest reproducer for every io_uring bug this step can hit, and it is the
experiment §10 has been waiting for: **wire latency dominating memory bandwidth
by two orders of magnitude is the assumption the whole design rests on.** Report
the ratio, not only the microseconds, and put it in §8.4 and §10.

### Part B — the ring, behind the wire's contract

Replace the shared-memory wire with a socket and a ring **without changing the
engine**: `osoe_wire_send`/`recv` keep their meaning, and `progress.c` keeps its
pass. §3.3 fixes the shape: `IORING_SETUP_SINGLE_ISSUER`, multishot receive over
a registered provided-buffer ring, **parse as a stream and never hold a provided
buffer**, one in-flight send per connection, both ring bounds asserted at init,
`TCP_NODELAY` with the comment §3.3 asks for. `ENOBUFS` and any CQE without
`IORING_CQE_F_MORE` re-arm; both are normal.

Keep the stand-in's fault injection (`OSOE_WIRE_SPLIT`, `OSOE_WIRE_SEED`) or its
equivalent: the tests that split a segment across reads are how the stream parser
is checked, and they must still split.

### Part C — the idle loop, as §3.10 specifies it

One armed `IORING_OP_FUTEX_WAIT` per ring, submitted with the socket work, so
the lead waits on *both* in one call and the 250 µs poll disappears. The
`futex_armed` flag is private to the progress thread; an earlier draft armed one
per pass and overflowed the CQ with 1,881 armed waits. The safety sweep stays.
Re-measure the barrier against slice 3's numbers (§5.3 carries them): the tree's
latency should now follow hops, not poll intervals.

### Part D — the socket rendezvous

§6.2 as written: only node leads connect, the port is a deterministic hash of
job and step with a candidate sequence, the handshake carries job and step ids,
the server checks `SHMEM_SYMMETRIC_SIZE`, the handler-table checksum and
`OSOE_COLLECTIVE_FANOUT`, and each node's fixed-size backend blob rides along
(the slot exists; AM publishes zeros). Decide `OSOE_JOB_SECRET` here — §10 lists
it as "decide with step 3" — and say what you decided either way.

This is where the O(N·P) broadcast and the compressed map belong.

### Part E — what the tests must now say

The logical node map (§8.1) is still how a job is made to span nodes on one
host, and every tier-2 test uses it. A socket changes what "node" means: two
logical nodes on one host now talk over loopback. Say what that does and does
not test, and keep the four poles. The differential test, `shmem.internode.*`,
`shmem.tree.*` and `shmem.hang-walk` must pass unchanged — if one needs
changing, that is a finding about the seam and belongs in the summary.

## Out of scope

Step 4 (pidfd liveness, abort propagation, `shmem_global_exit`, the watchdog's
replacement). New collectives, data-carrying messages, team split, contexts,
nonblocking anything. An RDMA backend. Sharding the intranode arrival counter.

## Rules

The Rules and Testing sections of `phase1-step2-slice1-prompt.md`, as amended by
slices 2 and 3, apply. In addition:

- **Complexity is a correctness criterion.** For every structure or loop, state
  in the commit message its cost in N (nodes), L (PEs per node), P and T (teams),
  in time and memory, per PE and per node. Anything O(P) per PE, or O(N) on a
  per-operation path, is a defect.
- **Model any new protocol before writing it**, with a broken variant that fails
  the property it targets and the trapped-state check on (`tools/modelcheck`).
  Slice 3 found two false premises this way, one of them the brief's own.
- **Memory orders come from the C11 model and are minimal** — §3.16's table, and
  add a row for any new shape.
- **Run the load loop** (`build/scratch/load/load-loop.sh`, or slice 3's
  `load-some.sh` for a subset) before calling any part done, and never while
  editing the tree: `shmem.boundary`, `shmem.strict-c11` and `shmem.static-link`
  read or rebuild the *source*, and a mid-edit tree fails them.
- Layout bumps: at most one per part; say what an older `osoe-stat` does.

## Working method

- State a short plan, then proceed. **Do not stop for approval. Stop only on a
  surprise**, saying what you expected, what happened, and the options:
  - `tools/probe` fails on the machine, or a kernel assumption of §3.3 does not
    hold;
  - the engine cannot keep its pass without a change above the boundary;
  - a model contradicts a pinned decision;
  - `echo-uring` shows the latency assumption of §10 to be wrong — that is a
    result, not a failure, and it changes copy, futex and chunk decisions;
  - the socket rendezvous needs §6.2's protocol to change beyond the blob.
- Branch `phase1-step3-slice1`. Small commits, warning-free, all four presets
  green in the VM, the load loop on every test added or changed, messages that
  say what was verified and how, the design doc changed in the commit that makes
  it true.
- Short progress notes at the end of each part.
- When done: summarize commits, verification, design-doc changes, measurements
  with their caveats, deferrals, open questions; add a row to
  `docs/prompts/README.md`; and add to `docs/design/successor-api-stage-setting.md`
  §2 any lesson this slice paid for. **Ask before merging into `main`.**
