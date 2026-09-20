# After osoe: setting the stage for a smaller SHMEM-like API

**Status:** stage-setting, with a strawman to argue against. Not a decision
record; nothing here binds osoe. Written 2026-09-20 from the second adversarial
review (`docs/reviews/phase1-step2-slice1-review.md`), whose investigators were
each asked to note where the OpenSHMEM API itself forced cost, complexity or an
unscalable structure on the implementation.

**Prior art in this repository.** `docs/reviews/core-api.md` surveyed six
production transports and proposed a minimal *core* underneath OpenSHMEM 1.4–1.6;
osoe's §4.10 boundary (`shmem/src/transport.h`) is an independent arrival at
nearly the same shape. This document asks the next question: if that core is
right, what is the smallest *application-facing* API worth putting on top of it,
once the obligation to be OpenSHMEM is dropped or loosened?

The placeholder prefix `oc_` follows `core-api.md`. It is not a name.

---

## 1. Goals

1. **The essential tools only.** One-sided put and get, a small set of remote
   atomics, completion, waiting on a remote-written word, collectives over groups,
   symmetric memory. One way to do each thing.
2. **Nothing that scales with PE count where it can scale with node count, and
   nothing linear where logarithmic is available** — in time and in memory, within
   a node and between nodes. Usable at ~10⁶ PEs.
3. **Never block a fabric feature.** Informed by which atomics hardware actually
   has (§3), and by keeping offloadable operations — collectives above all —
   *behind* the seam rather than above it.
4. **One grouping concept.** No teams-versus-contexts.
5. **An API for AI-assisted development**, not only for human intuition (§5).

## 2. What building osoe taught, as far as it has been built

Each lesson names the review finding it came from.

| # | Lesson | Evidence |
|---|---|---|
| L1 | **Completion without a handle is the root of the acknowledgement machinery.** `shmem_quiet` must cover everything a context ever sent, which forces the dirty set, the cumulative ack, and the request-an-ack side channel — exactly where osoe's two masked defects live. | Review B1; correctness C1, C2 |
| L2 | **Completion with no "not yet" and no error forces an invented deadline.** osoe bounded `quiet` by its bootstrap timeout and aborts healthy jobs on a slow link. | Review D3 |
| L3 | **Waiting on an arbitrary symmetric word** forces a two-sided protocol against every write path, including `memcpy` puts that cannot take part. The spec patched it in 1.5 ("update with an AMO"); implementations carry two counters to tell bugs from accepted losses. | Atomics report §6; litmus: 58 lost wakes per 10⁶ on the put path |
| L4 | **Collectives above the transport seam are the largest single RoCE loss**, and were nearly locked in by a decision whose reasons the code contradicted. What actually crosses is tiny: `arrive(group, epoch)` down, `released(group, epoch)` up. | Review D5 |
| L5 | **Equality-compared epoch counters are what nonblocking collectives break.** A blocking-only collective design is a dead end that has to be redone. | Design §5.4; deferral table |
| L6 | **PE-addressed everything invites O(P) tables.** osoe's one PE-dimensioned private array costs ~2 GiB per node at 10⁶ PEs. Membership must be a *rule*, never a table. | Review D7 |
| L7 | **A job-wide team count sizing per-node state** makes many small teams infeasible; team identity was undesigned when arrays began to be indexed by it. | Algorithmic-scaling 7 |
| L8 | **`finalize` is both a barrier and the teardown**, which creates "the root leaves first while still owing the release", and `atexit`-driven finalize turns a fatal error into a hang. | Protocols report; review D9 |
| L9 | **The type cross product is 89 % of the symbol table** (467 of 526) and none of the semantics. | Conformance |
| L10 | **The hardware atomic set is much smaller than the API's**, and 32-bit atomics cannot even be emulated on the containing word without corrupting a legal neighbour store. | RoCE seam F2, §6 |
| L11 | **"Undefined behaviour" is a poor contract for a library whose failures are distributed hangs.** osoe chose loud aborts and no abort landed on spec-defined behaviour; pre-init calls, left undefined, segfault. | Conformance |
| L12 | **Diagnosability had to be built as a product** (self-describing segment, blocked record, heartbeat) and was blind exactly where the API gave it nothing to name: below the seam. | Diagnosability D-2, D-3 |
| L13 | **A logical node map made one host a cluster** and was the most productive test device in the project. It belongs in the API's definition of a job, not in a test harness. | Design §8.1 |
| L14 | **Per-operation ordering (`FENCED`), counters rather than per-operation handles, and a "more follows" hint** survived every RDMA feature thrown at them. Keep them. | RoCE seam §7 |
| L16 | **The specification is silent on how a job dies** — signals, `exit()` without finalize, cancelled jobs, leaked shared objects — and that silence is where an operator meets the library first. A cancelled osoe job leaks all six of its shm objects; an un-finalized `exit(0)` hangs the rest. | Review D13, D14 |
| L15 | **Symmetric statics are what legacy code actually uses**: only 7 of the spec's 53 examples run without them. This prices any compatibility promise. | Conformance |

