# Phase 1, step 8, slice 2: threads, with an interim mutex on shareable contexts

**Status: written on 2026-09-26 (late evening); question C, which the
approved proposal said this slice would stop and ask, answered by the author
the same evening: C4, below.** The rest follows the proposal's decisions 1,
7-11 as approved (`docs/prompts/phase1-step8-slice1-prompt.md`;
`docs/decisions/2026-09-26-day.md`).

You are making osoe (OpenSHMEM Over Ethernet) provide
`SHMEM_THREAD_MULTIPLE`: every thread of a PE may call the library at once.
Slice 1 built contexts, serial; slice 3 will replace this slice's interim
mutex with §4.11's lock-free shared mode. What this slice builds is the
per-thread state and the concurrent claims that both need.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks** - a constraint on
  the seams, not work here (design §1).

## Read first

1. `CLAUDE.md`.
2. The spec: §9.2 (thread levels), §9.5 (contexts' options under threads),
   §9.4 and §9.10's thread-safety paragraphs (a team's collectives are not
   concurrent; different teams' may be).
3. `docs/prompts/phase1-step8-slice1-prompt.md`, "Proposed decisions" 1 and
   7-11, as approved.
4. Design §4.6 (as built: the landing zone, the blocking slot, the records and
   arena; contexts), §4.9 (the watch), §4.11 whole, §3.14 (counter blocks,
   `OSOE_MAX_THREADS`), §4.12 (locks), §5.7 (the split's borrowed word).
5. The models: `tools/modelcheck/landing_extent_model.py`,
   `copy_out_claim_model.py`, `watch_list_model.py` (§4.11 item 3).
6. The code: `metrics.c` (the counter-block claim, `_Thread_local`),
   `watch.c` (`the_watch`), `transport-am.c` (`arm`'s token counter, the
   blocking slot, `in_flight`, `complete_gets`), `progress-host.c`
   (`landing`, `landing_request`, `landed`), `lock.c` (the held list),
   `team.c` (the split's borrowed word), `collectives.c` (`collect_varying`),
   `init.c` (`OSOE_THREAD_MAX`).

## Decisions, made concrete

1. **`provided` becomes `SHMEM_THREAD_MULTIPLE`** when asked for;
   `shmem_init` still chooses the most, now MULTIPLE. Below MULTIPLE nothing
   takes a lock it did not take before.
2. **Thread blocks** (proposal 9): each of a PE's `OSOE_MAX_THREADS` counter
   blocks gains a completion slot (state, token, value, wake word, the reply's
   length) and a watch record (`rng`, `seq`, §4.11 item 3); claimed by CAS at
   the thread's first call, cached in a `_Thread_local`, released by a
   `pthread_key_create` destructor at thread exit. A PE's `active` bitmap,
   ⌈T/64⌉ words, marks records holding a published range; a writer notifies
   every record whose range overlaps. A full table ends the job naming
   `OSOE_MAX_THREADS` at every thread level (decided 2026-09-25). The
   per-PE blocking slot and `the_watch` go. Layout bump.
3. **Blocking replies land in the thread's slot**: a get's or fetching AMO's
   token names the thread block (bits the landing token has free), and the
   lead writes there; tokens come from a per-thread counter. The chunk-sized
   data area moves with it, so the slot is `chunk` bytes a thread - which is
   where the proposal's extents (7) come in:
4. **The landing zone becomes extents** (proposal 7): the zone's data is
   carved into fixed extents claimed by CAS over a bitmap, one allocator for
   blocking replies (a chunk's worth of extents) and nonblocking ones (a
   record's); a claimer that finds none copies out landed replies it can
   claim, then sleeps on the bitmap word. As `landing_extent_model.py`
   checks. This replaces slice 1's bump arena and its early completion of
   other contexts, and so the coupling design §4.11 records.
5. **Copy-out claims** (proposal 8): a nonblocking reply's record goes
   LANDED -> COPYING by CAS, the winner copies, credits its context, frees -
   `copy_out_claim_model.py`.
6. **Shareable contexts, `SHMEM_CTX_DEFAULT` among them, are serialized by a
   per-context mutex held around each operation** (proposal 1, the interim),
   a futex mutex in process memory; `PRIVATE` and `SERIALIZED` contexts, and
   every context below MULTIPLE, take none. A PRIVATE context's operations use
   its creator's thread block without the lookup.
7. **The lock list takes a mutex** (proposal 10).
8. **The split's broadcast word** moves out of the heap into spare bytes of
   the parent team's words line in the internal region (proposal 11), so a
   split never borrows from the allocator.
9. **`collect` with differing sizes: C4**, below - no heap borrow at all.

## Question C: where collect's lengths live under MULTIPLE

`collect` (differing sizes) gathers every member's length first, into
(members + 1) words it borrows from the symmetric heap for the call and gives
back. Symmetric only while every PE makes the same allocator calls in the same
order - true below MULTIPLE, where allocation is world-collective and one
thread calls at a time. Under MULTIPLE a `shmem_malloc` on one thread may
overlap a `collect` on another team on another thread (the spec serializes one
team's collectives, not different teams'), in different orders on different
PEs: the borrowed words land at different offsets, and the allocator, which
has no lock, races. A mutex fixes the race, not the order.

- **C1. A fixed lengths area per team number in the internal region, and the
  heap's borrow past it (recommended).** Each team number gets room for the
  lengths of up to 8,192 members (64 KiB; 4 MiB of address space a PE at 64
  team numbers, touched only by a collect's leads). A team that fits never
  borrows; a larger one borrows from the heap as today below MULTIPLE, and
  under MULTIPLE ends the job by name, pointing at a setting that raises the
  area. Keeps today's allgather and its speed; the limit is an
  implementation's, not an interface's.
- **C2. Places from the prefix scan, no scratch at all.** Each member's place
  is the exclusive scan of the lengths (slice 1 of step 10), the total its
  last member's inclusive one, broadcast; then the blocks go up and down the
  scan's tree into each member's dest at known places. No limit, no borrow,
  but a large collect moves the whole result through every level of a tree -
  about the old tree gather's cost, which the allgather replaced: 14.1 against
  2.0 ms for 1 MiB a PE on four nodes in the VM (design §5.8).
- **C3. A lengths area per team number sized for the whole job** (members + 1
  words a team number). No limit, but O(P) address space per team number a PE:
  512 MiB a PE at 10⁶ PEs and 64 team numbers, which the `/dev/shm` capacity
  check would refuse.

**The author's answer: C4, and why the others are gone.** The author asked
whether the lengths need to be symmetric at all; they do not. Only the way
they are exchanged forces it: the allgather writes into each peer's buffer by
its symmetric address. **C4: the lengths go through the reduction tree's
fixed per-team slots** - a sum over a vector of the team's members' words,
zero but for each node's own members' lengths - **into private memory**, and
the places are their prefix sums there; the data then goes by the allgather
as today, its `places` read locally. No limit, no scratch, and the data keeps
the allgather's speed; the lengths, 8 bytes a member, cost O(members) bytes a
tree level, pipelined in pieces like any reduction - one small reduction at
the Pi's scale. It needs `internode.h`'s reduce to accept a buffer in private
memory for this call: the generic implementation already only reads and
writes its own buffer; the contract says so, and a backend that would write a
peer's buffer directly (RDMA) stages instead for such a call. C1 and C3 put
artificial limits on teams, and C2 gave up the allgather's speed.

## Tests

Under tsan: threads issuing puts, gets, nonblocking gets, AMOs and waits at
once, on private contexts and on the default one, each thread checking its own
results; blocking gets from many threads at once across nodes (the extents);
waits by several threads of a PE on one variable (every one woken); a thread
exiting and its block reused; `OSOE_MAX_THREADS` exhausted by name; locks from
several threads. The SOS `mt_` programs and `spec-example/shmem_ctx` through
the sweep - the acceptance set - with every other SOS program still passing.
Mutations from each model's breakages. L1, L2, L4, `0,0,1,1`, `0,1,2,0,1,0`,
force-remote, tier 3.

## Stop and ask

If the extents cannot hold today's largest nonblocking get in flight without
enlarging the zone; if a thread block must grow with the job's size; if a
decision contradicts §4.11 as modeled.
