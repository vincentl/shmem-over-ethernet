# Implement phase 1, step 4, first slice: a job that dies, dies everywhere

You are starting **§7.1 step 4** of **osoe** (OpenSHMEM Over Ethernet),
"Robustness": pidfd liveness, abort propagation, `shmem_global_exit`,
keepalive; Slurm environment fixtures and the heap-base-shift test mode. Step 3
is merged: every node pair has a TCP connection driven by the lead's io_uring,
the waits sleep on per-class words, the library is pinned to its floor, and
tier 3 runs the multi-node tests over delayed, lossy and small-MTU links.

This slice takes the first half of step 4 — **the three ways a job learns that
it is dying, and the one way it then ends**. The second half, the Slurm
fixtures and the heap-base shift, is the next slice's.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node**, with no serial
  bottleneck where a cheap alternative exists.
- **A path to RoCE at ~10⁶ PEs (4,096 nodes × 256) that no interface blocks.**
- The Ethernet path is graded at *hundreds of nodes × 256 PPN*; the seam and the
  inherited data structures are graded at 4,096 × 256.

Machine setup: "Setting up on a new machine" in `phase1-step2-slice1-prompt.md`.
Meaning of phase/step/slice: `docs/prompts/README.md`.

*Written by Claude on 2026-09-23 while the author was away, under his mandate to
continue past slice boundaries (`docs/decisions/2026-09-23-autonomous.md`). It
follows §3.15 and §7.1 as written; where it chooses between readings, it says
so, and those choices are for the author to confirm.*

## Read first

1. `CLAUDE.md`.
2. Design §3.15 in full: the error model, "no wait gives up because time has
   passed", the abort word, detection and propagation as the design lays them
   out (`shmem_global_exit`'s three steps, a pidfd per local PE on the lead's
   ring, TCP keepalive, `KillOnBadExit`), and the development watchdog and what
   it deliberately does not do. §3.12 (agents, and why every PE has one), §6.4
   (the lifecycle, "no exit path runs a collective the application did not
   call", the goodbye and the end of stream), §5.3 (the world team's tree),
   §4.10 (the boundary: none of this may put a transport type above it).
3. Spec §9.1.6, `shmem_global_exit`: non-collective, terminates every PE
   "regardless of their current execution state", flushes I/O as normal
   termination does, and if several PEs call it the status is one of theirs.
4. The code the slice builds on (a map is in the decisions log's step-4
   section): the abort word in `node.h` and `osoe_node_declare_abort`; the
   sweep that reads it in every build (`watchdog.c`); `fatal.c` and why it ends
   in `_exit`; the agent loop in `agent.c`; the AM kinds in `am.h` and their
   dispatch in `progress.c`; `osoe_collective_tree`; the node-pair sockets in
   `wire.c` (only `TCP_NODELAY` is set today); the pid each PE publishes in its
   slot; `init.c`'s atexit handler for a PE that leaves without finalizing.
5. `docs/reviews/archive/phase1-step2-slice1-conformance.md` F1 (exit and atexit),
   and the second review's sequencing section on step 4.
6. `tools/modelcheck/close_model.py`: the goodbye protocol an abort must not
   break.

## What exists, and what does not

- **Within a node the abort already stops every wait**: a fatal declares the
  node's abort word, and every library wait reads it at its next sweep and
  leaves through `_exit`. What does not stop: a PE computing outside the
  library, which never sweeps.
- **Across nodes nothing carries it but a dying process's end of stream.** A
  remote node learns of a death only as a connection that ends without a
  goodbye: a crashed lead's, or one whose own library wait found the node's
  abort word and took its process down. A lead computing outside the library
  does neither — its agent's wait ends every sweep interval but never reads
  the abort word — and then a remote node learns nothing, ever, since no wait
  gives up because time passed. And an end of stream carries no status.
- **`shmem_global_exit` does not exist**, not even as a declaration.
- **Liveness outside the development watchdog does not exist.** A release build
  whose PE is killed waits for the launcher.
- **Keepalive is off**: a peer host that vanishes without a FIN leaves its
  connections open until the kernel's retransmissions give up, if data is in
  flight, and forever if not.

## Order of work

### Part A — the abort crosses nodes

An abort message carries the origin rank, the status, and the disposition (a
fatal, or a global exit with its status). Two ways to carry it are open, and
the model decides between them:

- **Along the world team's tree**: a lead sends it to its parent and children,
  never back to the neighbour it came from, once per node, the node's abort
  word set first being the dedup. O(k) messages per node and O(log N) hops.
- **As an abortive goodbye**: the last segment a dying lead sends on every open
  connection before its stream ends, in place of §6.4's goodbye. O(open
  connections) per node — the eager mesh's N, which is also what a normal
  goodbye already costs — and one hop.

The reason the second is on the table: the mesh already carries a death, in
one hop, as an end of stream without a goodbye, which a peer takes for a crash
and ends the job with a failure status. A tree-forwarded abort races those
ends of stream: a peer that reads a dying node's end of stream before the
tree's message reaches it exits with the wrong status, which §9.1.6 forbids
for a global exit. An abortive goodbye is read before the end of stream it
precedes, by construction. Say which the model chose, what the other lost,
and what each costs at 4,096 nodes; with on-demand connections (§6.2) the
second becomes O(connections a node has open) and still reaches every node
through the tree's own connections.

