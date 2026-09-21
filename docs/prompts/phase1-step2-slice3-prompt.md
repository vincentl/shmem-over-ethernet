# Implement phase 1, step 2, third slice: the internode seam, the tree, and scale

You are finishing **§7.1 step 2** of **osoe** (OpenSHMEM Over Ethernet). Slice 2
(`phase1-step2-slice2-prompt.md`) fixed what the second review found in the
protocols and failure paths. **It must be merged before you start**: this slice
builds on its acknowledgement protocol, its reply-pool sizing (`degree`), its
missed-wake counter, its model-engine trapped-state check and its node-scope AM
metrics. If `main` does not have them, stop.

This slice serves three of the author's goals directly, and they are the axes to
judge your own work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node, with no serial
  bottleneck** where a cheap alternative exists.
- **A path to RoCE at ~10⁶ PEs (4,096 nodes × 256) that no interface blocks** —
  which today means: collectives must sit behind a seam, not above one.

Machine setup: "Setting up on a new machine" in `phase1-step2-slice1-prompt.md`.
Meaning of phase/step/slice: `docs/prompts/README.md`.

## Read first

1. `CLAUDE.md`.
2. `docs/reviews/phase1-step2-slice1-review.md` — findings D5, D6, D7, D10, D12,
   and the two scoring tables.
3. `docs/reviews/phase1-step2-slice1-roce-seam.md` — §3 (leaks, with file:line),
   F1, F4, F5, F6, F7, F9, F10.
4. `docs/reviews/evidence/phase1-step2-slice1/reports/algorithmic-scaling.md`
   (findings 2–5, 9) and `reports/premises.md` (findings 1–3).
5. Design §4.10, §5.1–§5.4, §3.8, §3.10, §3.12, §6.2, §8.1, §9, and §10's entries
   on `osoe_transport_sync`, "A boundary progress call", and "dispatch per
   (pe, operation class)".
6. `shmem/src/transport.h`, `barrier.c`, `progress-host.[ch]`, `progress.c`
   (`:200-238`, `:575-584`, `:623-692`), `agent.c`, `init.c`, `state.h`, `job.[ch]`,
   `node.c:385-400`, `bootstrap.c`.

**`docs/reviews/` is a committed record: read it, never edit it.**

## Who does what

As in slice 2: you orchestrate and do the load-bearing work — the interface, the
tree, the models, every design-doc change. Sonnet subagents (at most two at a
time) run builds, presets, load loops, greps, mechanical edits and checklist
sweeps.

## Decisions already made — do not reopen

Decided by the author on 2026-09-20.

1. **`osoe_transport_sync` stays closed *in `transport.h`*, and its reasoning is
   rewritten.** §4.10 remains point-to-point and PE-addressed. The internode phase
   of a collective instead gets its **own, sibling interface**, bound at build time
   exactly as `transport.h` is, with AM as one implementation. §10's closed entry
   was decided on two reasons the code contradicts — what crosses at `barrier.c:61`
   is already `(team, epoch)`, and what comes back at `progress-host.c:99` is one
   release call; nothing node-addressed leaks upward and the backend needs no
   knowledge of futexes — and priced against SHARP, which RoCE switches do not
   have. The real baseline was a TCP star.
2. **The AM implementation of that interface is a tree, built now**, before step 3,
   on the shared-memory wire — the project's own rule that every internode semantic
   is validated before a socket exists.
3. **k-ary tree over the index of participating nodes**, parent of index `i` is
   `(i − 1) / k`, children `k·i + 1 … k·i + k`. Default `k = 4`,
   `OSOE_COLLECTIVE_FANOUT`. O(k) work per node, O(log N) depth, O(1) state per
   team per node.
4. No fatal time bounds (slice 2's decision 1) — nothing here may add one.

## Order of work

### Part A — the internode collective interface

**A1. The header.** A new `shmem/src/internode.h` (file name and identifiers
spelled out per CLAUDE.md; `osoe_internode_*`). It is a contract in the style of
`transport.h`: what each call promises, who may call it from which thread, what it
costs, what a backend may do underneath. Minimum surface, from what the code
already does:

```c
/* Down: this node has arrived for (team, epoch): every local member is in. */
void osoe_internode_arrive(uint32_t team, uint32_t epoch);
/* Up, called by the backend on its own thread: the group has been released. */
void osoe_internode_released(uint32_t team, uint32_t epoch);   /* implemented above */
/* Lifecycle, and the participant set a team's internode phase spans. */
```

Design the rest — lifecycle, how a team's participating-node set is given to the
backend, threading — and write down in the header **why the shape will survive
data-carrying collectives**: §3.8's "known future break" (a tree reduce forwards
`count × sizeof(type)` bytes) means `arrive` will grow a payload. Do not build
that; do not foreclose it. State in the header what an RDMA backend would do with
each call (a tree over put and atomics; or hardware offload).

