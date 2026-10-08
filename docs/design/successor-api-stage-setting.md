# After osoe: setting the stage for a smaller SHMEM-like API

**Status:** stage-setting, with a strawman to argue against; the proposal that
follows from it is `core-api-proposal.md` (2026-10-07), which replaces the
strawman of §4 where the two differ and keeps the lessons of §2. Not a decision
record; nothing here binds osoe. Written 2026-09-20 from the second adversarial
review (`docs/reviews/archive/phase1-step2-slice1-review.md`), whose investigators were
each asked to note where the OpenSHMEM API itself forced cost, complexity or an
unscalable structure on the implementation.

**Prior art in this repository.** `docs/reviews/archive/core-api.md` surveyed six
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
| L17 | **A seam nothing tests is a seam only by intention.** §4.10 held because no §4 file happened to use what it could see, not because anything stopped it; the AM backend's state was a field of the global state. Enforcing it was a 120-line CMake script and one test, and finding what had already crossed it took an afternoon. An API that names a boundary should ship the check with it. | Roce-seam F6; slice 3 part A |
| L18 | **One predicate answering two questions is a category error that testing finds late.** "Which path carries this operation" and "may I hand out a pointer" were one function; the flag meant to change only the second changed both, and a documented-safe configuration aborted 4 runs in 4 while three comments said it could not. The split is also what makes per-(pe, operation class) dispatch — a NIC with incoherent atomics — expressible rather than claimed. | Roce-seam F4; premises P2; slice 3 part G |
| L19 | **A diagnostic should name the next place to look, not only the state it found.** Publishing *the lowest child that has not arrived* turns "which node is stuck" from a visit to every node into a walk down the tree: three reads of eight nodes, and the answer names the host to ask next. State per node is O(1); the property is the tree's. | Diagnosability D-5; slice 3 part F |
| L20 | **"Who tells the engine there is work" belongs in the design, not in the implementation.** Without it every operation ended in a locked add on one node-wide word and a `futex_wake` syscall, and every pass swept every connection. A dirty-connection bitmap plus a claimed "the engine is asleep" flag removed both — but it is a two-sided protocol that needed a model, and its private half (what the engine knows without being told) was wrong in the first build and hung one run in eight. | Review D12; slice 3 part C |
| L21 | **A premise in a brief is still a premise.** "Every waiter on a release counter waits for the same value" was the argument for a chained wake; a member released early re-enters and waits for the *next* epoch on the same word, absorbs a wake and strands someone. The model found it before the code existed; the fix is one futex bitset. Design documents should mark which of their arguments are checked and which are assumed. | Slice 3 part E; `chained_wake_model.py` |
| L22 | **The development machine can veto the right algorithm if you let it.** On four cores a chained wake and a deeper tree both measure *worse*, because their advantage is parallel wake-up and per-hop latency that the target has and the laptop does not. Keep the algorithm, record the arithmetic for the target, and say plainly which measurement the number came from. | Slice 3 parts B4, E |
| L23 | **A value that decides who waits must be agreed before anyone waits on it.** A lead of the rendezvous tree expects children by its own node count, so one given too many waits for children that do not exist. In the order first written it did so before its parent had heard from it: every lead waited and none said why. The code came before the model, and a model of four leads found the hang in eleven states. The fix is an order (call the parent first) and a field (the count in the hello). It generalizes: in a job's description, the values that shape the waits must travel on the first message. | Step 3 slice 1 part D, `c825990`; `rendezvous_model.py` |
| L24 | **A stand-in marked "must not survive" survives unless a plan names it.** Step 2's `poll_lead` (wake the lead, sleep 20 µs) was marked for step 3 in the code and scheduled nowhere. On the ring it holds a put and quiet at 4× the floor and a stream at 3% of it; a yield in its place gives 1.1× and 34–60%. It was found because the floor is a harness nothing can tune. For an API: every completion a caller can wait for needs a wake path, and completion without a handle (L1) needs one per kind of evidence — acknowledgements, credits, buffer turnover. | Step 3 slice 1 part C, `5c95b0e`; design §8.4 |
| L25 | **A seam shaped by a stand-in fits only the stand-in.** Slice 3 gave the backend `agent_pass` and `agent_idle_ns`, so the library kept its own futex wait: the step-2 poll, renamed for the boundary. The first real backend could not use it. Its lead must wait on sockets and a word in one call, and a single-issuer ring refuses every other thread. The second shape (`agent_wait`, `agent_exit`) is the backend's own wait, which an RDMA backend's completion channel needs too. Draw a seam from the target backend's blocking point. | Step 3 slice 1 part B, `174a331`; roce-seam F5 |
| L26 | **Pin the fastest trip; report the median as a finding.** Measured in side-by-side pairs, io_uring's and a blocking socket's fastest round trips agree within about 1.1× in the median of seven pairs, under every load tried. That is the length of the path. Their median round trips diverged 3–5× beside one parallel build and agreed beside another, because a median measures where the woken thread runs. Pinned on the median, the ratio failed the load loop at 4.9× beside a parallel build; pinned on the paired fastest trips, it has not failed since. Under load the library's own round trip is its wake count, 31 µs against the harness's 4. | Step 3 slice 1 parts A and C, `7522e0b`, `5c95b0e`; design §8.4 |
| L27 | **A stop threshold measured on a variant the design forbids is not a threshold for the design.** The step-3 brief said to stop if a put and quiet cost more than 1.5× the floor once the waits had words. The 1.5× came from a scratch build that spun, which §3.11 rules out; the design's own cost is one library round trip, 2.06×, the same four wakes as a get. The spin that met 1.5× idle held a put and quiet at 4–7 ms beside 16 busy loops. A latency target stated without the wake structure it implies is a target for some other design. | Step 3 slice 2, part A (`a9f5bf2`); §3.11 |
| L28 | **A pin has to be able to fail on the defect it guards, and only breaking the code shows whether it can.** The fastest round trip is the right pin between two paths of the same length, and it could not see a 20 µs poll the size of an idle round trip. A stream at a large buffer could not see a poll in the writer's wait. What saw both was a comparison between two operations with the same wakes (a put and quiet against a get: 1.00× without the poll, 1.8× or more with it), and a stream at the buffer size where the wait happens. Timing each kind as its own block let a CPU's sleep land on one kind and not the next; interleaving the kinds within each round took the fastest-trip ratio from 1.7–4.1× to 1.4–2.2×. | Step 3 slice 2, part B (`301f10d`); §8.4 |
| L29 | **Count waiters where they already say they are.** "Waiters per wake word" cost the job nothing once each wait's published "blocked in" record named the word it sleeps on: the tool counts, and the job keeps no count. A counter beside each word would have cost two locked adds per sleep, on a line the engine writes at every acknowledgement. A wait that names what it waits on makes this diagnostic free. | Step 3 slice 2, part A (`8e8e927`); §3.14 |
| L30 | **A handoff bit does not stop barging; the queue has to be in the resource's own word.** With a flag and a wake of one, a thread arriving after the release still reaches the free credit before the woken waiter runs. Packing the free count, the next ticket and the tickets served into the credit's word lets a give serve the oldest ticket without the credit ever being free. Bounded shared resources — credits, descriptors — need admission decided in the resource itself, not in advice kept beside it. | Step 3 slice 2, part A (`b34ec94`); `credit_wait_model.py` |
| L31 | **One-host assumptions hide in the paths only tests take.** The library's mesh came from the rendezvous and found every peer by its host-list address. The tests below the library opened theirs without one, and dialled 127.0.0.1, which only a single host answers. Putting each logical node in its own network namespace found it on the first run. Nothing else in the transport failed there. | Step 3 slice 2, part C; §8.2 |
| L32 | **An abort races the death it announces, and loses unless it travels ahead of it on the same stream.** The eager mesh already carries a death in one hop, as an end of stream without a goodbye, which a peer takes for a crash. A tree-forwarded abort, logarithmic and the scaling goal's natural choice, reaches a node after a dying peer's end of stream does, and that node ends with a failure where the caller of `shmem_global_exit` gave a status. The last segment on each open connection is read first by construction. The model found it before any code existed. An API's teardown has to carry the job's status in-band with the connection's end. | Step 4 slice 1 part A; `abort_model.py` |
| L33 | **Who ends a process is a protocol, not a call to `exit`.** One thread of a process - the lead's agent, which owns the sockets - has to speak before the process ends, so no other thread may end it first. Three paths turned a global exit into a failure: the caller exiting, a watchdog that read the abort word and then found a sibling the exit had already ended, and a fatal overriding the exit's status. All three were found by reading and pinned by a model. A global exit is a request that the runtime carries out in order, not a function that exits. | Step 4 slice 1 part B; `abort_exit_model.py` |
| L34 | **A first-writer-wins record belongs in one atomic word.** The draft claimed the abort word, wrote the rank and status beside it, then published. A declarer killed between the claim and the publish would have left every reader waiting forever. Disposition, status and rank packed into one 64-bit CAS removed the window, and with it the memory-order rule the draft had needed. | Step 4 slice 1 part B |
| L35 | **Put a liveness check where something always runs.** The design put the lead's pidfds on its ring and the lead check in a library wait's sweep. A single-node job has no ring, and a computing PE never sweeps. The agent's loop, whose every wait is bounded, reaches both. The model then showed the check also makes an abort prompt, not only a crash visible. Every process of a job needs a runtime thread the application cannot starve. | Step 4 slice 1 part C; `abort_exit_model.py` |
| L36 | **A connection fails in two directions, and a failed send is only one of them.** A peer whose process ended with input unread resets the connection, and a node still sending to it finds that as a failed send - while the goodbye that explains the end is already in its kernel, ahead of the reset. The wire kept one error for both directions, so the failed send stopped the receiving and ended the node on the failure, losing the job's status whenever its ring had had no buffer free when the reset came. A duplex channel's error model needs a send error and a receive error, and a peer's end is judged by what its receive side delivers last. | Step 4, the third review's open finding; `abort_model.py`, `send_error_ends` |
| L37 | **A macro layer can make a wrong call compile to nothing.** osoe's C11 generics chose between a routine's context and plain forms by counting arguments, and left the slots no right call reaches empty: a call missing an argument became a parenthesized comma expression that evaluated its arguments and did nothing, silently without `-Wall`. All 32 generics had it. Every slot now names a function that does not exist and says what the call takes. A convenience header needs its own failure path, tested by compiling wrong calls. | Review of the nonblocking AMOs; `09576eb`, `shmem.generic-arity.*` |
| L38 | **A benchmark that stops the clock before completion measures the send buffer.** OSU's `put_bw` times its loop of puts and quiets only afterwards. On the cluster it reported 3.5 GB/s over gigabit Ethernet at 16 KiB, and 121-199 MB/s above the wire's 118 at larger sizes: osoe taking calls into about 3 MB of buffering. Completion without a handle (L1) leaves "when is it done" to the caller, and a benchmark's author got it wrong. A mark is a value a benchmark has to wait for. | Pi cluster, 2026-09-24; `402489c` |
| L39 | **Active sets written as numbers bake the job size into the call.** Four openshmem examples pass `PE_start 0, PE_size 4`: the whole job on the VM's four PEs, and on the cluster's eight a set that three of them call from PEs outside it. They passed in rehearsal and failed on the hardware. Groups built from rules the job defines - world, node, strided - cannot drift from the job's size this way. | Pi cluster, 2026-09-24; `402489c` |
| L40 | **A program's race reads like a library's lost write, and one host tells them apart.** ssca1 found one, two or three alignments from one fixed seed, run to run, which could have been osoe losing a put. The same four PEs on one logical node - no network, every put a `memcpy` - varied too, so the fault was the program's: its rank 0 wrote motifs into other PEs' shares while they were filling them. One barrier made 25 runs of 25 agree. A deterministic one-host mode (§5.6) makes that the first question, not the fourth. | `e059aea`; runbook §8 |
| L41 | **A nonblocking fetch needs somewhere to land that the library owns.** OpenSHMEM's `fetch` is any local address, so an implementation that overlaps nonblocking fetching atomics must copy each result through a landing area at the next quiet, as osoe's nonblocking gets do - and on RDMA through registered memory. osoe completes them at issue instead, a round trip each. An API that returns fetch results into slots it allocates lets the NIC write them directly. | `c56f8c6`; design §7.5 |
| L42 | **Collective resources must be per group even where the algorithm is per node.** osoe's internode data phase stages pieces in a node lead's slots, and a child writes its parent's slot without waiting for the parent to enter. That is safe within a team, since a node cannot start the next collective before this one's release, and a collision the moment two teams share a lead. Team numbers index per-node counters too, so a number must be unique job-wide, or reclaimed only after every node has let it go. | Team-creation inventory and proposal, `c84bd26` |
| L43 | **A gather built from the reduction tree moves the whole result at every level.** osoe's generic fcollect is an OR reduction over the result's bytes, so each level carries P·n bytes rather than its subtree's share: 1 MiB from each of 16 PEs took 773 ms on the cluster, about 21 MB/s of a 118 MB/s link. Allgather has to be an operation of its own behind the seam, with a pipelined or ring default, not a reduction reused. | Pi cluster, 2026-09-24; design §5.5, decision 8 |
| L44 | **The typed cross product grows with every revision of the spec.** L9 counted 467 of 526 symbols. With the typed RMA, the deprecated names and the nonblocking AMOs, osoe exports 4,857 `shmem_` entry points, 170 of them for the eight nonblocking fetching operations alone, each a wrapper over one of about six primitives. | `bc86e8f` |
| L45 | **A protocol can be proved correct and still serialize what it was built to overlap.** osoe's pipelined tree passed its model, every test and six mutations while a middle lead waited for its children with a release already in, holding every tree deeper than one level to about h/(h-1) times one piece at a time's speed, whatever the pieces in flight. Only a timing simulation found it. The fix - take whichever input is ready - needs a wait on several words at once. A collective engine needs "wait for any of these" as a primitive, and a speed claim needs a timing model beside the correctness one. | Review of step 5 slice 5; `6dfb1f6` |
| L46 | **Collective arguments that differ between members corrupt memory silently unless something carries them.** osoe's tree pieces carried a header that named a mismatch where they met. The allgather writes straight into peers' buffers and first lost that check: a larger count on one node wrote past a smaller peer's destination. A 24-byte shape on step 0's ready word restores it for nothing. Every collective should carry a signature of its arguments - group, sequence, sizes - compared before any data moves. | Review of step 5 slice 6; `43fdb77` |
| L47 | **Job-wide group numbers do not scale to grid splits.** A `split_2d` of P PEs makes 2 sqrt(P) groups at once, and osoe's never-reused, job-wide team numbers, which index per-node state, cap it: above about 2.6e5 PEs no square grid fits in 1,024 numbers. Groups need identifiers unique only where they meet - per node - or groups the library derives from rules without allocating state. | Review of step 7 slice 2; design §10 |
| L48 | **Over Ethernet a tree's fan-out is a bandwidth decision for large messages.** Pipelining osoe's tree could not take a 1 MiB broadcast on four Pi nodes below 27 ms at a fan-out of 4, whatever the pieces in flight, because the root sends three copies over one gigabit link; a fan-out of 2 reached 18 ms, the root's two copies at line rate. A collective engine needs a fan-out chosen per message size, or a bandwidth-optimal shape (chain, ring, binomial) beside the latency-optimal tree. | Pi cluster, 2026-09-26; design §10 |
| L49 | **A conformance test's limits assume a link.** SOS's `bigget` does 1,000 gets of 16 MiB and passes in a VM at memory speed; over gigabit Ethernet that is 16 GiB a PE and it times out at 120 s. A test suite for an implementation that targets slow links needs sizes and timeouts stated against the link's bandwidth, not a constant. | Pi cluster, 2026-09-26; runbook §8 |
| L15 | **Symmetric statics are what legacy code actually uses**: only 7 of the spec's 53 examples run without them. This prices any compatibility promise. | Conformance |

## 3. What the hardware offers

The author's hypothesis was that PCIe atomics match RoCE atomics. It is half
right, and the right half is the interesting one (full table with sources:
`docs/reviews/archive/phase1-step2-slice1-roce-seam.md` §6).

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