## 3. What the hardware offers

The author's hypothesis was that PCIe atomics match RoCE atomics. It is half
right, and the right half is the interesting one (full table with sources:
`docs/reviews/phase1-step2-slice1-roce-seam.md` §6).

| Fabric | Operations | Widths | CPU-coherent? |
|---|---|---|---|
| PCIe AtomicOp TLPs (2.1+) | FetchAdd, Swap, CAS | 32, 64 (CAS also 128) | Yes, completed at the root port |
| InfiniBand / RoCE (IBTA) | FetchAdd, CmpSwap | **64 only** | HCA-scope or global, by capability |
| mlx5 extended (vendor) | ADD, AND, OR, XOR, SWAP, CSWAP via masked ops | 32, 64 | Device scope only |
| mlx5 PCI-atomic mode (vendor) | fetch_add, swap, compare_swap — *executed as PCIe AtomicOps* | 4, 8, 16 bytes | **Yes**; prevalence unknown |
| Ultra Ethernet | fetching and non-fetching; CAS in the HPC profile only | datatype-sized | vendor-optional |
| CXL.mem / .cache | none — the CPU's own instruction over coherent memory | — | — |

**The intersection is {fetch-and-add, compare-and-swap} on 64-bit words, always
fetching.** Add Swap and you lose stock IBTA. Add 32-bit and you lose it again.
Everything else is either a CAS loop (≥ 2 round trips under contention) or a
target-CPU active message. An API built on the intersection has one atomic width
and two atomic operations, and states the rest as *derived, with a cost*.

Labels as in the review: the PCIe and IBTA rows are *documented*/*observed in
source*; the UET and CXL rows were carried from round one and not re-read.

## 4. Strawman

Offered to be attacked. Thirty-odd routines; C11; every handle explicit.

### 4.1 Two orthogonal objects instead of teams × contexts

OpenSHMEM's context is created *from* a team, carries ordering and completion,
and also scopes atomicity; its team carries membership and also owns contexts.
The strawman splits along the real seam:

- A **group** answers *who*. Membership is a rule — world, node, strided,
  Cartesian slice, or an explicit sorted range list — so a group costs O(1)
  memory per member regardless of size, and translation is arithmetic. A group's
  identity is derived deterministically from its parent's identity, the rule and
  a per-parent sequence number, so **creating a group needs no communication**
  and no job-wide count bounds anything (L6, L7). Collective *resources* are
  bound lazily at first use.
- A **stream** answers *in what order, and complete when*. It is purely local,
  never collective, and unrelated to any group. Operations on one stream to one
  rank may be fenced; completion is asked of a stream.

```c
oc_group oc_group_world(oc_job);          oc_group oc_group_node(oc_job);
oc_group oc_group_strided(oc_group parent, int start, int stride, int count);
oc_group oc_group_slice(oc_group parent, int dimensions, const int *shape, int keep_axis);
oc_group oc_group_ranges(oc_group parent, const struct oc_range *ranges, int count);
int  oc_group_rank(oc_group);   int oc_group_size(oc_group);
int  oc_group_translate(oc_group from, int rank, oc_group to);   /* O(1) or O(log ranges) */
void oc_group_release(oc_group);                                  /* local */

oc_stream oc_stream_create(oc_job, unsigned flags);   /* OC_STREAM_PRIVATE, OC_STREAM_NO_STORES */
void      oc_stream_release(oc_stream);
```

### 4.2 Memory: regions with declared purpose, references not raw addresses

```c
oc_region oc_region_create(oc_group, size_t bytes, unsigned purpose);  /* collective, once */
    /* purpose: OC_BULK | OC_ATOMIC | OC_WAITABLE */
void     *oc_allocate(oc_region, size_t bytes, size_t alignment);      /* local, deterministic */
void      oc_free(oc_region, void *);
struct oc_reference { uint32_t region; uint64_t offset; };             /* what the wire carries */
struct oc_reference oc_reference_to(oc_region, const void *local_address);
void     *oc_direct(struct oc_reference, int world_rank);              /* pointer, or NULL */
```

Allocation is **local and deterministic**, symmetric by contract, with no barrier
(osoe's allocator already has this property; the spec's barrier adds an O(N)
collective to every `malloc`). A debug build carries an allocation digest on the
next collective and names the first divergent call. Purpose is declared at region
creation, which is what an RDMA backend needs to register atomics-capable memory
separately (osoe's `set_range`, UCX's atomic key) — stated once, up front,
instead of inferred per allocation. `oc_direct` keeps osoe's contract: non-NULL
means *stores through this pointer are ordered against the library's own writes*,
not merely "is local". No symmetric statics (L15 prices this).

### 4.3 Data movement and completion

```c
void oc_put(oc_stream, struct oc_reference dst, int rank, const void *src, size_t len, unsigned flags);
void oc_get(oc_stream, void *dst, struct oc_reference src, int rank, size_t len, unsigned flags);
    /* flags: OC_FENCED (after everything earlier on this stream to this rank), OC_MORE (hint) */