**Model it first** (`tools/modelcheck`, extending `close_model.py`'s channels,
trapped-state check on): three nodes with an interior one, the final barrier
and §6.4's close, aborts that start at one node or several at once, and a
node that dies without a word (its kernel's end of stream). Properties: every
node ends; no node sends on a connection twice what the protocol sends once;
and every node that ends because of a global exit ends with a status some
caller gave. Broken variants: the tree without the race's fix; exit before
forwarding; an abort dropped while closing.

### Part B — every PE ends when its node aborts, whatever it is doing

Each PE's agent checks the node's abort word whenever it wakes and at each
sweep, and the lead wakes every local agent when the word is set (a futex
wake on each agent's word: O(L) once per job). An agent that finds it set ends
its process:

- **a global exit**: `exit(status)`, so standard I/O is flushed as §9.1.6
  requires. The library's own atexit handlers must then do nothing, since one
  is a barrier (`start_pes`'s finalize) and the other declares an abort that
  has already happened. `exit` from a thread that is not main can block on a
  stdio lock the main thread holds; fall back to `_exit(status)` after a bounded
  grace, and say how long and why that bound is not a wait that "gives up
  because time passed" (it ends a process that is already ending).
- **a fatal**: `_exit(EXIT_FAILURE)`, as a fatal already does.

`shmem_global_exit(status)` itself: declares the node's abort as a global exit
with the status (first writer wins; the spec allows any caller's status), wakes
the lead, waits until the lead has forwarded the abort — or a process that exits
first takes the only agent that could have sent it with it — and then calls
`exit(status)`. Declared in `shmem.h`, `pshmem.h` (the `pshmem_` strong /
`shmem_` weak pair), `shmem.exports`, and `_Noreturn` where C11 allows.

**Model the exit ordering** with Part A's model or beside it: the caller, its
lead, a remote lead, and a computing PE on each node; the property is that
every process ends and every one that ends has had its abort forwarded.

### Part C — liveness without the watchdog

The lead's ring holds a **pidfd for every local PE** (`pidfd_open` on the pid
each PE publishes, polled on the ring as §3.15 says); a completion for a PE that
has not finalized declares the node's abort as a fatal naming the PE, which
Parts A and B carry to every PE of the job. A non-lead PE checks the **lead's**
pid at each sweep (`kill(pid, 0)`, one syscall per sweep of a sleeping wait,
nothing on a wait that does not sleep). Say what the development watchdog then
still adds, and whether it should stay.

### Part D — keepalive

`SO_KEEPALIVE` with `TCP_KEEPIDLE`, `TCP_KEEPINTVL` and `TCP_KEEPCNT` on every
node-pair connection. Keepalive asks whether the peer's *kernel* is there, which
contention does not stop: a stopped node's kernel still answers, so a stopped
node is waited for, as the no-fatal-timeouts decision requires. Choose the
values, make them overridable for tests, and do not set `TCP_USER_TIMEOUT`
unless you can say why it is not a fatal timeout. Test it at tier 3: a node
whose link drops everything is found dead within keepalive's bound, and a node
that is merely stopped is not.

### Part E — what the notes must now say

§3.15 as built; §7.1 step 4 says what is done and what the next slice has;
§3.14 if the abort's origin and disposition are published; §8.2's
"tests the review found missing" for whatever this slice closes.

## Rules

The Rules and Testing sections of `phase1-step2-slice1-prompt.md`, as amended by
the later slices, apply. In addition:

- **Complexity is a correctness criterion.** State in every commit message the
  cost in N, L, P and T, in time and memory, per PE and per node.
- **Model any new protocol before writing it**, with a broken variant that
  fails the property it targets and the trapped-state check on.
- **Memory orders come from the C11 model and are minimal** — §3.16's table,
  and a row for any new shape.
- **No wait gives up because time passed.** An abort ends a job because
  something died or someone asked; nothing in this slice may end a slow one.
- **Run the load loop** on every test added or changed, never while editing the
  tree; and tier 3 for anything that touches the sockets.
- Layout bumps: at most one per part; say what an older `osoe-stat` does.

## Working method

- State a short plan, then proceed. Stop only on a surprise, saying what you
  expected, what happened, and the options:
  - a model contradicts §3.15, or shows the goodbye protocol cannot absorb an
    abort without a redesign;
  - `exit` from an agent thread cannot flush standard I/O reliably on the
    development kernel, which would make §9.1.6's "flushes I/O" unreachable
    as designed;
  - an abort needs a change above the §4.10 boundary beyond declaring
    `shmem_global_exit`;
  - keepalive cannot tell a stopped node from a vanished one.
- Branch `phase1-step4-slice1`. Small commits, warning-free, all four presets
  green in the VM, the load loop on every test added or changed, messages that
  say what was verified and how, the design doc changed in the commit that
  makes it true.
- When done: summarize commits, verification, design-doc changes,
  measurements, deferrals, open questions; add a row to `docs/prompts/README.md`;
  and add to `docs/design/successor-api-stage-setting.md` §2 any lesson this
  slice paid for.
