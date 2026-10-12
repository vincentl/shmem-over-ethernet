# A core SHMEM API: the proposal

*Proposed by Claude on 2026-10-07, at the author's request, from his direction
of the same day and from what building osoe taught. It is the next step after
`successor-api-stage-setting.md` (2026-09-20), whose lessons L1 to L49 it draws
on and whose strawman it replaces where the two differ, and after the core-API
review `docs/reviews/archive/core-api.md` (2026-09-16), which surveyed six
production transports for the operations a NIC does natively: that review's
core is the floor this API stands on, not the API itself. Where this document
reasons from what hardware does, the source is that survey: osoe is a software
transport over Ethernet, built to be a working 1.6 and not from how Slingshot
or InfiniBand implement SHMEM, and it is cited here as one transport's
experience, labelled as such (the author's caution, 2026-10-07). Nothing here
binds osoe. The author will answer §11's questions, and the proposal will be revised
from his answers. The placeholder prefix `oc_` is the earlier documents' and is
not a name.*

**How to read it.** §1 is the author's direction, stated so that the rest can be
checked against it. §2 is the proposal on one page: three layers, the rule that
admits a call to each, and the core's table. §3 is what the reading corpus and
MppUtil say about how programs synchronize, the evidence the choices lean on.
§4 takes each item of the core in turn: what it is, why, what else was
considered and why it was not chosen, or was left open as a question. §5 writes
a software pipeline on the API. §6 is the thread model, §7 the OpenSHMEM 2.0
standard library built on the core, §8 OpenSHMEM 1.6 as a library on the two,
§9 the checklist for an API meant to be used with an AI assistant, §10 what is
deliberately absent. §11 is the questions and push-backs for the author,
numbered, each with a recommendation; §12 is what follows the answers.

## 1. The author's direction

From the author on 2026-09-20 (`successor-api-stage-setting.md` §1) and
2026-10-07:

1. **The essential tools only, one way to do each thing.** Puts and gets, a few
   remote atomics, completion, waiting on a word, collectives, symmetric memory.
2. **Nothing that scales with PE count where it can scale with node count**, in
   time and in memory; usable at about 10⁶ PEs.
3. **Never block a fabric feature**: the atomics are the ones hardware has, and
   collectives are offloadable because they sit behind the seam.
4. **One grouping concept.** No teams against contexts.
5. **Teams are arithmetic progressions**, never arbitrary lists of PEs:
   membership and translation are arithmetic, and no table of members exists
   anywhere. (2026-10-07)
6. **No handles.** Synchronizing on an object that captures a list of remote
   operations is of little use to programs and too hard to implement well.
   (2026-10-07; it carries L14's "counters, never per-operation handles" up
   from the transport to the application API.)
7. **A team may form a sub-team of itself**, and the team is the realm in which
   operations are ordered and completed: completion is asked of a team, not of
   a handle or a context. (2026-10-07)
8. **Fence, quiet and barrier**: decide whether the triad is essential to a core
   or too much for one. (2026-10-07)
9. **A software pipeline must be writable**, and a pipeline needs realms of
   synchronization. (2026-10-07)
10. **Is "team" the right word?** (2026-10-07)
11. **Shaped for AI-assisted development** as well as for people.
12. **Reason from the hardware, not from osoe.** osoe was a blank-page attempt
    at a working 1.6 over Ethernet, not informed by how high-speed networks
    implement SHMEM; what hardware does comes from the core-API review's
    survey, and osoe is one data point. (2026-10-07)
13. **A thread model from first principles**: elegant, usable and above all
    implementable, since the corpus's problems were bound by SHMEM's history
    and the specification's own model has been the recurring trouble.
    (2026-10-07)
14. **Three layers, each with a rule of admission**: a coherent PGAS core
    aligned with the hardware, which vendors implement; an OpenSHMEM 2.0
    standard library of the functions complete programs need, built on the
    core, with nothing admitted for ergonomics alone; and OpenSHMEM 1.6 built
    on the two. (2026-10-07)

## 2. The proposal on one page

Three layers, and the rule that admits a call to each:

- **The core.** A call is admitted if removing it changes what programs can
  express (*necessary*) or changes a cost class on real hardware (*native*):
  one round trip to two, O(1) to O(P), native to emulated. Vendors implement
  the core and nothing else. Twenty-five calls and one flag.
- **OpenSHMEM 2.0, the standard library on the core** (§7). A function is
  admitted if a complete PGAS program needs it, it is built from core calls
  with no change of cost class, it has one spelling, and its cost is stated as
  the core calls it makes. Written once, portable, shipped with the
  specification; a vendor may replace any function with a native one whose
  contract holds. Nothing is admitted for ergonomics alone. About fifteen
  functions.
- **OpenSHMEM 1.6 on the two** (§8). The compatibility library, allowed to be
  slow where 1.6's semantics are expensive.

### The core

In the shape of the talk's backup slide, with the reason each call is in the
core beside it:

| | Calls | Why in the core | What it answers |
|---|---|---|---|
| **Job** | `open` `close` `abort` | necessary | a job that starts, releases, and ends with its status in band; `open` returns the world team and fills the capabilities |
| **Teams** | `split` `rank` `size` `translate` `release` | necessary | *who*, as an arithmetic rule with a tag; and *the realm*, the scope in which this PE's operations are ordered and completed; one thread's at a time |
| | `node_mates` | native | locality, the runtime's knowledge: a team's members on my node, in O(1) |
| **Memory** | `region_create` `region_release` | necessary | symmetric regions over a team, declared by purpose and placed at one offset on every member; a reference `(region, offset)` is arithmetic on the base `region_create` returns |
| | `direct` | native | a node-mate's pointer, with an ordering contract |
| **Data** | `put` `get` | necessary | nonblocking, on a team to a member of it |
| | `put_signal` | native | the data, then an add to or a set of a waitable word fenced behind it, as the one NIC operation it is |
| **Order** | the flag `FENCED` | native | placed after my earlier operations on this team to this target: the NIC's fence bit, a quiet otherwise |
| **Atomics** | `compare_swap` `read` | necessary | the universal atomic, and a coherent read of a word the atomic engine writes |
| | `fetch_add` `add` | native | the hottest fetching atomic, and the same without a reply |
| **Waiting** | `await` | necessary | my own waitable word, a comparison, a deadline |
| **Completion** | `quiet` | necessary | everything this PE issued on the team is complete and visible, collectives included |
| | `quiet_to` | native | the same for this PE's operations to one member: per-peer completion, which the hardware has |
| **Arrival** | `sync` | native | arrival, posted now, in effect once my earlier operations on the team are complete, released at `quiet`; a tree of atomics and awaits in software, an engine in hardware |
| **Collectives** | `broadcast` `reduce` | native | nonblocking, named by (team, sequence), begun once the PE's earlier operations on the team are complete; what in-network engines offload |

Sixteen calls are necessary; nine, and the flag, are native. The reference
constructors `reference_to` and `reference_offset` are inline arithmetic in the
core's header, part of the reference type and not calls (§4.7).

### The 2.0 library

`allocate` `free`; `set`; `barrier`; `allgather` `exchange` `exchangev`
`collect`; the derived atomics, swap, and, or, xor, min, max and the 32-bit
forms; `lock` `unlock`. §7 gives each its recipe in core calls and its cost.

### The rules

Four rules make the layers one system:

1. **Every call returns a status, and every wait takes a deadline.** `DONE`,
   `NOT_YET`, `PEER_FAILED`. Misuse is a named fatal in every build, never a
   status and never undefined (L11).
2. **A call that fetches a value returns it; every other call completes at its
   team's `quiet`.** There are no blocking and nonblocking variants of anything,
   and no object stands for an operation in flight.
3. **A team is a rule, and the rule is its identity.** Membership, translation
   and identity are arithmetic on `(start, stride, count)` relative to the
   parent, so creating a team needs no communication, and a team costs O(1) per
   member however large it is.
4. **A team is one thread's at a time.** The owner may change through the
   program's own synchronization; threads that communicate at once use
   different teams, created thread-safely by rule and tag. Completion,
   collectives and waits follow: a quiet is the owner's, a collective is one
   call per PE from the owner, a wait is any thread's on its own PE's word (§6).

Fence, quiet and barrier survive as the core's *Order*, *Completion* and
*Arrival* rows: a flag on an operation, the quiet of a team, and a sync
completed by quiet, three different questions that hardware answers with three
different mechanisms, none derivable from the others without a round trip. What
does not survive is their unscoped form.

## 3. What the corpus says

Two surveys of the reading corpus (`corpus/`) were run for this proposal on
2026-10-07, read from source and not run. The first asked how programs write
pipelines and producer-consumer hand-offs: bale's exstack and conveyors, PRK's
Synch_p2p, Stencil and Transpose, DeepEP, pplx-kernels, Triton-distributed,
the NPB codes and the SOS examples. The second asked how every program uses
the completion and grouping routines. The findings, each with what it decides.

### 3.1 Pipelines and hand-offs

1. **Receiver readiness is a data-carrying flag in every pipeline, never a
   barrier.** Put, fence, flag in PRK (`prk/SHMEM/Synch_p2p/p2p.c:296-299`),
   bale's porters (`bale/src/bale_classic/convey/putport.c:261-263`) and
   Triton; put-with-signal in pplx (`internode_dispatch.cu:157-165`),
   Triton's pipeline-parallel hand-off and NCCL's GIN; an atomic add on a ring
   tail in DeepEP (`legacy/internode.cu:823-840`). Barriers carry readiness
   only in the bulk-synchronous codes: exstack, NPB's IS, SP/BT and MG.
   hclib's Graph500 carried a payload-and-header `put_signal` of its own
   before the specification had one. So `put_signal` and `await` are the
   hand-off primitives (§4.4, §4.6), and a team's `sync` is not one.
2. **Flags are monotone or epoch-tagged, and rarely cleared.** bale's
   `2·count + last`, PRK's `iter + 1` and a parity sense, Triton's
   `2·repeat + k`, GIN's `≥ target`; pplx and one Triton tutorial reset to
   zero. So a signal by add, and an `await` with a comparison (§4.6, Q11).
3. **Source-buffer reuse is obtained five ways, and only one of them needs
   `quiet`**: a blocking put's return; a receiver's credit or acknowledgement
   that implies the put landed (bale's `consumed` and `s_can_send`, PRK
   Transpose's `send_flag`, Triton's flag reset, DeepEP's head, GIN's
   credit); a send ring that shares its slot index with the receive ring
   (DeepEP); per-message source space with a handshake at the phase's end
   (pplx, NPB); and a quiet (bale's exstack2 and nonblocking porter, DeepEP
   at phase boundaries, the specification's pipelined reduce, SOS's
   mandelbrot). That is §5's remark: a credit protocol is the better pipeline,
   and `quiet` is for where nothing comes back.
4. **The completion subset programs want is per destination or per frame,
   never per operation.** Per frame: the specification's own pipelined-reduce
   example (`tests-sos/test/spec-example/shmem_ctx_pipelined_reduce.c:72-77`)
   and SOS's mandelbrot keep a context per frame parity, quiet the previous
   frame's and double-buffer, which is a sub-team of itself per frame (§4.2).
   Per destination: bale's nonblocking porter keeps per-partner bookkeeping
   and pays a full quiet for it, batched and rate-limited, counting its quiets
   as a first-class statistic (`convey/porter_impl.h:42-48,127`); DeepEP
   bypasses NVSHMEM's API for a per-(PE, queue pair) quiet of its own
   (`legacy/ibgda_device.cuh:489-495`); GIN flushes per peer
   (`flush_async(peer)`). No program in the corpus keeps a context per
   partner, and none wants a handle on an operation. So `quiet_to` (§4.3,
   **Q16**): the per-destination quiet is the one subset that per-peer completion
   makes O(1), where a pair team per partner would be P − 1 teams a PE inside
   an all-to-all.
5. **Depth.** One for the bulk-synchronous codes; two for the stencil, the
   pipeline-parallel hand-off, DeepEP's low-latency path and the
   specification's examples; 6 to 32 chunks or slots for DeepEP's rings and
   Triton's queue; n columns for PRK's wavefront. Deep rings never complete
   per slot: the receiver returns a monotone counter. So sub-teams per slot
   serve a depth of two to four, and credits serve the rest (§5).
6. **Wherever a set of PEs is used collectively it is an arithmetic
   progression.** bale's conveyor hops are a row (start pe − pe mod n, stride
   1), a column (stride n) and a "middle" progression (stride n²)
   (`convey/tensor.c:398-401,462-465`), and bale's communicator type is
   literally `{start, stride}` (`mpp2shmem.h:60-63`); PRK's neighbours are
   id ± 1 and id ± Px; NPB's rings have strides 1 and p; DeepEP's RDMA team
   is `split_strided(WORLD, rank mod 8, 8, nodes)`; GIN's teams are world,
   node and rail (the same local index across nodes, stride PPN); Triton's
   rail partner is `local_rank + node × local_world_size`. What is **not** a
   progression is never a team: expert routing (pplx, DeepEP, Triton), SP/BT's
   diagonal ring partner, mandelbrot's masked round-robin and bale's random
   destinations are *destinations* chosen per item and addressed in the
   world. So teams as progressions (§4.2) lose nothing the corpus does, and
   an irregular pattern addresses ranks of a progression and needs, at most,
   `quiet_to`.