oc_mark   oc_stream_mark(oc_stream);                       /* a sequence point: a counter value */
oc_status oc_wait(oc_stream, oc_mark, const struct oc_deadline *);
    /* OC_DONE | OC_NOT_YET | OC_PEER_FAILED; deadline NULL = block, zero = poll */
```

Everything is nonblocking; a blocking put is `oc_put` + `oc_wait`. A **mark** is a
counter value, so this is still counters-not-handles (L14) — but the completion
request now *carries its own sequence number by construction*, which is the fix
shape for osoe's acknowledgement defects (L1), and `OC_NOT_YET` gives the caller
the deadline policy the library otherwise has to invent (L2).

### 4.4 Atomics: the hardware intersection, and the rest priced

```c
uint64_t oc_atomic_fetch_add    (oc_stream, struct oc_reference, int rank, uint64_t addend);
uint64_t oc_atomic_compare_swap (oc_stream, struct oc_reference, int rank, uint64_t expected, uint64_t desired);
uint64_t oc_atomic_read         (oc_stream, struct oc_reference, int rank);
void     oc_atomic_add          (oc_stream, struct oc_reference, int rank, uint64_t addend);  /* reply discarded */

/* Derived: may take more than one round trip, or run on the target CPU. */
uint64_t oc_atomic_swap   (...);   uint64_t oc_atomic_fetch_and(...);
uint64_t oc_atomic_fetch_or(...);  uint64_t oc_atomic_fetch_xor(...);
```

64-bit only, in `OC_ATOMIC` regions only, 8-byte aligned by construction. The
header says which tier a routine is in, so neither a person nor a model has to
guess which calls are one NIC operation.

### 4.5 Waiting: a distinct object whose only writers notify

```c
oc_status oc_await(struct oc_reference own_word, enum oc_compare, uint64_t value,
                   const struct oc_deadline *);
```

Legal only on a word in an `OC_WAITABLE` region, which only atomics (and a put
carrying `OC_NOTIFY`) may write. That removes L3's two-sided protocol against
`memcpy`, makes "notification: exact" keepable on a NIC (osoe's RoCE seam F8),
and tells an RDMA backend exactly which words need event bridging.

### 4.6 Collectives: nonblocking first, identified by (group, sequence), behind the seam

```c
oc_mark oc_barrier   (oc_group);
oc_mark oc_broadcast (oc_group, int root, struct oc_reference buffer, size_t len);
oc_mark oc_reduce    (oc_group, struct oc_reference dst, struct oc_reference src,
                      size_t count, enum oc_type, enum oc_operation);