**A2. Route the existing code through it.** `barrier.c` calls the interface and
no longer includes `progress-host.h`. `release()` (`progress-host.c:99`) becomes
the upcall; its two-way `if` on team is fine for now but must not be the reason a
third team cannot exist — index by team number.

**A3. Lifecycle and progress.** `init.c:120-131` and `agent.c:70-72` call the AM
engine by name (roce-seam F5), and §10's closed row "every place the SHMEM layer
can block is a backend function, so no progress call is needed" is now false:
`barrier.c:69` blocks on an epoch futex outside any backend. Give the agent
thread's pass and its wait to the backend through declared entry points, so that
`agent.c` and `init.c` name no AM symbol. Rewrite that §10 row to say what is true.

**A4. Enforce the boundary.** `state.h:10-41` hands six AM headers to eight §4
files that use none of them (F6). Split it, and add a CMake script test in the
style of `cmake/CheckExports.cmake` that fails if any file above the boundary
includes a header below it. Define "above" and "below" as two lists in one place.
*Criterion:* adding `#include "progress.h"` to `rma.c` fails the test.

**A5. Small seam gaps** (F9, F10): `osoe_transport_get` gains a `flags` parameter
(no flag is defined for it yet; say what it is for); `osoe_transport_load` and
`osoe_transport_set_range` move out of `transport-local.c` to wherever a second
backend would not have to edit; a CMake option selects the backend; the job
rendezvous (`job.h`) gains a fixed-size opaque per-node **backend blob** published
before `ready`, unused by AM.

*Criteria for part A:* no behaviour change — all tests pass unmodified; the include
check passes; `transport.h:5-7`'s sentence "The SHMEM layer calls only what is
declared here" is true of `transport.h` plus `internode.h`, and says so.

### Part B — the tree

**B1. Model first** — `tools/modelcheck/tree_barrier_model.py`, recorded under
`docs/design/evidence/`. At least 5 nodes with `k = 2` (depth 2), two epochs, one
member re-entering epoch *k+1* early, SC and TSO where shared words are involved.
Properties: a node is released only after **every** participating node has
arrived; each node is released exactly once per epoch; no node ever holds two open
epochs for one team; no trapped state. Broken variants, each failing its property
*first*: forward upward before all children have arrived; count children across
epochs without resetting; release locally before forwarding down *and* reset after
(the root-node hazard §5.3 already records); a child's arrival applied to the wrong
epoch.

**B2. Implement.** Per team per node, in the progress thread's private state:
the epoch in progress, children arrived, whether this node's own members have
arrived. A node forwards `ARRIVE` to its parent when both are complete; the root
then starts the release; a node receiving `RELEASE` **forwards it to its children
first, then releases locally**, so depth latency is not serialized behind a local
wake. The wire message (`struct osoe_am_barrier {team, epoch, from_node}`) should
not need to change; if it does, that is a surprise. Topology is computed from the
team's participant list — write it for an arbitrary sorted list of nodes even
though only the world team (all nodes) exists, and test the function directly at
tier 1 with sparse lists. `OSOE_COLLECTIVE_ROOT` and `participants()` go.

Slice 2's reply-pool `degree` becomes `k + 1`. §3.8's queue-bound argument 2
("tree degree × concurrent teams") is now about a real tree: re-derive it in the
doc.

**B3. Test.** Node maps of 7, 8 and 13 logical nodes, with `OSOE_COLLECTIVE_FANOUT`
2 and 4 (depth 3 at 8 nodes with `k = 2`), one PE per node and uneven
(`0,0,0,1,2,2,3,4,5,6`-style). The `arrivals.h` technique proves no PE leaves
before the last arrives. Mutations, each recorded: forward on the *first* child;
skip forwarding the release to the last child; do not reset the child count.
Slice 2's missed-wake counter must stay zero.

