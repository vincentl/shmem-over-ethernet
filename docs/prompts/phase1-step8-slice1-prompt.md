# Phase 1, step 8: contexts and SHMEM_THREAD_MULTIPLE - a proposal, three slices

**Status: proposed on 2026-09-26, for the author.** Design §4.11 is designed and
model-checked; §10's open points were decided on 2026-09-25
(`docs/decisions/2026-09-25.md`, item 3), and the last, `pe_quiet`'s piggyback,
was modeled on 2026-09-26 (`tools/modelcheck/pe_quiet_model.py`). What is left
is the order to build it in, and the decisions §4.11 does not make: chiefly
contexts, which §7.3 only plumbed. Numbered decisions below are the ones to
approve, amend or refuse.

*Written by Claude on 2026-09-26.* The evidence: eight SOS programs - sixteen
runs on two maps - fail only because they ask for `SHMEM_THREAD_MULTIPLE`
(`spec-example/shmem_ctx`, `unit/mt_a2a`, `mt_contention`, `mt_lock_test`,
`mt_membar`, `thread_wait`, `threading`, `web`: "Init failed (requested thread
level 3"), the largest group of SOS failures once team creation lands; and
§4.11's usage study (Kokkos and hclib call `shmem_init_thread`; the threaded
GUPs and Graph500 variants issue on one context per thread, U6).

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks.**

## What exists (a survey of main at 3af41f5)

- **Thread level.** `shmem_init_thread` clamps to `SERIALIZED` (`init.c`): a
  request for `MULTIPLE` gets `SERIALIZED`, which is what SOS's programs refuse.
- **Contexts.** Plumbed, per §7.3: every RMA and AMO entry takes a handle and
  resolves it with `osoe_ctx_of` (15 calls), which knows only
  `SHMEM_CTX_DEFAULT`; `shmem_ctx_create` and `shmem_team_create_ctx` return
  nonzero, as the spec permits. The one context is a process-memory struct
  (`transport-am.c`): the §4.6 dirty set, arrays indexed by peer node, `calloc`ed,
  so O(nodes) virtual and O(communication degree) resident.
- **Nonblocking gets.** `bytes_issued`/`bytes_landed` and their wake word are in
  the PE's landing slot (node segment); records and arena bytes are claimed by
  plain increments of a process-static `in_flight`; the agent writes a reply
  into the arena and credits `bytes_landed`; the issuing thread copies to `dest`
  in quiet (`complete_gets`), or when records or arena run out.
- **Blocking gets and fetching AMOs.** One blocking slot per PE, and tokens from
  a function-static counter bumped with a plain `++` (`arm`).
- **Waits.** One watch slot per PE (`watch_rng`, `watch_seq` in the PE slot),
  cached in a process-static `the_watch`; a reentrant publish is only counted.
- **Per-thread state that exists.** `OSOE_MAX_THREADS` counter blocks per PE
  (§3.14), claimed by CAS at a thread's first counted event and cached in a
  `_Thread_local`, never released; a thread that finds the pool full counts
  into a private orphan block. (§4.11 item 5 says the thread-block cache is "as
  `base/order.c` already does"; `base/order.c` caches nothing, and the precedent
  is this one, `metrics.c`. Corrected in the design notes by slice 2.)
- **Other process statics two threads would race:** `lock.c`'s list of held
  locks (`realloc`-grown, "one thread at a time").

## Proposed decisions

1. **Three slices, each correct and testable on its own.**
   1. *Contexts*, serial mode only; `provided` stays `SERIALIZED`.
   2. *Threads*: thread blocks and what hangs off them - completion slots, the
      landing zone's extents, watch records - copy-out claims, the lock list;
      and `provided` becomes `MULTIPLE`, with every shareable context,
      `SHMEM_CTX_DEFAULT` among them, **serialized by a per-context mutex held
      around each operation, as an interim**.
   3. *§4.11's lock-free shared mode*: the generation word, two counter pairs,
      `full` and the quiet mutex (items 1 and §10's `pe_quiet`), the dirty
      bitmap with `high_seg` (item 2), replacing slice 2's interim mutex.

   §4.11 rejected a mutex around every operation for its cost - two locked
   instructions an operation, and a descheduled holder convoys every thread on
   the context - not for its correctness; as a step it lets slice 2 run the SOS
   `mt_` programs and tsan against the per-thread structures with real
   concurrency before slice 3 changes the hot path. Programs with one context
   per thread (U6) use `PRIVATE` or `SERIALIZED` contexts, which slice 2 already
   leaves lock-free. *Alternatives:* slices 2 and 3 as one; or slice 2 without
   `MULTIPLE`, its structures tested only by threads taking turns.

2. **A context is two parts.**
   - **In the node segment, a fixed-size block** from a per-PE pool of
     `OSOE_MAX_CONTEXTS`, claimed by CAS on its state word at creation and
     released at destroy: what the agent writes or another process reads - the
     nonblocking-get counters and their wake word now (moved out of the landing
     slot; the default context is block 0), and from slice 3 the generation
     word, `full` and the quiet mutex. Tens of bytes each: the pool is O(1) per
     context whatever the job's size.
   - **In process memory, the dirty set**, allocated at creation and freed at
     destroy, as the default context's is today: O(nodes) virtual, O(degree)
     resident.

   `shmem_ctx_create` returns nonzero, as spec §9.5.1 allows, when the pool is
   empty or the allocation fails.

3. **`OSOE_MAX_CONTEXTS`: default 64 per PE, between 1 and 4,096,** read at the
   first init and agreed between a node's PEs (it sizes the node segment's
   pool), as `OSOE_MAX_TEAMS` is; not between nodes, since no other node
   indexes the pool. The default context counts against it.

