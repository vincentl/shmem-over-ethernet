# Phase 1, step 8, slice 3: lock-free issue on shareable contexts

**Status: written on 2026-09-26 (late), after slice 2 was reviewed and
verified.** It builds design §4.11 items 1, 2 and 4 as designed and
model-checked, over the structures slice 2 left; the one choice the design
leaves open, how a quiet finds its own landed pieces, is decided below
(decision 5) and recorded.

You are replacing slice 2's interim: under `SHMEM_THREAD_MULTIPLE` a
shareable context - `SHMEM_CTX_DEFAULT` and any context created without
`SHMEM_CTX_PRIVATE` or `SHMEM_CTX_SERIALIZED` - holds a mutex around every
call on it. After this slice, **issue takes no lock**; quiet, `pe_quiet` and
destroy's implicit quiet take the context's mutex, as §4.11 item 4 decides.
Serial contexts, and every context below `MULTIPLE`, do not change.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks** - a constraint on
  the seams, not work here (design §1).

## Read first

1. `CLAUDE.md`.
2. The spec: §9.12.2's Notes ("completes the operations posted prior to
   calling `shmem_quiet`"), §9.5.
3. Design §4.11 whole, especially items 1 (get completion), 2 (dirty set) and
   4 (lock-free issue, locked quiet), and "As built, slice 2"; §4.6 (the
   serial path, which stays); §3.16 (every word two threads touch is a
   protocol word).
4. The models, which already pass for this design: `get_completion_model.py`,
   `dirty_set_model.py`, `pe_quiet_model.py` (tools/modelcheck; their evidence
   copies in `docs/design/evidence/multiple/`).
5. The code: `transport-am.c` (`context_enter`/`leave`, `remote_put`,
   `remote_rmw`, `remote_get_nonblocking`, `mark`, `flush_dirty`,
   `complete_gets`, `copy_out`), `node.h` (`struct osoe_context_block`, the
   landing record, the token's bits), `progress-host.c` (`landed`).
6. `docs/decisions/2026-09-26-evening.md` items 2 and 9.

## Decisions, made concrete

1. **Modes, as §4.11 names them.** A context's mode is fixed at creation:
   shared when the thread level is `MULTIPLE` and the options have neither
   `PRIVATE` nor `SERIALIZED`, serial otherwise. Serial mode is today's code,
   unchanged, pending list and all. Every operation branches once on the mode.
2. **Get completion (item 1).** The context block gains a generation word
   `gen`, the word `full` (item 2's piggyback test), and a second pair:
   `bytes_issued[2]` and `bytes_copied[2]`, chosen by `gen & 1`; layout 24. A
   nonblocking get's issue loads `gen`, adds to `bytes_issued[g & 1]`, reloads
   `gen`, and on a change takes the add back, bumps the wake word and retries,
   counting `am.issue_retries`. **The pair travels in the token**: bit 47,
   taken from the sequence bits (16 to 46 after it), so the copier credits
   `bytes_copied[pair]`. Quiet under the mutex: bump `gen`, wait for the
   closed pair's equality while copying out its own landed pieces (decision
   5), flush and clear the dirty set, store `full` if it is a full quiet.
   `bytes_landed` stays as the lead's count, for `osoe-stat`.
3. **The dirty set (item 2)**, in process memory as slice 1 put it, so the
   node-segment block does not grow with the job: `high_segment[slot]` becomes
   atomic and is raised by a load-compare-CAS loop; a bitmap over peer slots
   replaces the touched list for shared contexts, set by `fetch_or` only when
   the bit is clear, after the raise. Quiet clears under the mutex, re-loads
   `high_segment` and the acknowledgement, and re-sets when still behind.
   Fence reads the bits and flushes without the lock. The O(nodes) process
   memory per context is slice 1's, unchanged.
4. **`pe_quiet`** clears only its nodes' bits, bumps `gen` for gets, and never
   stores `full`; a quiet piggybacks when `full - e` is positive as a wrapping
   difference, exactly as `pe_quiet_model.py` checks.
5. **How a shared quiet finds its landed pieces** (open in §4.11): it scans the
   PE's landing records for LANDED ones whose token names its context and
   claims each by slice 2's compare-and-swap. The scan is the zone's extent
   count, 256 loads at the default, against a network round trip; it adds no
   protocol word and so needs no model. A per-context bitmap of extents is the
   upgrade if measurement shows the scan. Any other thread's room-making may
   still copy the same pieces; the claim makes that safe, as today.
6. **Issue on a shared context takes no lock**: put, non-fetching and fetching
   AMOs, blocking and nonblocking gets. Blocking operations already complete
   in their thread's slot, and the landing extents are already claimed by
   compare-and-swap.
7. **Not built: the quiet with nothing outstanding** that returns without the
   mutex (§10); it is unmodeled. Nor the agent copying replies out early.

## Order of building, each part tested before the next

1. Modes and the token's pair bit; layout 24 with `gen`, `full` and the
   second pair; `osoe-stat` reads both pairs.
2. The dirty set's atomics and bitmap for shared contexts; fence and quiet
   over it, still under the interim mutex.
3. The generation protocol and decision 5's scan; still under the mutex.
4. Remove the mutex from issue. Quiet, `pe_quiet` and destroy keep it.
5. Tests, the timing, the notes.

## Tests

- `test-multiple.c` gains threads that share the default context: each puts
  its own words to a neighbour and quiets, then reads them back with a
  blocking get; issues nonblocking gets while other threads quiet; and a
  `pe_quiet` among them. Every result is checked by value, on one node, on
  block and interleaved maps, and under tsan on one PE a node.
- A pin that issue took no lock: a counter of mutex acquisitions on the
  default context, zero across a run of puts and gets from several threads.
- The mutations this slice must catch: the issuer never re-loads `gen`;
  quiet clears the bits without the re-check; the bit set before
  `high_segment` is raised; the raise by load and store; the copier crediting
  the pair the token does not name; a `pe_quiet` storing `full`.

## The timing (a speed claim needs one)

This slice claims that issue from several threads on one context scales
where the mutex did not. Measure it before and after, in the VM and on the
Pi: `osoe-bench` gains a threads line, T threads of one PE each issuing
8-byte puts and nonblocking gets on the default context for a second, for T
of 1, 2 and 4, reporting operations a second. The claim holds if 4 threads
beat 1 by more than they did under the mutex; if they do not, stop and say
so before merging.

## Stop and ask

- A model fails the design as built here, or a mutation above escapes.
- The context block would need anything that grows with the job's size.
- Decision 5's scan shows in the timing, and the fix would add a protocol
  word.
- The timing does not show the claim.