**B4. Measure, roughly.** Barrier latency against logical node count 2…16, before
and after, release preset, idle VM, median of many. The shared-memory wire hides
the star's O(N) term, so this is a *no-regression* check, not proof of the win;
say so. Put the arithmetic for 500 and 4,096 nodes on TCP in §5.3 with its
assumptions labelled.

**B5. Design doc.** §5.1–§5.3 describe the tree as built. §10's
`osoe_transport_sync` entry is rewritten per decision 1 — keep the history, mark
what was wrong. §9's invariants: check each one that mentions the root or node 0.

### Part C — work discovery on the send side

(D12; algorithmic-scaling 5.) Today every remote operation does an unconditional
locked add and a `futex_wake` **syscall** on one node-wide word
(`progress-host.c:142-149`), the wake names no connection, and every pass visits
every connection (`progress.c:575-584`): O(N) per wake, and L-way contention on
one line.

**C1. A dirty-connection set.** In the connection area, a bitmap over connection
slots, 64 per word, each word on its own cache line only if measurement says so. A
writer that commits into connection `s` sets bit `s` (`fetch_or`, `seq_cst`), then
wakes the lead. The lead `exchange`s each word to zero and runs `drain`/`flush`
only for set bits, plus any connection with a send in flight or a queue non-empty
(private knowledge). **Receiving still polls every wire** on the shared-memory
wire; step 3's completions replace that — say so in §3.10 and do not try to fix it
here.

**C2. Skip the syscall when the lead is awake.** A `lead_sleeping` word: the lead
stores 1 before its final check-then-`futex_wait`, 0 on waking; a writer sets its
bit, then loads `lead_sleeping` and calls `futex_wake` only if it is 1. **This is a
two-sided protocol (§3.16): both stores `seq_cst`, and it must be modelled** —
writer sets bit then loads the flag; lead stores the flag then loads the bits. The
broken variant (a `release` store on either side, under TSO) must lose a wake. If
the model or the four-preset run gives any doubt, keep the unconditional wake and
record why: a lost wake here costs a full sweep interval on the data path.

*Criteria:* with 16 logical nodes and traffic on one connection, a pass touches
O(1) connections on the send side (count it, export it); `agent.wakeups` and futex
syscalls per put drop measurably in a burst test; slice 2's echo and starvation
tests still pass; missed-wake stays zero under the load loop.

### Part D — O(P) memory, and what an RDMA address needs

(D7; roce-seam F7.) `node.c:391` allocates `local_of_rank[world_size]` in **every
PE**; `bootstrap.c:81` sizes `gtids` by world size too: ~250 MiB per node at
500 × 256, ~2 GiB at 4,096 × 256.
- `gtids` is sized by the local count. "Is rank *r* local, and which index?" is a
  binary search over the node's sorted ranks — O(log L), O(L) memory — with an O(1)
  fast path when the node's ranks are one contiguous block. It is on the put path:
  measure `shmem_p` before and after, and keep the intranode floor (13–17 ns over a
  raw store).
- The shared job map stays one copy per node. Add to it, per rank, the rank's
  **index on its own node** (16 bits suffice at 256 PEs per node; assert). An RDMA
  backend cannot form a remote address without it. Shared O(P) per node is
  accepted for now — 6 bytes × 10⁶ ≈ 6 MiB per node; note a range-compressed map as
  the known next step in §6.2, and the O(N·P) broadcast that fills it
  (algorithmic-scaling 9).
- Sweep for any other allocation or loop sized by `world_size` in per-PE code
  (Sonnet; remember CLAUDE.md's rule: prove the grep can find `local_of_rank`
  before believing an empty result). Report each; fix the cheap ones.

### Part E — the intranode release