oc_mark oc_gather    (oc_group, ...);   oc_mark oc_exchange (oc_group, ...);   /* alltoall */
oc_mark oc_scan      (oc_group, ...);
oc_status oc_group_wait(oc_group, oc_mark, const struct oc_deadline *);
```

A collective is named by its group and its sequence number in that group, so
nothing depends on bounded skew (L5). The implementation contract — not the
application API — requires the internode phase to sit behind a bound interface
with a generic tree over put/atomics as the default implementation, so SHARP,
multicast or triggered operations can replace it (L4). Complexity is part of the
contract: O(log N) depth internode, O(log L) depth intranode, O(1) state per
group per node.

### 4.7 Lifecycle, failure, and introspection

```c
oc_job    oc_open(const struct oc_options *);    /* no global state; no atexit */
oc_status oc_quiesce(oc_job, const struct oc_deadline *);   /* collective: everything complete */
void      oc_close(oc_job);                                  /* local: release resources */
_Noreturn void oc_abort(oc_job, int status, const char *reason);
int oc_rank(oc_job);  int oc_rank_count(oc_job);  int oc_node(oc_job);  int oc_node_count(oc_job);
```

`quiesce` and `close` are separate (L8). Misuse is a **named fatal in every
build**, never undefined (L11). Job shape — which ranks share a node — is part of
`oc_options`, so one host is a cluster by definition and every program is
testable on a laptop (L13). Every blocking call publishes what it waits on and on
whom; the introspection segment's format is part of the specification (L12).
Profiling is a registered callback table, not weak symbols and a doubled ABI.

### 4.8 Deliberately absent

Global locks; contexts made from teams; the typed routine cross product (a
`_Generic` convenience header may exist, the ABI is sized); 32-bit and
floating-point atomics; symmetric statics; `shmem_ptr` on arbitrary memory;
deprecated names; reserved environment-variable syntax; implicit finalize.

## 5. What "an API for AI-assisted development" might mean

A hypothesis to test, not a finding. The osoe experience — a library and its
tests written with a model's help, then reviewed by models — suggests an API is
easier for that process to use *correctly* when:

1. **Contracts are local.** No hidden global state, so a call site can be judged
   from the handles in scope. (OpenSHMEM's default context and implicit world
   team make every call depend on distant state.)
2. **There is one way to do each thing**, and names are regular:
   `oc_<object>_<verb>`, arguments always stream–target–rank–source–length.
   Redundant spellings are where generated code drifts.
3. **Misuse fails loudly, identically, and in machine-readable form** — a stable
   error identifier, the rank, the routine, the offending value. A model
   iterating on a test needs the failure to say what was wrong; "undefined" gives
   it a hang.
4. **Cost is declared.** Each routine states its class — local / one-sided /
   collective — its round trips, and its complexity in N and L, in a structured
   header comment. Performance review then becomes checkable rather than folklore.
5. **The semantics are small enough to ship as an executable model.** osoe's
   model checker found real bugs; a reference model of ordering, completion and
   waiting, published with the specification, lets tools check a program's use of
   the API, not just the library's implementation of it.
6. **Every program runs deterministically on one host** (L13), with a seedable
   scheduler in the reference implementation, so a generated test is a
   reproducible test.
7. **Blocking always has a deadline form and a reason** (L2, L12), so an agent
   running a test suite gets a diagnosis rather than a timeout.

## 6. The compatibility stance — undecided, and the options

| Stance | What it means | For | Against |
|---|---|---|---|
| **A. Clean break** | New names and semantics; OpenSHMEM is inspiration only | Every lesson above can be taken whole; smallest surface; best for §5 | No existing code, benchmarks or users; must earn adoption alone; loses the corpus as a test suite |
| **B. Strict subset** | Every routine is an existing OpenSHMEM routine; codes port by deletion | The corpus and every SHMEM programmer come free; conformance is checkable against a real spec | L1–L3 and L8 are *semantic* and cannot be fixed by deletion: quiet keeps no handle, waits stay on arbitrary words, contexts still come from teams. Goal 4 is unreachable. L15: the subset people need includes statics |
| **C. Core + shim** | The strawman is the core; OpenSHMEM 1.6 is implemented on top as proof of sufficiency | This is what §4.10 and `core-api.md` already converge on; the shim *is* the test that the core is complete; legacy code keeps working while new code targets the core | Two surfaces to specify and test; the shim may drag core design toward SHMEM's shapes; anything the shim cannot express efficiently (L1's handle-less quiet over marks: easy; statics: a toolchain) becomes core pressure |

A decision is not needed until osoe has collectives, teams and nonblocking
operations built — those are where the remaining lessons will come from. What
*would* inform it cheaply: grep the corpus for what B would have to keep (statics,
`shmem_ptr`, locks, typed names) and see whether B is a subset anyone could use.

## 7. Questions this document cannot answer yet

1. Can group identity really be communication-free when two members split the
   same parent in different orders? (OpenSHMEM makes split collective over the
   parent for this reason. A per-parent sequence number assumes ordered calls.)
2. Is local deterministic allocation enough for codes whose allocation sizes
   depend on rank-local data? If not: a collective `oc_region_create` per such
   allocation, or an explicitly asymmetric region kind?
3. Does a mark per stream suffice for multi-rail backends, or must marks be per
   (stream, rank)?
4. Is 64-bit-only atomics acceptable to real codes? The corpus can answer: count
   `int`/`int32_t` AMO call sites and whether any depend on width.
5. Where do GPUs sit? `transport.h` is host-side and blocking; the strawman
   inherits that and should say so or fix it.
6. What is the smallest collective set? Barrier, broadcast, reduce, gather,
   exchange, scan is a guess; `bale` and `convey` suggest exchange dominates.
7. Should failure be survivable? `OC_PEER_FAILED` exists in the strawman as a
   status, but osoe's error model is "the resource manager owns the job". The
   two must be reconciled before the status means anything.

## 8. What to record while finishing osoe

So that the next revision of this document rests on evidence rather than memory:
each time a spec rule costs a structure, a barrier, a table or a test that the
hardware did not ask for, add a row to §2 with the commit that paid for it.
Collectives, team split and nonblocking RMA are the three pieces of work most
likely to add rows.