7. **A second scope axis appears in the GPU codes under other names.**
   PyTorch's symmetric memory signals by (peer, channel) and barriers by
   channel (`c10d/symm_mem/NVSHMEMSymmetricMemory.cpp:275-320`); DeepEP's data
   and ring tail travel on queue pair c and its credits on c + C. A channel is
   a group with a second completion scope: a sub-team of itself (§4.2).
8. **A team needs symmetric scratch of its own.** bale's SHMEM conveyors
   refuse any communicator but the world because they cannot allocate
   symmetric memory for a team (`convey/simple.c:464-467`). So regions are
   created over a team (§4.7, Q10).
9. **Ordering is wanted per target in every use.** PRK's stencil offers one
   fence for four neighbours or one a neighbour as a build option
   (`prk/SHMEM/Stencil/Makefile:30-31`). The flag on the operation is per
   target by construction and makes the option moot (§4.3).

### 3.2 Completion and grouping routines across the corpus

Counted as call sites in code, with comments and definitions stripped, bale's
`lgp_barrier` and `mpp_quiet` wrappers expanded, and the SOS tests, the OSU
benchmarks and OSB's micro-benchmarks set apart from the applications.

1. **The barrier is the synchronization programs write.** About 620
   `shmem_barrier_all` call sites in the classic applications and 300 more
   through bale's wrappers, against six call sites for every other
   synchronization routine together: `shmemx_sync_all` four times in one
   program, two demonstration active-set barriers, and `shmem_sync`,
   `shmem_team_sync` and `sync_all` never. Every GPU code ends its phases with
   one. Many are set-up, reset-then-rendezvous or timing barriers that need
   arrival alone, and the two programs that want arrival alone with writes in
   flight, the specification's pipelined reduce and DeepEP, split the barrier
   into a targeted quiet and a sync. So the barrier must be one line (Q6), and
   arrival alone is had by issuing the writes on another team (§4.3).