(algorithmic-scaling 4.) `osoe_epoch_wake_all` is one `futex_wake(INT_MAX)`: O(L)
work on the lead's **progress thread**, which is the node's network. Replace it
with a chained wake: the releaser wakes 2; every waiter that wakes and finds its
target reached wakes 2 more before returning. Depth O(log L), and the progress
thread does O(1).
- Argue in §5.3 why no waiter is stranded: every waiter on a `released` word waits
  for the same value (§5.4's bounded skew); a waiter that arrives late loads the
  word before sleeping and never sleeps; a woken waiter always passes the wake on;
  the sweep bounds any residue and slice 2's counter makes residue *visible*.
- Model it (small: 1 releaser, 4–5 waiters, wake-2). Broken variant: a waiter that
  returns without passing the wake on must strand someone.
- *Criteria:* missed-wake zero across the suite and under the load loop; barrier
  latency at L = 2…32 on one node not worse than before.
- **Do not** shard the arrival counter (§5.3's two-level tree). The Pi has four
  cores and there is no measurement at large L; record in §5.3 that nothing in the
  control-segment layout prevents it, and what measurement would trigger it.

### Part F — seeing it

(D10 second half; diagnosability D-5.) A per-connection metric scope in the §3.14
table (diagnosability D-3 says it needs a fourth scope in `segment.c:169-192`):
segments and bytes each way, header-only segments, queue depths and high-water,
last acked / wanted-through. Per team per node: epoch in progress, own-arrived,
children arrived, **the lowest child index that has not arrived**. With the tree
that last metric means a hung barrier is found by walking *down* from the root in
O(log N) `osoe-stat` visits instead of visiting every node; add the hostname to
the node header so the walk can name where to go next, and a one-line-per-node
`osoe-stat --summary`. *Criterion:* repeat diagnosability's experiment 1 (one PE
never reaches the barrier) on 8 logical nodes and find the node from `osoe-stat`
output alone, following child pointers; put the transcript in the commit message.

### Part G — force-remote tells the truth

(roce-seam F4; premises 3; conformance F2.) §8.1, `job.h:79-84` and commit
`56dedff` say the flag leaves the route alone; in fact one predicate
(`osoe_is_local`) answers both "which path carries this" and "may I hand out a
pointer", so a put to a node-mate under `OSOE_FORCE_REMOTE=1` aborts with "no
connection toward node". Split the predicate in two — route, and direct access —
which is also what makes §10's "dispatch per (pe, operation class)" true rather
than claimed. Under the flag, same-node traffic takes the intranode path and
`shmem_ptr` returns NULL; and because spec §7 requires `shmem_ptr` to be non-null
for every member of `SHMEM_TEAM_SHARED`, the shared team under the flag is the
singleton {self}. Correct §4.8's and `de540da`'s claim in the doc. Add the missing
differential pole: an all-AM run with more than one PE per node is impossible
(no self-connection), so say in §8.1 exactly what the differential test does and
does not compare (premises 11).

## Out of scope

Sockets, io_uring, step 3's receive path. Step 4. New collectives, data-carrying
messages, team split, contexts, nonblocking anything. An RDMA backend. Sharding the
arrival counter. Lazy connection establishment — but **do not make it harder**:
nothing added here may touch every connection periodically.

## Rules

The Rules and Testing sections of `phase1-step2-slice1-prompt.md`, as amended by
slice 2's brief, apply. In addition:

- **Complexity is a correctness criterion in this slice.** For every structure or
  loop you add, state in the commit message its cost in N (nodes), L (PEs per
  node), P and T (teams), in time and memory, per PE and per node. Anything O(P)
  per PE, or O(N) on a per-operation path, is a defect.
- Models are required for B1, C2 and E, each with a variant that fails *first* on
  the property it targets, and with the trapped-state check on.
- Layout bumps: at most one per part; say what an older `osoe-stat` does.

## Working method

- State a short plan, then proceed. **Do not stop for approval. Stop only on a
  surprise**, saying what you expected, what happened, and the options:
  - slice 2 is not merged, or its pieces this slice depends on are missing;
  - the interface of part A cannot be made to carry the existing barrier without a
    behaviour change, or needs `transport.h`'s contract to change beyond A5;
  - the tree needs a wire-format change;
  - a model contradicts a pinned decision;
  - part C2's protocol cannot be made sound — then keep the unconditional wake;
  - part D slows the intranode put floor measurably.
- Branch `phase1-step2-slice3`. Small commits, warning-free, all four presets green
  in the VM, the load loop (30× in isolation under 4× oversubscription) on every
  test added or changed, messages that say what was verified and how, the design
  doc changed in the commit that makes it true.
- Short progress notes to the user at the end of each part.
- When done: summarize commits, verification, design-doc changes, measurements
  with their caveats, deferrals, open questions; add a row to
  `docs/prompts/README.md`; and add to `docs/design/successor-api-stage-setting.md`
  §2 any lesson this slice paid for (its §8 asks for exactly that). **Ask before
  merging into `main`.**