4. **Handles.** A handle is the address of its context's process-memory part,
   an element of a process array of `OSOE_MAX_CONTEXTS`;
   `SHMEM_CTX_DEFAULT` stays `(shmem_ctx_t)1` and means element 0.
   `osoe_ctx_of` checks the address is an element and its state is live, one
   compare and one load on the path every operation already takes, so a
   destroyed or foreign handle ends the job by name - the spec leaves both
   undefined. A destroyed element is reused by a later creation.

5. **Team contexts.** `shmem_team_create_ctx` records the team, and every
   point-to-point routine on a context translates its PE through the team
   (`world_start + world_stride` × pe, or the team's table): a branch the
   default and world contexts skip. `SHMEM_TEAM_INVALID` returns nonzero with
   `SHMEM_CTX_INVALID` (§9.5.2); `shmem_ctx_get_team` returns the team, or
   `SHMEM_TEAM_WORLD` for the default and world contexts (§9.5.4).
   `num_contexts` in a team's config is recorded and not reserved against:
   §9.5.2 lets a creation fail regardless.

6. **Options.** `SERIALIZED` and `PRIVATE` make a serial context (§4.11's
   modes); below `MULTIPLE` every context is serial. `NOSTORE` makes quiet and
   fence on the context skip the serializing fence (§9.5). A `PRIVATE`
   context records its creator; from slice 2 its operations use the creator's
   thread block without a lookup, and a destroy from another thread ends the
   job by name (§9.5.3 requires the creator).

7. **The landing zone becomes extents** (slice 2). Today it is one blocking
   slot, then records, then an arena, each allocated with one caller at a time.
   Under `MULTIPLE` both blocking and nonblocking replies need concurrent
   claims, so the zone is carved into fixed extents claimed by CAS over a
   bitmap - the decision of 2026-09-25, which `landing_extent_model.py` checks -
   and one allocator serves both: a blocking get or fetching AMO claims one
   extent for its reply; a nonblocking get claims a record's worth of extents,
   its header naming the context block the agent credits. A claimer that finds
   no free extent copies out landed replies it can claim, then sleeps on the
   bitmap word with the value it read.

8. **Copy-out claims come with slice 2**, not slice 3: even serial contexts
   share their PE's landing zone, and a claimer copying out another context's
   landed replies (7) races that context's quiet. CAS LANDED to COPYING, the
   winner copies, credits, frees - `copy_out_claim_model.py`.

9. **Thread blocks** (slice 2) are the counter blocks grown: each of a PE's
   `OSOE_MAX_THREADS` blocks gains the thread's completion slot (§4.11 item 5)
   and watch record (item 3); claimed by CAS at the thread's first library
   call, cached in a `_Thread_local`, and - new - released by a
   `pthread_key_create` destructor at thread exit, so the table holds live
   threads, not every thread that ever called. A full table ends the job naming
   `OSOE_MAX_THREADS` (decided 2026-09-25) at every thread level: a
   `SERIALIZED` program with more live calling threads than the default - the
   node's online CPU count - raises the variable. The per-PE blocking slot and
   `the_watch` go; the active bitmap (⌈T/64⌉ words per PE) is new.

10. **The lock list** (slice 2) takes a mutex: `shmem_set_lock` and
    `shmem_clear_lock` already cost a network round trip, so a mutex costs
    nothing measurable, and the list is per PE, as the spec's locks are.

11. **Collective scratch symmetric by construction, not by allocator history**
    (slice 2). `collect` borrows its lengths' room from the symmetric heap for
    a call, and a team split its broadcast word: a block taken and given back,
    at the same offset on every PE only while every PE makes the same allocator
    calls in the same order. Below `MULTIPLE` that holds - allocation is
    world-collective and one thread calls at a time. Under `MULTIPLE` a split
    on one thread may overlap a `shmem_malloc` on another, since the spec
    serializes one team's collectives, not different teams', and the threads
    of different PEs interleave the two differently: the borrowed block lands
    at different offsets, and the allocator's metadata, which has no lock,
    races. A mutex fixes the race and not the order. A split's word can move
    into spare bytes of its parent's words line in the internal region, which
    every member addresses alike; `collect`'s lengths are a word per member, too
    many to reserve per team number, and need a design of their own - slice 2
    stops and asks there. *(Found by the review of step 7's second slice,
    2026-09-26.)*

## Tests, across the slices

The SOS `mt_` programs and `spec-example/shmem_ctx` through
`build/scratch/sos/sweep.sh` (the acceptance set, slice 2), with every other SOS
program still passing. Contexts at L1, L2, L4 and on two and three nodes:
creation to exhaustion returning nonzero, destroy's implicit quiet, `NOSTORE`,
team contexts' PE translation checked by value, a destroyed handle ending the
job by name. Under tsan, threads issuing puts, gets, nonblocking gets, AMOs and
waits concurrently on private contexts (slice 2) and on the default context
(slices 2 and 3), each thread checking its own results; exhaustion of
`OSOE_MAX_THREADS`. Mutations from each model's breakages.

## Stop and ask

If a context's node-segment block must grow with the job's size; if the landing
zone's extents cannot hold today's largest nonblocking get in flight without
enlarging the zone; or if a decision above contradicts §4.11 as modeled.

## Slice 1's build brief: contexts, serial only (DRAFT - pending the author's decision)

You are building **contexts** for osoe (OpenSHMEM Over Ethernet): spec §9.5 whole
- `shmem_ctx_create`, `shmem_team_create_ctx`, `shmem_ctx_destroy`,
`shmem_ctx_get_team` - and every RMA, AMO and ordering routine on a created
context. The thread level stays at most `SHMEM_THREAD_SERIALIZED`: every context
is serial (§4.11's modes), so nothing here is concurrent; slice 2 makes it so.

### Read first

1. `CLAUDE.md`.
2. The spec, §9.5 whole, and the context forms of §9.6-§9.8, §9.12.
3. This file's "Proposed decisions" 2 to 6, as the author approved them.
4. Design §4.6 (the dirty set, nonblocking gets and their counters), §4.10
   (the transport boundary, which already takes a context), §4.11 ("Context
   modes", item 4), §7.3 (plumb but do not implement: the context form is the
   definition).
5. The code: `team.c`'s `osoe_ctx_of` and context stubs; `transport-am.c`'s
   `struct osoe_transport_context`, `mark`, `flush_dirty`,
   `osoe_transport_wait`, `remote_get_nonblocking` and `complete_gets`;
   `progress-host.c`'s `landed`; `node.h`'s landing slot and PE slot;
   `quiet.c`; `session.c` (it validates a context).

### Decisions (from the proposal, made concrete)

1. **The node segment's context blocks**: a per-PE array of
   `OSOE_MAX_CONTEXTS` blocks after the landing zones (layout 20), each a state
   word and what the agent credits: `bytes_issued`, `bytes_landed` and their
   wake word, moved out of the landing slot. Block 0 is `SHMEM_CTX_DEFAULT`'s,
   live from init to finalize.
2. **A nonblocking get's record names its block**, and `landed` credits that
   block's counters; `complete_gets` copies out the records of the context it
   is called for, and a get that finds no record free copies out any landed
   record of its PE (serial: one caller at a time).
3. **The process-memory context**: `struct osoe_transport_context` gains its
   block index, options, team and state, and lives in a process array of
   `OSOE_MAX_CONTEXTS`; handles are element addresses, `SHMEM_CTX_DEFAULT`
   element 0. The dirty set is allocated at creation, freed at destroy.
4. **Team contexts translate their PE** in the SHMEM layer, before the
   transport, through the team's mapping; the PE check is against the team's
   size (§9.5: "team-relative PE numbering").
5. **Options**: recorded; `NOSTORE` skips the serializing fence in quiet and
   fence; `PRIVATE` records the creating thread and a destroy from another ends
   the job by name.
6. **`OSOE_MAX_CONTEXTS`**, default 64, 1 to 4,096, read at the first init,
   agreed between a node's PEs (node.c, as `OSOE_MAX_TEAMS` is).

### Tests

Creation to exhaustion returning nonzero on the 65th (default counts); destroy's
implicit quiet (puts and nonblocking gets outstanding on the context, visible
after destroy returns); two contexts' quiets independent (a quiet on one does
not wait on the other's gets - pinned by counters, not time); team contexts'
PE numbering checked by value on a split team; `ctx_get_team`; `NOSTORE`
accepted; a destroyed or foreign handle and `SHMEM_CTX_INVALID` in an RMA ending
the job by name; `OSOE_MAX_CONTEXTS` disagreeing between a node's PEs refused;
L1, L2, L4, `0,0,1,1`, `0,1,2,0,1,0`, force-remote. SOS: `shmem_team_context`
must pass; `spec-example/shmem_ctx` still asks for MULTIPLE (slice 2).

### Stop and ask

If a context's node-segment block must grow with the job's size; if the
landing zone's records cannot say which context a reply belongs to without
changing the AM wire format; or if the spec's §9.5 contradicts a decision here.