2. **Contexts are used by seven programs, all benchmark ports** (HClib's
   suite, OSB's threaded variants), and by no application: not bale, ISx,
   NPB, PRK, Kokkos, DeepEP, pplx, Triton or PyTorch. Six use a context per
   thread; three use them also as a completion window, mandelbrot's two per
   thread for double-buffering, Graph500's two rotated every 512 operations
   and the one it moves to quieted, the pipelined reduce's one per stage; one
   hashes destinations into six contexts, with a context fence between
   payload and header (`hclib/.../main-single-mailbox-concurrent-ctx.c:299-309`).
   Each is a sub-team: per thread (§6), per slot (§5), or `quiet_to` (§4.3).
3. **`shmem_pe_quiet` occurs nowhere.** The per-PE quiets that exist are below
   the API: DeepEP's IBGDA call, and MORI's `quiet_pe` and `quiet_pe_qp`
   (`triton-distributed/.../libmori_shmem_device.py:125,138`). Request handles
   (`shmemx_*_nb`, `wait_req`, `test_req`) occur in four demonstration files
   and nowhere else.
4. **Every fence is followed by a flag put or an atomic to the PE that
   received the data**, and no program relies on order between puts to
   different PEs. One fence routinely covers several destinations: the
   stencil's four puts, one fence, four increments
   (`prk/SHMEM/Stencil/stencil.c:470-474`); hclib's emulation of `put_signal`
   as put, fence, put. Triton's three consumer-side fences are local acquires
   after a wait, which `await` provides. So the flag on the operation loses
   nothing (§4.3).
5. **Signals set an epoch and compare for equality.** Triton: 66 `SIGNAL_SET`
   against 4 `SIGNAL_ADD`, 42 `CMP_EQ` against 4 `CMP_GE`, the value a call
   count that only grows; pplx announces a count by a set; PyTorch's symmetric
   memory signals by put-with-signal. An add alone would serve them, since the
   counts are monotone, but it is not their idiom, and a set is a plain write,
   native everywhere. So `put_signal` takes the operation, add or set, as the
   specification's does (§4.4, Q11), and a standalone `set` is the 2.0
   library's (§7).
6. **Subset completion is wanted five ways and met well by none**: per
   partner (bale's exstack2, which quiets three times a buffer; DeepEP; MORI);
   per buffer (mandelbrot); per stage (the pipelined reduce); by rolling
   window (Graph500's two rotated contexts); and per thread,
   `osb/ssca1_threaded/pairwise_align.c:560-611` prefetching from every thread
   on the default context, where each thread's wait covers them all. Realms
   answer four of the five and `quiet_to` the fifth.
7. **Every team and active set is an arithmetic progression.** About 183 of
   190 active-set calls are the whole world; the rest are contiguous blocks
   (Graph500's partitions), grid rows and columns (bale's twohop, which takes
   the vendor's team path when the column stride is not a power of two,
   `convey/twohop.c:674-677`), DeepEP's rail of stride 8, and the SOS tests'
   teams of every stride, negative included. PyTorch's kernels ask for "one
   team per block", N teams of one membership (`nvshmem_extension.cu:1083,1169`),
   which is the identity rule (§4.2). `SHMEM_TEAM_SHARED` is only queried; the
   node team serves device selection and multicast pointers.
8. **Locks** are used by UTS and the demonstrations alone. **Timeouts**:
   DeepEP writes its own barrier with timeouts and rank masking and polls with
   timeouts in eleven kernel files, which rule 1 gives every program.

### 3.3 A communicator stack on OpenSHMEM 1.4: MppUtil

`corpus/openflame/MppUtil/`, IDA's `mpp_utilV4`, the framework under bale's
conveyors, built a team construct on OpenSHMEM 1.4's active sets, and its
choices are the proposal's prior art (read 2026-10-07: `src/comm.c`,
`doc/mpp4_comm.pod`, `doc/mpp4_comm_stack.pod`, `doc/mpp4_thread.pod`).

1. **A communicator is `(size, my_pe, start, stride)`, and equality compares
   the rule** (`mpp_comm_is_equal`): identity by rule, in shipped code. Splits
   are relative to the current communicator: rows of x, rows and columns,
   three dimensions by splitting the yz communicator again, or an explicit
   strided set. So §4.2's rule, and `split_2d` as two splits.
2. **A process-global stack of communicators**, 64 deep, whose top is the
   implicit argument of almost every call through the `PROCS` and `MY_PROC`
   macros; push and pop optionally barrier before and after; a library pushes
   on entry and pops on return, and so runs on any subgroup unchanged. Its
   manual says the stack is shared by every thread of the process and that
   changing it while sibling threads call is unsafe. The nesting is the
   proposal's tree of teams, of which a stack is one path; the implicit
   argument is what §9's first item forbids. A thread-local current team with
   push and pop is a convenience header above the core (§4.2's table).
3. **Every split is collective over the current communicator** and agrees a
   slot in a symmetric pool of 32 internal records, pSync arrays and one SHMEM
   lock, by an OR reduction of in-use masks, the mechanism SOS uses for its
   pSync pool and osoe adopted; non-members take part and receive the null
   communicator. Identity by rule and tag makes the agreement unnecessary
   (§4.2).
4. **What 1.4 denied it**: strides had to be powers of two (its manual
   expected the restriction to lift, and 1.5's teams lifted it); 32
   communicators; symmetric allocation on the world alone, so a conveyor on a
   sub-communicator fails for want of scratch (`Convey/convey.h`:
   "communicator is not `MPP_COMM_WORLD`, and `mpp_alloc()` does not support
   teams"); collectives single-threaded, the library's scratch workspace "not
   thread-safe, as it's only intended to be used by collective routines, which
   cannot be multithreaded". Each is a row of §4.2 or §4.7.
5. **Its thread model** (`mpp4_thread.pod`, `src/thread.c`): a threading
   adapter of lock, unlock and thread id, for pthreads or Loom; a mutex around
   every put, get and atomic when the SHMEM beneath is not thread-safe ("in
   OpenSHMEM 1.3 locks and atomics are not thread safe",
   `test/test_threadsafety.c`); worker threads registered with the NIC by a
   vendor call, with "non-trivial overhead", the advice to register once and
   never unregister, and a warning that NIC resources can be exhausted;
   collectives funnelled through one thread with a thread barrier on either
   side (`Mpp_Barrier_Loom`). §6 makes the registration the team's and the
   funnel the model.
6. **Loom** (`corpus/openflame/Loom/`), the thread library beside it, organizes
   threads in a tree of warps split by colour, with barriers and reductions
   of their own: the thread-level analogue of teams, and where thread
   collectives belong (§6).
7. **Its worked example**, clusters as rows with leaders in a column and a
   dynamic job queue on the column, is the second test case beside §5's
   pipeline: two splits and no stack on this API.

## 4. The items

### 4.1 The job is the world team

```c
typedef int64_t oc_deadline;          /* absolute nanoseconds; OC_BLOCK waits, OC_POLL returns at once */

oc_status oc_open (const struct oc_options *, oc_team *world, struct oc_capabilities *);
void      oc_close(oc_team world);                                 /* local; nothing collective */
_Noreturn void oc_abort(oc_team, int status, const char *reason);   /* ends the job, in band */
```

**What.** `oc_open` returns the world team, every PE in the job in the order
the launcher gave, and fills the capabilities: which calls are native here, the
budgets of teams, hardware queues, counters and per-thread records, and the
node map. Rank and size are team queries (`oc_team_rank(world)`). `oc_options`
carries the job's shape,
which ranks share a node, so that one host is a cluster by definition and every
program is testable on a laptop (L13); the thread level; and the diagnostic
sink. `close` releases resources and is local: the barrier that OpenSHMEM's
`finalize` hides is written as `oc_sync(world); oc_quiet(world)` before it
(L8). `abort` is a request the runtime carries out in order, ahead of the
connections' end on every stream (L32, L33), not a call to `exit`. `open` and
`region_create` are the two cold calls that return only when their work is
done; everything else follows rule 2.

**Why no job object.** The strawman had `oc_job` beside its groups. Everything a
job object would answer, the world team answers, and a separate object is one
more thing to pass. Every team descends from the world, so the world is the
root of all identity.

| Alternative | Why not |
|---|---|
| An implicit global job, as OpenSHMEM's `shmem_init` | contracts stop being local: a call site cannot be judged from the handles in scope (§9); two libraries in one process cannot each open a job |
| A separate `oc_job`, the world team derived from it | one more object for no answer the world team does not give; **Q13** |
| A separate `query` call for the capabilities | they are read once, at start, and never change; `open` fills them and the core is one call smaller |
| `finalize` as barrier plus teardown | L8: the root leaves first while owing the release, and `atexit` turns a fatal into a hang |

### 4.2 Teams: a rule for who, and the realm of completion

```c
oc_status oc_team_split     (oc_team parent, int start, int stride, int count, uint32_t tag, oc_team *out);   /* local; thread-safe */
oc_status oc_team_node_mates(oc_team, oc_team *out);                 /* the one locality rule */
int       oc_team_rank(oc_team);    int oc_team_size(oc_team);
int       oc_team_translate(oc_team from, int rank, oc_team to);    /* -1 if not a member */
void      oc_team_release(oc_team);                                  /* local */
```

A team is two things at once (whether "team" is the word for it is **Q5**).

**Who: an arithmetic progression of its parent's ranks.** Rank *i* of the team
is rank `start + i × stride` of the parent, for `0 ≤ i < count`. The stride
may be negative, as the specification's `split_strided` allows, so a reversed
team costs nothing, and it never wraps. A progression of a progression is a
progression, so every team is affine in world ranks, and translation to the
world or between any two teams is a multiply, an add and a divisibility test,
O(1), with the one exception below. No team holds a list of members, so a team
of a million PEs costs each member the same few words as a team of two (L6).
This is the whole of OpenSHMEM's team space: every team descending from
`SHMEM_TEAM_WORLD` is an arithmetic progression in world ranks, `split_2d`
included (`docs/decisions/2026-09-26-day.md`, item 7); only the shared team's
splits are not, and they never leave a node.

**The locality rule.** `oc_team_node_mates(T)` is the team of T's members on
the caller's node, in T's rank order. For the block and cyclic distributions
every launcher produces it is a progression too, and the implementation uses
that; for an irregular node map it is the one team whose translation is a
lookup, in a table of the node's size kept once per node, O(PPN) and never
O(P). It never leaves a node, and its sub-teams are progressions of it.

**The realm: where this PE's operations are ordered and completed.** Every
point-to-point operation names a team and a rank in it. The team scopes the
operation twice: the rank is translated through it, and the operation is
counted in it. `oc_quiet(T)` completes everything this PE issued on T; a
`FENCED` operation on T is placed after the PE's earlier operations on T to
the same target; a collective on T begins, for this PE, when its earlier
operations on T are complete. Nothing issued on another team is touched by any
of these. So a team is OpenSHMEM's team and its context in one object, and the
one job a context did that a team did not, scoping completion, becomes a
property of membership.

**A sub-team of itself.** `oc_team_split(T, 0, 1, oc_team_size(T), tag, &T2)`,
the identity rule with a tag, makes a team with T's members and a realm of its
own. It is how
a program gets two completion scopes over the same peers: one per thread, one
per buffer of a double-buffered exchange, one per phase. It is the construct the
author asked for in place of handles, and §4.3 says why it does the handles'
job where handles cannot be built.

**Identity without communication.** A team's identity is its parent's identity,
its rule, and a *tag* the program supplies: (parent, start, stride, count,
tag). The tag tells apart teams of one rule over one parent: a thread's index
for a per-thread sub-team, a slot's index for a buffer's, a constant of a
library's own; it is zero where there is one team of a rule. Every member
computes the same identity, because the rule is canonical, there being one way
to write a progression, and because the tag is the program's intent and not an
accident of call order. So creating a team sends no message, waits for no one,
and is thread-safe; a second live team of one identity on one PE is a named
fatal. The strawman's first open question, whether identity can be
communication-free when members split in different orders, is answered for
progressions by the rule and the tag being the name; it was open only because
the strawman allowed range lists, which have no canonical form, and counted
splits, which depend on order. Collective resources, a counter set and whatever
per-node state the implementation keeps for a team (osoe: a block of counters
and small slots), are bound at the team's first collective, found by the
identity on a node-local map, and freed when the last member on the node
releases the team. A member that never created the team is not detected at
creation; a collective on the team then waits for it, and a deadlock detector
names the PE and the team (osoe has one: "a program's own deadlock ends the
job once it is proved", `docs/decisions/README.md`). A member that created it
with different arguments is caught by the collective's signature (L46).

**Non-members do not call.** OpenSHMEM's split is collective over the parent
and hands non-members `SHMEM_TEAM_INVALID`; here only the members of the new
team call the split, since nobody else needs to know. A pair team between
pipeline neighbours is made by the two of them, and the world does not stop
for it.

**What a team costs.** Per member: the rule and its affine map, and an issuer's
completion state, which is what a context costs in the surveyed transports
(`core-api.md` §2.8): a share of a transmit queue, a completion counter pair,
no lock while one thread owns it, and nothing per peer. Hardware counters are
a budget: CXI gives sixteen a domain, so a few teams own hardware counters and
the rest count completions in software from the queue's completion events, one
event an operation, which is how libfabric providers without counters and UCX
complete anyway; a software transport keeps a per-peer dirty set instead
(osoe's context: a cache line and a small array, design §4.11). Once a
collective has run on the team, whatever per-node collective state the
implementation keeps (osoe: about 15 KiB a PE at its defaults). The teams one
PE may hold at once, and how many of them own hardware queues and counters,
are in the capabilities. A split costs no message; a release costs none.

**What a team is not.** It is not an atomicity domain: atomics on two teams to
the same word are atomic with each other, because atomicity is a property of
the memory (an atomic region, §4.5), not of the realm. It is not a protection
domain: a team scopes which ranks a call can name and nothing about who may
write a region. And it is not a stream in the hardware sense: the
implementation may back several teams with one queue or one team with several,
as long as the completion promise holds.

| Alternative | Why not, or where open |
|---|---|
| Teams for membership, contexts for completion (OpenSHMEM 1.4 on) | two concepts for one job; a context created from a team carries both and scopes atomicity besides (strawman §4.1); the author's direction 4 |
| Groups and streams as orthogonal objects (the strawman) | a stream is local and a group collective, so a program that wants to complete its operations to *these* peers keeps a stream per peer set anyway, which is a team; two objects where one does |
| Arbitrary member lists (`MPI_Group_incl`; the strawman's `ranges`) | O(members) memory per team per PE (L6); no canonical form, so creation must communicate; the author's direction 5 |
| Identity by a per-parent sequence number alone (the strawman) | communication-free only if every member splits in the same order, and not thread-safe; the rule and a program-supplied tag need neither (**Q18**) |
| An implicit current team on a process-global stack (MppUtil, §3.3) | a call's meaning depends on distant state, and the stack is shared by the process's threads, which its own manual calls unsafe; a thread-local current team is a convenience header above the core |
| Job-wide team numbers (osoe's first design) | L7, L47: a job-wide count sizes per-node state and caps grid splits; replaced in osoe by per-PE numbers, here by identity from the rule |
| A k-dimensional progression (a Cartesian plane; two interleaved stages together) | not one progression; nested strides keep membership O(k); **Q1** |
| Collective creation over the parent (OpenSHMEM) | agreement at creation, at a logarithmic cost a split, and every parent member calls; **Q2** |

### 4.3 Order, completion and arrival: the triad re-examined

The author asked whether fence, quiet and barrier are key or too much. They are
three answers to three questions, and the test of whether a question is
essential is whether hardware answers it with a mechanism of its own.

| Question | OpenSHMEM's answer | What hardware has | Here |
|---|---|---|---|
| **Order**: will my later write to *p* land after my earlier one? | `shmem_fence`, a call that opens a window | a fence bit on an operation (CXI's `CQ_FENCE`; mandatory in Ultra Ethernet's HPC profile; mlx5's fence bits); free on an ordered stream (osoe's one TCP stream a node pair; Portals with total ordering); a drain where neither exists | the flag `OC_FENCED` on `put`, `put_signal` and `add`: placed after every earlier operation of this PE on this team to this target, across every rail. No standalone call |
| **Completion**: are my writes complete and visible? | `shmem_quiet` on a context: everything it ever sent | counters per endpoint, found sufficient in all six transports surveyed (`core-api.md` §2.6); a flush per dirty peer where completion does not imply visibility | `oc_quiet(team)`: this PE's operations on the team, point-to-point and collective; `NOT_YET` at a deadline (L2) |
| **Arrival**: has everyone reached this point? | `shmem_barrier` (quiet, then sync) and `shmem_sync` (arrival alone) | in-network barriers (SHARP); triggered operations that fire when a counter reaches a value (Portals); a tree of puts otherwise | `oc_sync(team)`: arrival, posted now and taking effect once this PE's earlier operations on the team are complete; released at `oc_quiet(team)` |

**Verdict.** None of the three can be built from the others without a round
trip: order without a fence bit is a quiet between the two writes; completion
without counters is an acknowledgement per operation; arrival without a
collective is a put and a wait per member. So the triad is key. What is too
much is its *unscoped* form, a quiet that must cover everything a context ever
sent, which L1 names as the root of osoe's acknowledgement machinery, and the
fence as a *call*, whose window the implementation has to track per (context,
peer) to find "the next operation to each rank" (`core-api.md` §2.5). Scoped to
a team and reduced to a flag, the three are the whole of synchronization: order
within a realm, completion of a realm, arrival of a realm.

**Why `quiet` on a team does the handles' job, and handles cannot do it.** A
handle that stands for a list of remote operations needs a record per operation
in flight, completion tracked out of order against those records, and a wait
that checks the list. No surveyed transport works that way, and the one that
comes closest, Portals' per-operation events, is the one SOS turns off in
favour of counters. The strawman's *mark*, a counter value, avoids the records,
and on a NIC with a completion counter per endpoint, CXI, Portals, Ultra
Ethernet, it is a scalar and cheap. On a transport that completes per peer,
UCX's flush per endpoint, Open MPI's per-peer dirty set, osoe's cumulative
acknowledgements per node pair, a mark is a vector of per-peer high-water
marks, and a program that takes marks freely keeps O(marks × peers) of state,
a handle in a thinner coat. So a mark's cost is the transport's and not the
API's, cheap on one fabric and O(peers) on another, which is the portability
failure an API exists to prevent. A team's realm is a counter set the program
declares once, bounded by the program's structure and not by the operations in
flight; `quiet(T)` is the same algorithm whatever the transport; and a program
that wants finer completion makes a finer team, or asks `quiet_to`. The
author's instinct is right, and the reason is that completion on real
transports is per issuer and often per peer, never per operation.

**`quiet_to`: completion toward one member.** `oc_quiet_to(T, r)` completes
this PE's operations on T to rank r of T and nothing else. The survey (§3.1,
item 4) found that the one completion subset programs reach for inside an
all-to-all is per destination: bale's porter keeps per-partner bookkeeping and
pays a full quiet, DeepEP reaches under NVSHMEM's API for a per-(PE, queue
pair) quiet, GIN flushes per peer, and AMD's MORI library gives device code `quiet_pe`
and `quiet_pe_qp` (§3.2, item 3); hclib's Graph500 hashes destinations into
six contexts for the same end. A sub-team per partner would be P − 1 teams a
PE, and O(P) state is what L6 forbids; `quiet_to` costs nothing beyond the
per-peer completion state the realm already keeps. On a transport that
completes per peer (osoe's dirty set; a queue pair per peer) it is O(1); on
one with shared endpoints and few counters it is a flushing read to that peer
or a quiet of the realm, declared in the capabilities. It is OpenSHMEM 1.6's
`pe_quiet` for one PE, a scope and not a handle, and a list of PEs is a loop.

**Honesty about L1.** Team-scoped completion keeps the counter and
acknowledgement machinery; it scopes it. What goes is the pressure toward
per-operation state and the one scope that covers everything. The machinery is
intrinsic to counter completion, which the hardware survey found universal, so
this is the floor and not a flaw.

**`barrier`.** OpenSHMEM's barrier is quiet then sync. Written here it is
`oc_sync(T); oc_quiet(T)`, and it means exactly that: the PE's arrival takes
effect once its operations on T are complete, and `quiet` returns when every
member's has, so no member is released while another's writes on T are in
flight. The two-call spelling is deliberate: a program that has issued nothing
on T pays nothing for the completion half, and a program that wants arrival
alone with writes still in flight issues those writes on another team. Whether
to spell `oc_barrier(T)` as an inline over the two is **Q6**.

| Alternative | Why not, or where open |
|---|---|
| A fence call (OpenSHMEM; the core-API review's `ctx_fence`) | window state per (team, peer); the flag names the operation that is ordered and maps to the NIC's bit directly; L14 |
| Ordered delivery by default (Ultra Ethernet's ROD; GPI-2's queues) | forfeits spraying and multi-rail everywhere to serve the few operations that need order |
| No fence at all; `quiet` between dependent writes | a round trip for every data-then-flag pair; `put_signal`, the hot pattern of the AI codes, is built on the fence bit |
| Per-operation handles (`shmemx_*_nb`, MPI requests, GASNet events) | the author's direction 6; a record per operation; no surveyed transport is built that way |
| Marks (the strawman) | a vector, not a scalar, on transports that complete per peer; replaced by sub-teams; **Q4** |
| `pe_quiet` with a list of PEs (OpenSHMEM 1.6) | no OpenSHMEM users in the corpus (`core-api.md` §3(b)), though the GPU codes build its one-PE form below the API (§3.1); `quiet_to` is that form, and a list is a loop |
| `sync` released on return, independent of `quiet` | breaks rule 2 for one call; the corpus wants arrival alone at six call sites against about nine hundred barriers (§3.2, item 1), and the two programs that want it with writes in flight already keep those writes in a scope of their own, which here is a second team |
| `barrier` as the only collective sync (OpenSHMEM 1.0) | a program that has quieted already pays a quiet it does not need, though with teams the cost is small, since only operations on T are waited for |

### 4.4 Data movement, and the one completion rule

```c
#define OC_FENCED  (1u << 0)   /* placed after my earlier operations on this team to this target */
#define OC_MORE    (1u << 1)   /* more follows to this target: a hint for aggregation */

oc_status oc_put       (oc_team, int rank, struct oc_reference dst, const void *src, size_t len, unsigned flags);
oc_status oc_get       (oc_team, int rank, void *dst, struct oc_reference src, size_t len, unsigned flags);
enum oc_signal { OC_SIGNAL_ADD, OC_SIGNAL_SET };
oc_status oc_put_signal(oc_team, int rank, struct oc_reference dst, const void *src, size_t len,
                        struct oc_reference signal, enum oc_signal, uint64_t value, unsigned flags);

oc_status oc_quiet   (oc_team, oc_deadline);             /* everything this PE issued on the team: complete and visible */
oc_status oc_quiet_to(oc_team, int rank, oc_deadline);   /* the same, for this PE's operations to that member alone   */
```

**Every one of these is nonblocking and completes at `oc_quiet(team)`.** The
source of a `put` is the caller's until then; the destination of a `get` holds
the bytes after it. There are no `_nbi` twins (osoe exports 4,857 `shmem_`
entry points, most of them a blocking and a nonblocking spelling of a typed
routine, L44) and no blocking bulk form: a blocking put is a put and a quiet,
and a program that wants one usually wants the quiet for other reasons too. The
one hot blocking pattern, a scalar get (`shmem_g` in Kokkos, GUPS and bale, the
hottest calls in the usage survey), is a fetch and returns its value under rule
2: `oc_read` (§4.5), which on a bulk region is a get of one word.

**`put_signal`** is a put whose arrival the target can wait for: the data, then
an atomic add to, or an atomic write of, a waitable word, fenced behind it, in
one call. The add counts arrivals from several producers; the set announces an
epoch or a value, and is the idiom of the GPU codes (§3.2, item 5). It is the
hottest primitive of the NVSHMEM codes (pplx, DeepEP, Triton-distributed: a
call per token) and what every surveyed NIC does as write work requests
followed by a fenced atomic one; it is also what osoe's collectives are built
from. One call rather than `put` then `add` with `FENCED`, because the pair is
what hardware does and what programs mean, and because a compound call lets an
implementation use a NIC's native signalled write where it has one
(`core-api.md` §7.2, C2). The flag form stays available to a program that wants
one signal after several puts.

**Addressing.** A target is `(team, rank, reference)`, and the reference is
`(region, offset)`, the same on every PE, so the wire never carries an address
and no PE keeps another's base (L6; `core-api.md` §2.1). The caller's side of a
bulk operation may be any local memory; the capabilities say whether a `get`
into private memory lands directly or through a landing area copied out at
`quiet`, which is osoe's way, forced by a progress thread in another process
(design §3.12).

| Alternative | Why not, or where open |
|---|---|
| Blocking and nonblocking variants (OpenSHMEM) | doubles the surface for one bit of information the team's `quiet` already carries; L44 |
| A blocking bulk `get` | needs completion per operation inside the core (`core-api.md` §2.3), the one place the survey found per-operation state unavoidable, for a pattern the corpus does not have at bulk sizes; a scalar read is a fetch and returns |
| `put` then a fenced `add` in place of `put_signal` | two calls the implementation must recognize as one to use a native signalled write |
| A signal by add alone | the corpus's signals set an epoch and compare for equality, 66 sets to 4 adds in Triton (§3.2, item 5); a set is a plain write, native everywhere, and cheaper than an add; **Q11** |
| Typed routines | L9, L44: the cross product is the symbol table, not the semantics; a `_Generic` convenience header may exist above the core, with its own failure path (L37) |
| Strided and blocked transfers (`iput`, `ibput`) | no application users (`core-api.md` §3(b)); loops of puts with `MORE` |

### 4.5 Atomics: the hardware's intersection, the rest priced

```c
oc_status oc_fetch_add   (oc_team, int rank, struct oc_reference, uint64_t addend, uint64_t *old, oc_deadline);
oc_status oc_compare_swap(oc_team, int rank, struct oc_reference, uint64_t expected, uint64_t desired,
                          uint64_t *old, oc_deadline);
oc_status oc_read        (oc_team, int rank, struct oc_reference, uint64_t *value, oc_deadline);
oc_status oc_add         (oc_team, int rank, struct oc_reference, uint64_t addend, unsigned flags);   /* no reply */
```

64-bit words, 8-byte aligned by construction, in regions created `OC_ATOMIC`
or `OC_WAITABLE`, except `oc_read`, which reads a word of any region. The
three that fetch block for their round trip and return the value, rule 2, with
a deadline; `add` does not fetch and completes at the team's `quiet`.

**Why these four.** The core-API review's required set was write, read,
compare-and-swap and add (`core-api.md` §2.4); this is that set with the
fetching and non-fetching forms told apart by what programs use, and with the
write where it belongs: as the set operation of `put_signal`, and as the 2.0
library's standalone `set` built on it (§7), since where the signaller is a
word's only writer an aligned write is atomic on every fabric, and that is what
a signal is. The stage-setting's §3 table: the intersection of PCIe
AtomicOps, IBTA RoCE and mlx5 is {fetch-and-add, compare-and-swap} on 64 bits,
always fetching. Add swap and stock IBTA is lost; add 32 bits and it is lost
again, and a 32-bit atomic cannot be emulated on the containing word without
corrupting a legal neighbour store (L10). A non-fetching `add` is the same NIC
operation with the reply discarded, and the hottest atomic in the corpus:
histograms, GUPS, bale, DeepEP's `amo_nonfetch_add`. `read` is a fetch every
fabric has in some form. Everything else, swap, and, or, xor, min, max, 32-bit
and floating-point, is *derived*: a compare-and-swap loop, two round trips or
more under contention, or a target-CPU handler. It is the 2.0 library's, each
routine with its cost in its header and a vendor free to substitute a native
one (§7, **Q7**).

**Atomicity is the region's.** All atomics to a word go through one engine, the
NIC's or the CPU's, chosen per region and never both, the RoCE review's lesson
for NICs whose atomics the CPU cannot see. Teams do not enter into it. A word
written by atomics is read coherently by `oc_read` or `oc_await`, never by a
plain load unless `oc_direct` returned a pointer, whose contract covers it.

| Alternative | Why not, or where open |
|---|---|
| OpenSHMEM's atomic set (Tables 6 to 8: eleven types by twelve operations) | L10; the native-coverage table of `core-api.md` §2.4 |
| Swap in the core | native on PCIe atomics, mlx5 and Ultra Ethernet; absent from stock IBTA; **Q7** |
| Min and max | Graph500 emulates them at four round trips an edge (`core-api.md` C9); native on Portals, CXI and UET, not on IBTA; **Q7** |
| Nonblocking fetching atomics | L41: the fetch needs a landing place the library owns, and OpenSHMEM's is any local address; a round trip is the honest cost, and the corpus has no users of `_nbi` fetches |

### 4.6 Waiting

```c
enum oc_compare { OC_EQUAL, OC_NOT_EQUAL, OC_GREATER_EQUAL, OC_LESS_EQUAL, OC_GREATER, OC_LESS };
oc_status oc_await(struct oc_reference own_word, enum oc_compare, uint64_t value, oc_deadline);
```

The caller waits on a word of its own, in a region created `OC_WAITABLE`, until
the comparison holds or the deadline passes. Only atomics and `put_signal`
write such words, never a put and never a store, which is what lets an
implementation wake the waiter exactly: it knows every path that can change the
word (L3: osoe's litmus test found 58 lost wakes a million on the put path when
every write counted). On a NIC these are the words that need event bridging,
and the region tells the vendor which they are. A deadline is mandatory in
spirit, `OC_BLOCK` waits and `OC_POLL` tests, because a wait with no bound is
where a job hangs without saying why (L2).

A stage with several producers counts them: each adds to one word, and the
stage awaits `GREATER_EQUAL n`. A wait for *any* of several words is not in the
core (**Q12**).

| Alternative | Why not, or where open |
|---|---|
| Waiting on any symmetric word (`shmem_wait_until`) | L3: a two-sided protocol against every write path, `memcpy` included; the specification itself retreated to "update with an AMO" in 1.5 |
| `wait_until_all`, `_any`, `_some` and the vector forms (1.5) | no users; "any" is an engine's primitive (L45), not yet shown to be an application's; **Q12** |
| `test` as a separate call | `await` with `OC_POLL` |

### 4.7 Memory

```c
#define OC_BULK      (1u << 0)   /* puts, gets, collectives */
#define OC_ATOMIC    (1u << 1)   /* atomics */
#define OC_WAITABLE  (1u << 2)   /* words an await may wait on */

oc_status oc_region_create (oc_team, size_t bytes, unsigned purpose, oc_region *, void **base);   /* collective; agrees placement; returns when made */
void      oc_region_release(oc_region);
void     *oc_direct(oc_team, int rank, struct oc_reference);          /* a pointer, or NULL */

struct oc_reference { uint32_t region; uint64_t offset; };                                   /* what the wire carries */
static inline struct oc_reference oc_reference_to(oc_region, const void *local);             /* (region, local - base) */
static inline struct oc_reference oc_reference_offset(struct oc_reference, uint64_t bytes);  /* arithmetic */
```

Kept from the strawman (§4.2), the arguments restated. A region is created
once, collectively over a team, the world for most programs, and exists on
that team's members: each allocates its part, a reference into it is valid
toward members of that team and its sub-teams, and its identity derives from
the team's as a team's does from its parent's, so the members agree on it
without a message. A library that works on a team can then make its symmetric
scratch among the team's members alone, which bale's conveyors cannot today
(§3.1, item 8). The purpose is declared at creation because that is what an
RDMA backend needs to register atomic-capable and waitable memory apart from
bulk memory (osoe's `set_range`, UCX's atomic key), stated once rather than
inferred per allocation. The region is the primitive, and the only one:
`region_create` returns its local base, a program carves it as it likes, and
the two reference constructors are arithmetic on that base, part of the type
and not calls. The allocator over a region, local, deterministic and
barrier-free where the specification's `malloc` is a logarithmic collective for
nothing, is the 2.0 library's (§7). `oc_direct` gives a node-mate's pointer with
osoe's contract: non-`NULL` means that stores through it are ordered against the
library's own writes, not merely "is local". No symmetric statics: L15 prices
the compatibility promise (7 of the specification's 53 examples run without
them), and the library of §8 pays it with a linker script, not the core.

**What a region is to the implementation.** Not a new mapping and not a new
NIC registration. `open` reserves one pool per purpose on each PE, sized by an
option, and registers each once; a region is a range of the pool of its
purpose, with an id in a small per-PE table, and nothing per region reaches
the NIC. The pools are three because waitable and atomic memory need
registration attributes that bulk memory must not carry: on CXI a counter
bound to a memory region disables optimized RMA for everything in it.

**Placement is symmetric within the team.** A region sits at the same offset
of the pool on every member, so that an initiator turns `(region, offset)`
into a target address with one add and no table. Placed per member instead,
every PE would need every member's base for every region, O(regions ×
members), the key-times-chunk-times-peer table the core-API review found in
NVSHMEM's addressing (NV §2). Symmetric placement has a cost of its own: the
members must find a range free on all of them at once, and a PE in several
teams has its pool carved by each in an order the others never see, so a hole
free on one member is taken on another, which is MppUtil's "your app is
fragmenting the communicators" (§3.3) in memory's form. The design below keeps
that agreement to one per team.

**Two ends of the pool.** Regions of the world team grow from the bottom by an
identical sequence of calls on every PE, symmetric by construction with no
agreement, as today's heap is. The common case, a program that makes at most
one team, which is every application in the corpus, never meets the rest. Team
arenas grow from the top.

**A team's arena, agreed once.** A team that owns symmetric data gets an arena
at its first `region_create`, which is collective anyway and is where the team
first meets: an AND reduction over the team of each member's bitmap of free
chunks, the lowest common run taken, sized by the request and rounded up to
whole chunks. Every later region of the team is carved from the arena locally,
in call order, and so lands at the same offset on every member with no
message; a region that does not fit triggers one more agreement, correct
always and rare in practice. Arenas are whole chunks of a buddy structure, so
released ones coalesce. An arena is released whole with its team; a live
region at the team's release is a named fatal, and a sub-team's arena is
released before its parent's. A realm needs no memory: the pipeline's pair
teams carry completion and the per-thread sub-teams carry issue, and neither
creates a region, so arenas exist only for the few, long-lived teams that own
data.

**The creation-time discipline.** Create a team's regions at its creation,
before any sub-team of it exists and before the PE returns to its other teams.
Followed, it makes the top end of the pool a tree that mirrors the tree of
teams and fragments little. It cannot be what makes placement correct, since
the agreement is needed wherever PEs belong to different numbers of teams, a
middle stage's PE creating twice the pair teams a first stage's does; and it
cannot be mandatory, since programs allocate on the world in a later phase and
libraries allocate on entry. So it is recommended, and a debug build names a
region created out of its turn (**Q21**).

**Declared.** The pool sizes, the chunk size and the arenas a PE may hold are
capabilities; no common range for an arena is a named fatal that says which
member had none.

| Alternative | Why not, or where open |
|---|---|
| One heap with hints per allocation (`malloc_with_hints`) | hints have no users; purpose at region creation is what registration needs |
| Placement per member, with the members' bases at every initiator | O(regions × members) of state, the table that sank NVSHMEM's addressing (`core-api.md` NV §2) |
| An agreement at every region creation | one per team suffices once its arena exists; later regions are local |
| A sparse reservation far larger than physical memory, pages backed on touch | clean for the kernel and wrong for the fabric: a NIC without on-demand paging pins the whole reservation, optimized memory regions are a budget (about two thousand on CXI, a twelve-bit index on Ultra Ethernet), and the reservation times PEs per node must fit the node-mates' mappings; withdrawn |
| The creation-time discipline as a rule | forbids a later phase's allocation on the world and a library's on entry; recommended and checked instead; **Q21** |
| `allocate` and `free` in the core | an allocator is built on the region with no change of cost class, so the admission rule puts it in the 2.0 library |
| Regions always job-wide, whatever team creates them | every PE of the job would take part in a team-local library's set-up, and bale shows the need for a team's own scratch (§3.1, item 8); **Q10** |
| Symmetric statics | L15; a toolchain matter, kept by the library of §8 |
| `shmem_ptr` on any memory | the ordering contract is what makes a pointer usable; a pointer with no contract is a race |

### 4.8 Collectives

```c
oc_status oc_sync     (oc_team);
oc_status oc_broadcast(oc_team, int root, struct oc_reference buffer, size_t len);
oc_status oc_reduce   (oc_team, struct oc_reference dst, struct oc_reference src, size_t count,
                       enum oc_type, enum oc_operation);
```

All nonblocking. Each is identified by its team and its sequence number in that
team, so nothing depends on bounded skew (L5); each begins, for this PE, once
the PE's earlier operations on the team are complete, a collective being a
fence for its team; and each completes at `oc_quiet(team)`. Each carries a
signature of its arguments, the team's identity, the sequence and the sizes,
compared where members first meet and before data moves (L46). The
implementation's contract, not the API's: the internode phase sits behind a
bound interface with a tree over puts and atomics as the default, so that
SHARP, multicast or triggered operations can replace it (L4); depth O(log N)
between nodes and O(log L) within one; O(1) state per team per node; the shape
chosen per message size (L48: a fan-out of 2 was 1.5 times faster than 4 for a
megabyte on gigabit Ethernet; Träff's circulant at large sizes, v0.2 step 3).

**Why these three.** Arrival, one-to-all and all-to-one-to-all with an
operation are what in-network collective engines offload, SHARP's barrier,
reduce and broadcast, and what Portals' triggered operations build, which is
the native test. `allgather` was in the core in the first draft because a
gather reused from the reduction tree was slow (L43), an argument about how to
build it and not about where it belongs; no in-network engine offloads it, so
it is the 2.0 library's (§7), a ring or recursive doubling over `put_signal`
and `await`. So are `exchange` and its variable-size form, which have real
demand (bale and its conveyors, ISx, the MoE codes; `core-api.md` C10) and are
puts and a sync in every implementation surveyed, and `collect`, an allgather
of sizes and then puts. Scans have no users. Whether exchange belongs in the
core anyway is **Q8**.

| Alternative | Why not, or where open |
|---|---|
| Blocking collectives only | L5: a dead end that has to be redone |
| A handle or mark returned by each collective (the strawman) | the author's direction 6; the team's sequence number identifies it and `quiet` completes it |
| `allgather`, `alltoall` and `alltoallv` in the core | offloaded nowhere, so built in the 2.0 library at no change of cost class; **Q8** |
| Scans, active sets, `_to_all` | no users, or the 1.6 library's work (§8, §10) |
| Collectives above the transport seam (osoe's first decision) | L4: the largest single RoCE loss; what crosses is `arrive(team, epoch)` down and `released(team, epoch)` up |

### 4.9 Failure, lifecycle and diagnostics

- **Statuses, not undefined behaviour.** `DONE`; `NOT_YET`, a deadline passed;
  `PEER_FAILED`, the job is ending and the status says so in band (L32).
  Misuse, a rank outside the team, a reference outside its region, a collective
  out of step, is a named fatal in every build, with a stable identifier, the
  rank, the routine and the offending value in machine-readable form (L11, §9).
- **A peer's death ends the job**, osoe's model: the resource manager owns the
  job. `PEER_FAILED` exists so that the end is reported where the program is
  waiting, not so that the program may continue; a survivable core is **Q9**.
- **Every blocking call publishes what it waits on and on whom**, in an
  introspection segment whose format is part of the specification (L12, L19,
  L29): a stuck job is read, not guessed at.
- **The seam ships its check**: the boundary between the API's layer and the
  transport is a header, and a build step holds the code to it (L17).
- **One host is a cluster, deterministically.** The job's shape is in the
  options (L13), and the reference implementation's scheduler is seedable, so
  a test is a reproducible test (§9).
- **Profiling** is a registered callback table, not weak symbols and a doubled
  ABI.

## 5. A software pipeline on this API

The author's test case. P PEs in S stages of m = P/S; stage s is the block of
ranks [s·m, (s+1)·m), and rank r hands frames to rank r + m. Each hand-off is a
pair team, a progression of two, K frames deep: K sub-teams of the pair, one
per buffer slot, so that the sender can complete frame f's put without waiting
for frames f+1 to f+K−1. Back-pressure is a credit the receiver adds to the
sender's word.

```c
oc_team world;  struct oc_capabilities capabilities;  oc_open(&options, &world, &capabilities);
int P = oc_team_size(world), me = oc_team_rank(world), m = P / S;
int s = me / m;

oc_team stage;  oc_team_split(world, s * m, 1, m, 0, &stage);        /* my stage: a block of m */

/* The pair with my downstream partner, {me, me + m}: rank 0 sends, rank 1 receives.
 * Both partners compute the same rule, so both get the same team; the tag k
 * gives K realms, one per slot. My upstream pair is {me - m, me}. */
oc_team down[K], up[K];
for (int k = 0; k < K; k++) {
    if (s + 1 < S) oc_team_split(world, me,     m, 2, k, &down[k]);
    if (s > 0)     oc_team_split(world, me - m, m, 2, k, &up[k]);
}

/* Symmetric memory: K frame slots in one region, two waitable words in another. */
oc_region bulk, words;  char *slot;  uint64_t *word;
oc_region_create(world, K * FRAME, OC_BULK, &bulk, (void **)&slot);
oc_region_create(world, 2 * sizeof(uint64_t), OC_WAITABLE, &words, (void **)&word);
uint64_t *frames  = &word[0];                     /* frames landed here, counted by my upstream   */
uint64_t *credits = &word[1];                     /* slots freed at my downstream, counted by it */
struct oc_reference slot_ref    = oc_reference_to(bulk, slot),
                    frames_ref  = oc_reference_to(words, frames),
                    credits_ref = oc_reference_to(words, credits);
oc_sync(world); oc_quiet(world, OC_BLOCK);        /* every PE's regions exist before any put */

for (uint64_t f = 0; f < FRAMES; f++) {
    int k = f % K;
    if (s > 0) {                                                        /* take frame f from upstream */
        oc_await(frames_ref, OC_GREATER_EQUAL, f + 1, OC_BLOCK);
        compute(slot + k * FRAME);
        oc_add(up[k], 0, credits_ref, 1, 0);                            /* slot k is free again */
    } else {
        produce(slot + k * FRAME);
    }
    if (s + 1 < S) {                                                    /* hand frame f downstream */
        if (f >= K) {
            oc_await(credits_ref, OC_GREATER_EQUAL, f + 1 - K, OC_BLOCK);   /* its slot k is free there  */
            oc_quiet(down[k], OC_BLOCK);                                    /* my put of frame f - K is done */
        }
        oc_put_signal(down[k], 1, oc_reference_offset(slot_ref, k * FRAME), slot + k * FRAME, FRAME,
                      frames_ref, OC_SIGNAL_ADD, 1, 0);
    }
}
oc_sync(stage); oc_quiet(stage, OC_BLOCK);        /* the stage is done; a stage-wide reduce would go here */
oc_sync(world); oc_quiet(world, OC_BLOCK);
oc_close(world);
```

Three realms, three kinds of synchronization, and no handle:

- **The pair, per slot** (`down[k]`, `up[k]`): the realm in which frame f's put
  is completed by `oc_quiet(down[k])`, which waits for frame f−K's put and
  nothing else. Without sub-teams of the pair a `quiet` on the pair would wait
  for every frame in flight and hold the pipeline to a depth of one; with a
  handle per put it would reach the same depth with a record per frame.
- **The stage** (`stage`): the realm of the stage's own collectives, a reduce
  of its statistics or a sync at the end, which touch nothing in flight on the
  pairs.
- **The world**: the start and the end.

**A remark the example makes.** In this pipeline `oc_quiet(down[k])` is
redundant: the credit for slot k arrives after the receiver consumed frame
f−K, which is after its signal landed, which is fenced behind its data, so the
credit already proves the put complete, and the sender could reuse its source
without the quiet. A pipeline with flow control gets completion for free from
its own protocol. `quiet` earns its place where no acknowledgement comes back:
a halo exchange whose neighbours do not signal, a histogram built of atomics, a
benchmark (L38). A realm per neighbour set is what makes that quiet cheap. The
proposal keeps `quiet` because those cases are real, and says plainly that a
credit protocol is the better pipeline.

**Depth, and the all-to-all.** The corpus's pipelines run two to four deep
when they complete per slot, and its deep rings, 128 tokens in DeepEP and 32
slots in Triton's queue, never complete per slot at all: the receiver returns a
monotone counter (§3.1, item 5). So K sub-teams of a pair is the right shape
for a double or quadruple buffer, and a credit counter for a deep ring. The
case the pair teams do not cover is bale's: an all-to-all in which each PE
wants to know that its puts *to partner d* are complete before it posts to d,
for every d, which is `oc_quiet_to(world, d)` and not P − 1 teams (§4.3).

**Interleaved stages.** Had stage s been the ranks {s + S·j}, a cyclic layout
with the stages' members alternating on each node, the pairs would be {me,
me + 1}, a progression of two with stride one, and the stage a progression with
stride S; everything above holds. What a single progression cannot express is
the two stages *together*, {s + i + S·j : i < 2}, which a collective across a
hand-off would want: that is a two-dimensional progression, and **Q1** asks
about it.

## 6. Threads

The specification's thread model is a second system beside its teams: four
levels, contexts that may fail to be created, a shareable default context whose
quiet must cover other threads' work, and carve-outs for collectives, splits,
private contexts and lock re-entry scattered through the text (§9.2 item 4
promises blanket thread safety; §9.4, §9.10, §9.13 and Annex C take pieces
back). The corpus shows what came of it (§3.2, item 2): contexts in seven
benchmark ports and no application, and MppUtil wrapping every call in a mutex
(§3.3, item 5). The core's model is instead a consequence of its team, derived
from first principles and from how other systems met the same problem, not
from the specification's history.

**The model.**

1. **Memory belongs to PEs; issue belongs to threads.** The symmetric address
   space is per PE, and threads share their PE's part of it and its private
   memory. A thread is an instruction stream that issues operations and waits.
   Nothing at a target knows about threads except the waiter being woken.
2. **A team is one thread's at a time.** The program ensures that no two
   threads use a team at once, for point-to-point operations, collectives and
   quiet alike. Ownership moves through the program's own synchronization, as
   an object that may be handed between threads but never shared. A debug
   build records the owner and names a wrong thread. Concurrency is more
   teams: a thread that communicates takes a sub-team of itself tagged with
   its index (§4.2, **Q3**), and creation is thread-safe.
3. **Completion is the owner's.** `quiet` and `quiet_to` on a team belong to
   the thread that owns it. There is no cross-thread quiet, so no shared
   counters and no protocol between threads.
4. **A collective is one call per PE per team, from the owner.** Threads of a
   PE that hold pieces of a contribution combine them first, with their thread
   library, and the owner calls. Two threads may run collectives on two teams
   at once.
5. **Waiting is per thread, on PE memory, and exact.** Any thread may `await` a
   word of its own PE, several threads at once on the same or different words.
   Only the core's atomics and `put_signal` write waitable words, from any PE
   or any local thread (the specification's §9.11.1 lets a local thread's
   atomic satisfy a wait, and its §3.2 makes a plain store beside an atomic
   undefined), so every writer is seen and every waiter woken. A thread blocked
   in any call blocks nothing else (§9.2 item 3).
6. **Ordering across threads is the host's.** An operation is ordered after
   the issuing thread's earlier stores and after whatever happens-before its
   issue under the C11 model. A `FENCED` operation and `quiet` order the
   calling thread's stores; another thread's stores are ordered by the
   program's synchronization, as the notes to §9.12.1 and §9.12.2 already say.
   The core adds no memory model of its own.
7. **Progress belongs to the runtime.** Operations aimed at a PE complete
   whether or not its threads call the library: §4.1's expectation, made a
   promise. The implementation owns a progress thread where the hardware needs
   one, and declares the cores it takes.
8. **Per-thread state is bounded, declared, and never fails late.** A thread's
   first call makes its record, a wait record and a completion slot for its
   fetching operations; the records a PE may hold is a capability, and
   exhaustion is a named fatal. A thread's sub-team takes a private hardware
   queue if one is free and otherwise shares one behind the implementation's
   own protection; creation never fails, and the capabilities say how many
   private queues exist. There are no thread levels: under rule 2 the
   specification's serialized level is a program using one team, and its
   funneled level is that program with one thread; what `shmem_init` provides,
   and whether a provided level may fall below the requested one, which the
   specification never says, do not arise.
9. **Open and close are one thread's; abort is anyone's.** The thread that
   opened the job closes it, the specification's rule for `finalize` (§9.2).
   `abort` is a request the runtime carries out from any thread (L33).

**Waypoints, none of them SHMEM's.** Rust's ownership: an object may be moved
between threads but not used by two at once, and the compiler enforces it;
1.4's serialized context was this without the enforcement. Single-issuer
queues: io_uring's fastest mode is one ring per thread with one issuer, and
UCX workers, libfabric endpoints, GASNet-EX endpoints and NIC queue pairs all
default to one thread per object and charge for sharing. UPC++'s personas: an
issuer identity that owns the completion of what it issued, with collectives
run from one persona per process. MPI's endpoints proposal, which gave threads
ranks of their own and so made threads addressable, and was not adopted:
memory and arrival belong to processes and issue to threads, and this model
keeps threads out of the address space. CUDA streams and NVSHMEM's initiator
scopes: with thousands of threads the issuer object became a stream, and a
warp or block cooperates to issue one operation, which is the device-side
question below. And MppUtil's and Loom's funnel: thread barrier, one thread's
PE barrier, thread barrier (§3.3).

**Why issue is cheap to share and completion is not.** Two kinds of state:
monotone state, counters that only increase and bits that are only set, is
safe to share through atomics; reset state, equality waits and cleared bits,
is where every protocol lives. Issue is monotone, so several threads could
issue on one queue cheaply where the queue takes several producers, and the
surveyed transports have such queues or share one behind a lock. Completion
accounting is the problem. A completion counter shared by several issuers is
sound only when completions arrive in the order of issue: a quiet that
snapshots the issued count and waits for the counter to reach it is satisfied
by later operations completing early, which NICs do across queue pairs and
rails (`core-api.md` §2.3 and §2.6; osoe's review found the same snapshot rule
unsound for its gets). In-order completion holds for one reliable queue pair
and for one ordered stream, and for nothing wider. So "puts are thread-safe"
is a property of some transports' puts and not of the model, and the model
does not stand on it: one owner per team keeps every reset word single-writer,
and the cost of making one shared object correct, three exhaustive models and
a mutex in osoe's case, is paid by no one.

**On hardware.** A team is one issuer's share of a queue and its counters,
with no lock while one thread owns it: the shape every NIC above provides.
CXI's sixteen counters a domain bound the teams that own hardware counters to
a handful; the rest count completions in software from completion events, one
an operation, as libfabric providers without counters and UCX do anyway. The
per-thread record is a few hundred bytes (osoe's: about 768 on aarch64, 16 a
PE by default). All of it is declared, and none of it fails after start-up.

**In use.** A worker takes `oc_team_split(world, 0, 1, P, worker_index,
&mine)` at start, communicates on it, quiets it, and the program's thread
barrier hands the team sync to one thread: `oc_sync(world); oc_quiet(world)`
by the owner of the world team. That is the funnel every hybrid code writes,
with the thread's team in the place of a context. A library called from
several threads takes its team as an argument and tags its own sub-teams with
a constant of its own.

**Two things left to other layers.** The 1.6 library's shareable default
context has no counterpart here, and §8 and **Q17** say how the library copes.
Collectives with threads as participants, a reduce declared as k calls on this
PE with a local counter gathering the contributions before one inter-PE phase,
is what GPU block-scoped collectives are and would be the first exception to
rule 2; it is left for the device question, where the initiator's scope, a
thread, a warp or a block, has to be an argument of every operation
(stage-setting §7, question 5).

| Alternative | Why not, or where open |
|---|---|
| Thread levels (OpenSHMEM 1.4, MPI) | under rule 2 every level but "any threads" is a program using one team; the specification never says whether a provided level may fall below the requested one; **Q19** |
| Teams shareable by threads, a lock-free issue path and a locked quiet (osoe's shared contexts) | the heavy protocol, paid by every program for the few that share; the corpus's threaded codes use an issuer per thread (§3.2, item 2); **Q17** |
| A mutex around every operation on a shared team (MppUtil's `unsafecomms_lock`) | two locked instructions an operation, and a descheduled holder convoys every thread; acceptable only as the 1.6 library's way with its default context (§8) |
| Threads as members, with ranks of their own (MPI endpoints; Loom's warps) | makes threads addressable, which sank the MPI proposal; the symmetric space and the arrival counts are per PE |
| An implicit current team (MppUtil's stack) | §3.3, item 2; a thread-local convenience above the core |
| Explicit thread registration with the NIC (Cray's extensions; MppUtil) | the sub-team is the registration, and its hardware queue a declared allocation rather than a vendor call |
| Collectives with a participant count per PE | the first exception to rule 2; deferred to the device question |

## 7. OpenSHMEM 2.0: the standard library on the core

The second layer: the functions a complete PGAS program needs, each built from
core calls with no change of cost class, each with one spelling and its cost
stated as the core calls it makes. It is written once and shipped with the
specification, so that a vendor writes the core and nothing else; and a vendor
may replace any function here with a native one whose contract holds, so that
an in-network allgather or a NIC's swap is reachable without being in the core.
The rule that keeps it standard rather than ergonomic: a function enters
because programs cannot do without it, never because it saves typing.

| Function | Built from | Cost in core calls | Why standard |
|---|---|---|---|
| `allocate`, `free` | a deterministic allocator over a region: local, symmetric by contract, no barrier; a debug build carries a digest of allocations on the next collective and names the first divergent call | local | every program carves regions, and one allocator with one contract beats one per program; the specification's `malloc` is a logarithmic collective for nothing |
| `set` | `put_signal` with no data and the set operation | one call | the standalone set of a waitable word, which Triton's kernels do twenty times (§3.2, item 5) |
| `barrier` | `sync` then `quiet` | two calls | the most-called collective by far (§3.2, item 1); **Q6** |
| `allgather` | a ring or recursive doubling over `put_signal` and `await`, then `sync`; never a reduction reused (L43) | O(log P) rounds | ISx, OSU, the 1.6 `fcollect` |
| `exchange`, `exchangev` | puts with `MORE` to every member, the counts by an `allgather` for the variable form, then `sync` | O(P) puts a PE | bale and its conveyors, ISx, the MoE codes (`core-api.md` C10); **Q8** |
| `collect` | an `allgather` of sizes, then puts | as `exchangev` | the 1.6 `collect` |
| swap, and, or, xor, min, max, the 32-bit forms | compare-swap loops, or native where the vendor substitutes (PCIe atomics, mlx5 and UET for swap; Portals, CXI and UET for min and max) | two round trips or more under contention | the atomics programs reach for beyond the intersection; Graph500's min saves four round trips an edge where native; **Q7** |
| `lock`, `unlock` | a queue lock on `compare_swap` and `await`, a word of a region per lock | three atomics and a wait per contended acquire | every PGAS library ships one and a hand-rolled lock hides bugs; one user in the corpus, the UTS benchmark; **Q20** |

**Excluded, named as ergonomics**: typed generics, an optional header outside
both layers with its own failure path (L37); blocking and nonblocking twins; a
thread-local current team in MppUtil's style (§3.3); `split_2d` as a spelled
call, since two splits or the k-dimensional rule (Q1) cover it; scans, which
have no users; `pe_quiet` with a list of PEs, which is a loop of `quiet_to`.

The 1.6 library of §8 builds on both layers.

## 8. OpenSHMEM 1.6 as a library on the core

The stance the talk's backup slides assume, C of the stage-setting's §6, the
core underneath and OpenSHMEM 1.6 implemented on top as the proof that the core
is enough, is assumed here too (**Q14**); the library builds on the core and
on the 2.0 library of §7. What it needs, by the routine families of the usage
survey:

| OpenSHMEM 1.6 | On this API | Cost |
|---|---|---|
| typed RMA and atomics, `p` and `g`, the `_nbi` and `ctx_` forms: most of the 4,857 entry points | generated, a line each: `put`, `get`, `read`, `fetch_add`, `compare_swap`, `add` on the value's bits; 32-bit and bitwise atomics the 2.0 library's derived ones | one core call; the derived atomics several round trips |
| the default context | the world team's realm; under the multiple thread level a mutex per operation on it, since the core has no team two threads may use at once (§6, **Q17**) | nothing below the multiple level; a locked path at it |
| `shmem_ctx_create`; `shmem_team_create_ctx(T)` | a split of the world, or of T, by the identity rule, with a tag the library draws from a per-PE counter, since a context never carries a collective | a counter set |
| `shmem_quiet`, `shmem_ctx_quiet` | `quiet` of the realm | as the core |
| `shmem_fence`, `shmem_ctx_fence` | a "fence pending" bit per realm, which puts `FENCED` on the PE's operations until those issued before the fence are complete | the window state the core declines to keep, paid by the library and O(1) a realm |
| `shmem_pe_quiet` | `quiet` of the realm | over-waits; conforming, as the specification's own note allows |
| `shmem_barrier_all`; the active-set `shmem_barrier` | `sync(world); quiet(world)`; `quiet(world); sync(T); quiet(T)` | as the core |
| `shmem_team_sync` | `sync(T); quiet(T)`: realm T carries only T's collectives, since RMA goes on the default context's realm | as the core |
| `team_split_strided`, `split_2d`, `translate_pe`, `team_my_pe`, `team_n_pes` | `split`, twice for `split_2d`; the collective the specification requires over the parent becomes a `sync` of the parent | a sync a split |
| `SHMEM_TEAM_SHARED`, `shmem_ptr`, `team_ptr` | `node_mates(world)`; `direct` | as the core |
| `put_signal`, `signal_add`, `signal_set`, `signal_wait_until`, `wait_until`, `test` | `put_signal`, `add`, the 2.0 `set`, `await`; the heap created waitable, at the vendor's price | as the core; the price |
| broadcast, reductions, `sync` | the core's three, started, then `quiet` | as the core |
| `fcollect`, `collect`, `alltoall(s)` | the 2.0 library's `allgather`, `collect`, `exchange` and `exchangev` | software, as today |
| scans, active sets, `_to_all` | puts, `sync` and the 2.0 collectives, in the library | software, as today |
| the `malloc` family, statics, locks, sessions, `global_exit` | the 2.0 allocator with a `sync` a call; a linker script; the 2.0 lock; `MORE`; `abort` | cold paths |

Run on it, the SOS test suite and the reading corpus would show whether the
core is enough; the library is the test.

## 9. For AI-assisted development

The stage-setting's §5 hypothesis, as a checklist against the items:

1. **Contracts are local.** Every call names the team it acts in; there is no
   default context and no implicit world (§4.1, §4.2), and a team's owner is
   one known thread (§6).
2. **One way to do each thing, and regular names.** `oc_<object>_<verb>`;
   arguments always team, rank, destination, source, length, flags; no blocking
   and nonblocking spellings (rule 2); no typed cross product (§4.4).
3. **Misuse fails loudly, identically and machine-readably** (§4.9).
4. **Cost declared.** Each routine's header states its class (local, one-sided,
   collective), its round trips and its complexity in N and L; the capabilities
   say which atomics are native and what a team costs (§4.5, §4.2).
5. **Semantics small enough to ship as an executable model.** Four rules and
   twenty-five core calls; a reference model of order, completion and arrival,
   published with the specification, lets a tool check a program's use of the
   API as osoe's models checked the library (**Q15**).
6. **Deterministic on one host** (§4.9).
7. **Blocking has a deadline and a reason** (rule 1).

## 10. Deliberately absent

From the core and the 2.0 library alike: contexts; handles, marks and requests;
typed routines (an optional header outside both layers); blocking and
nonblocking variants; a standalone fence; `pe_quiet` with a list; symmetric
statics; `shmem_ptr` on arbitrary memory; strided and blocked transfers; vector
and multi-word waits; scans; active sets and `_to_all`; sessions; implicit
finalize; thread levels; an implicit current team; deprecated names; reserved
environment-variable syntax; undefined behaviour. From the core alone, built in
the 2.0 library instead: the allocator, the standalone `set`, `barrier`,
`allgather`, exchange, `collect`, the derived atomics and the lock.

## 11. Questions and push-backs for the author

Each with a recommendation, so that "as recommended" is an answer.

**On teams**

1. **One progression, or a nested k-dimensional one?** A single
   `(start, stride, count)` cannot express a Cartesian plane of a 3-D grid
   (MPI's `Cart_sub` keeps any subset of the axes; OpenSHMEM's `split_2d` keeps
   one), nor two interleaved pipeline stages together (§5), nor "the lead of
   every node" when nodes differ in size. A rule `start + Σ i_d × stride_d`,
   `0 ≤ i_d < count_d`, with the strides nested (`stride_(d+1) ≥ stride_d ×
   count_d`), keeps membership and translation O(k) by successive division and
   is still a rule, never a table; k = 1 is the common case and `split_2d` the
   k = 2 one. *Recommendation:* allow it, k ≤ 4, as the one `split` taking a
   rule; the 1-D form is the rule with k = 1. The cost is a loop of k where
   there was one multiply.
2. **Creation without communication asks only that the members agree on the
   rule and the tag.** With the tag (Q18), identity no longer depends on the
   order of splits, so the specification's rules that threads must not split
   one parent at once and that creation events occur in the same order on
   every PE (§9.4, §9.10) fall away. What remains: a member that never creates
   the team is found at the first collective by a deadlock detector, not at
   creation, where OpenSHMEM's collective split finds it, at a logarithmic
   cost a split with every parent member calling. *Recommendation:*
   communication-free creation; the pipeline's pair teams and the per-thread
   sub-teams are the case for it, and a debug build can `sync` a new team at
   creation to catch the mismatch early.
3. **A sub-team of itself per thread**, tagged with the thread's index, gives
   PEs with different thread counts different sets of sub-teams, harmless for
   point-to-point and an error for a collective on one that some PE never
   made. Accept, or add a flag `OC_TEAM_LOCAL` that forbids
   collectives and so needs no agreement, which is a context under another
   name? *Recommendation:* accept; one kind of team.
4. **Marks.** The strawman's mark is a counter value, no object and no list; on
   a NIC with a counter per endpoint it is a scalar and cheap; on transports
   that complete per peer it is a vector (§4.3), so its cost is the
   transport's, which is the portability form of the author's objection. *Recommendation:* no marks; sub-teams do the job; revisit only
   if a workload shows unplanned partial completion that teams cannot be shaped
   for.
5. **"Team", or another word?** The object is a set of PEs that is also the
   scope of this PE's order and completion to them. Candidates: *team*,
   OpenSHMEM's word for the same membership rule, so a port is mechanical and
   reviewers arrive holding it, but it carries no completion meaning there and
   a reader may miss the addition; *group*, MPI's, membership only, the same
   problem; *communicator*, MPI's membership plus context, the closest existing
   concept, but long and two-sided in connotation; *realm*, the author's word
   for the scope, new and without baggage, odd for membership ("rank 3 of the
   realm"); *scope* and *domain*, overloaded (a NUMA domain, an atomicity
   domain, a lexical scope); *lane*, osoe's per-thread buffer, taken; *cohort*,
   *circle*, *set*, unfamiliar or too generic. *Recommendation:* **team**, with
   "realm" kept for prose about the completion role ("a team is the realm of
   its operations"), since the realm meaning is a strict addition to what every
   OpenSHMEM team already is, and `oc_quiet(team)` reads as what it does.
   Second choice: *realm* as the type's name, if the author wants the semantic
   shift signalled in every call.

**On order, completion and arrival**

6. **`barrier` as a spelled inline over `sync` then `quiet`, or only the two
   calls?** The barrier is about nine hundred call sites in the classic
   applications against six for every other synchronization routine together
   (§3.2, item 1). *Recommendation:* `barrier` in the 2.0 library as exactly
   the two (§7): the hottest collective should be one line, and the core stays
   at twenty-five.
7. **The derived atomics**, swap, and, or, xor, min, max and 32-bit: in the
   core with their tier declared, or in the 2.0 library? Swap is native on
   PCIe atomics, mlx5 and Ultra Ethernet and absent from stock IBTA; min and
   max would save Graph500 four round trips an edge where native.
   *Recommendation:* the 2.0 library (§7), each routine's cost in its comment
   and a vendor free to substitute a native one; the core stays the
   intersection.
8. **Exchange** (`alltoall`, `alltoallv`): in the core or in the 2.0 library?
   Real demand (bale, ISx, the MoE codes), offloaded nowhere. *Recommendation:*
   the 2.0 library (§7), on `put` with `MORE` and `sync`, with `allgather` and
   `collect` beside it; a vendor substitutes an in-network one if it ever
   exists.
9. **Peer failure.** The core says a dead peer ends the job and `PEER_FAILED`
   reports it where a PE is waiting. A survivable core would need every team to
   tolerate a missing member, which changes every collective.
   *Recommendation:* the job ends; keep the status so that the end is in band
   (L32) and the door is not nailed shut.

**On surface and scope**

10. **Regions created over a team other than the world**: the team's own,
    existing on its members and addressable by them alone, or always
    job-wide? bale's SHMEM conveyors refuse every communicator but the world
    because they cannot allocate symmetric memory for a team (§3.1, item 8).
    *Recommendation:* the team's own, as §4.7 now says; a region's identity
    derives from its team's, so the members agree on it with no message.
11. **The signal of `put_signal`: an add alone, or a set as well?** The
    first draft of this proposal said add alone, one way. The survey found
    the GPU codes set an epoch and compare for equality, 66 sets to 4 adds in
    Triton (§3.2, item 5), and a set is a plain write, native everywhere and
    cheaper than an add. *Recommendation:* both, as the specification and
    NVSHMEM have them, since they are two things: an add counts producers, a
    set announces a value; and a standalone `set` in the 2.0 library (§7).
12. **A wait for any of several words.** L45 found the collective engine needs
    it; no application in the corpus does. *Recommendation:* out, until a
    program shows the need; producers add to one word.
13. **The world team as the job, with no `oc_job` object.** *Recommendation:*
    yes.
14. **The compatibility stance**: still C, the core plus the 1.6 library (§8)?
    *Recommendation:* yes; the library is the proof that the core is enough,
    and the corpus is its test suite.
15. **An executable reference model in the specification** (order, completion,
    arrival, waiting) and a deterministic one-host mode in the reference
    implementation: in scope for the next revision? *Recommendation:* yes to
    both; they are what made osoe checkable.

**Added from the corpus survey**

16. **`quiet_to`: a quiet toward one member of a team.** The author asked for
    completion by team alone. The survey found that the one subset programs
    reach for inside an all-to-all is per destination, met today by a full
    quiet (bale) or by going under the API (DeepEP, GIN), and that a sub-team
    per partner would be P − 1 teams a PE (§3.1, item 4; §4.3). It is a scope,
    not a handle, and O(1) wherever completion is per peer. *Recommendation:*
    include it, in the core's native tier; without it the proposal has no answer
    for bale's conveyors but the full quiet they pay today.

**On threads**

17. **The 1.6 library's shareable default context.** The core has no team two
    threads may use at once (§6, rule 2), so the library on top must implement
    `SHMEM_CTX_DEFAULT` under the multiple level some other way: a mutex per
    operation on the world team's realm, correct and convoy-prone; a read-only
    quiet the core would allow on a team another thread owns, completing what
    happens-before the call by reading the owner's monotone counters and
    clearing nothing, which the specification's own notes make sufficient
    (§9.12.2: another thread's operations are covered only when the program
    ordered them before the quiet); or a shareable team mode in the core,
    carrying osoe's lock-free protocol as a declared cost. *Recommendation:*
    the mutex, in the library, since its threaded users are benchmarks; the
    read-only quiet noted as the upgrade if a user appears; no shareable mode
    in the core.
18. **A tag on the identity-rule split**, supplied by the program, in place of
    an implicit count of earlier splits. Identity becomes the program's intent,
    creation becomes thread-safe and order-free, and a duplicate live identity
    is a named fatal; the cost is one argument on every split, zero where a
    rule has one team. *Recommendation:* the tag.
19. **No thread levels.** Rule 2 makes the serialized and funneled levels
    programs that use one team, and the single level a program with one
    thread; the per-thread record is made at first use and bounded by a
    capability. The alternative is two levels, one letting an implementation
    skip the per-thread lookup. *Recommendation:* none; the lookup is one
    thread-local load, and a level that may be provided below what was asked,
    which the specification leaves open, is a trap an API meant for assisted
    development should not keep.

**On the layers**

20. **The lock in the 2.0 library.** A queue lock on `compare_swap` and `await`
    is a page, every PGAS library ships one, and a hand-rolled lock hides bugs;
    the corpus has one user, the UTS benchmark. *Recommendation:* keep it in
    the 2.0 library and mark the doubt; drop it if the author would rather the
    library carry nothing without an application user.

**On memory**

21. **The creation-time discipline: a rule or a recommendation?** Creating a
    team's regions at its creation, before sub-teams and before returning to
    other teams, keeps the pool's top end a tree and fragmentation low (§4.7).
    As a rule it forbids a later phase's allocation on the world and a
    library's allocation on entry. *Recommendation:* a recommendation, checked
    in debug builds, with the arena agreement carrying correctness either way.

## 12. What follows the answers

The proposal is revised from them: §2's table, the signatures and §5's example
settle, and the open rows of the alternatives tables close or become decisions.
Then, in order: the executable model of §2's four rules, small enough to
publish; the capabilities structure and the header comments' cost declarations;
and the two libraries' feasibility checked against the corpus, a grep of what
every program calls against §7's recipes and §8's table, before any code.
