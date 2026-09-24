/*
 * transport.h - the boundary between the SHMEM layer and whatever carries
 * operations to other PEs (design §4.10).
 *
 * The SHMEM layer (§4), and the library around it (§5, §6), call only what is
 * declared here and in internode.h, its sibling for the internode phase of a
 * collective. They never call the AM layer (§2.1) directly, so a different
 * backend - RDMA, say - replaces the implementation of these two headers without
 * touching anything above them. The shmem.boundary test holds every file above
 * to that: it fails the build's tests if one of them includes, however
 * indirectly, a header of the backend below (cmake/CheckBoundary.cmake lists both
 * sides).
 *
 * Addressing. Every location is (pe, region, offset): a world rank (the SHMEM
 * layer translates team-relative ranks before calling), a region
 * that exists in every PE, and a byte offset from that region's start in the
 * target PE (§4.1, §4.2, invariant 20). Never an address: a PE's heap sits at
 * a different address in each sibling process, and the wire never carries
 * one (invariant 7). The SHMEM layer computes offset = addr - base of the
 * region in the calling PE, and checks bounds before calling; the transport
 * does not re-check them.
 *
 * Backends. Each call is dispatched on the single locality function (§8.1):
 *   - intranode: the target's heap is mapped in this process (§4.1); every
 *     operation is a memory operation in the calling thread;
 *   - AM: everything else (§3; not in phase 1 step 1).
 * Backends are bound at build time: no table of function pointers, and a
 * backend's fast path may become static inline behind these names (§4.10).
 *
 * Dispatch is per (pe, operation class), not per PE. A backend may carry one
 * PE's puts and gets while another carries that PE's rmw - a NIC whose atomics
 * are not coherent with the CPU must, since a CPU atomic and a NIC atomic on
 * one word are then not exclusive, and rmw below promises exclusivity across
 * the whole atomicity domain. So osoe_transport_direct returning non-NULL says that
 * *stores* through the pointer are ordered as described; it does not say that
 * rmw to that PE executes locally. Nothing above this header changes: the spec
 * already makes a plain store racing an AMO on one word undefined (spec §3.2).
 *
 * Capabilities. Every operation is mandatory for every backend, and nothing
 * above this header asks what a backend supports. A backend without native
 * support emulates underneath (§4.10).
 *
 * Threads. Phase 1 provides up to SHMEM_THREAD_SERIALIZED (§4.6): at most one
 * thread of a PE is inside these calls at a time, though it need not be the
 * same thread each time. Nothing here is safe to call concurrently.
 * SHMEM_THREAD_MULTIPLE is open (§10).
 *
 * Errors. There is no error return. A transport failure aborts the job
 * (§3.15). That is the right contract because something outside the job - the
 * resource manager, or the launcher - owns tearing it down; a boundary that
 * returned recoverable errors would be inventing a job it does not own.
 *
 * Every blocking call here checks the abort word and the lead's liveness at
 * least every OSOE_SWEEP_NS (base/deadline.h), whether or not it takes a
 * deadline. A backend must not spin or block indefinitely without sweeping:
 * osoe_transport_wait, a blocking osoe_transport_get and any future blocking call sweep on
 * the same schedule as osoe_transport_watch_wait, which is the difference between a
 * hang and a clean abort. A blocking call also publishes the "blocked in"
 * record (§3.14) naming what it waits on, and gives up only by §3.15's
 * no-progress rule - never because time has passed. The AM backend's four
 * waits do all three (transport-am.c, struct am_wait), and shmem.unswept-wait
 * and shmem.stopped-node.* are the tests that would see one stop.
 */
#ifndef OSOE_TRANSPORT_H
#define OSOE_TRANSPORT_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Regions, numbered the same in every PE. */
enum osoe_region {
    OSOE_REGION_HEAP = 0,   /* the PE's symmetric heap (§4.1) */
    OSOE_REGION_INTERNAL,   /* library scratch and collective control words
                             * (§5.8); never visible to the application */
    /* OSOE_REGION_STATICS arrives with §4.3 phase 1.5 */
    OSOE_REGION_COUNT
};

/* Per-context transport state: ordering window, dirty set (§4.6). Owned by
 * the backend and embedded in the SHMEM layer's context. The default
 * context's state is created by osoe_transport_init; creation for other contexts
 * arrives with shmem_ctx_create (§7.3). */
struct osoe_transport_context;

/* An absolute CLOCK_MONOTONIC instant (§3.10), from base/. */
struct osoe_deadline;

/* ---- Lifecycle ------------------------------------------------------- */

/*
 * Called once per session by the first shmem_init, after the node and job
 * rendezvous and heap mapping, and by the last shmem_finalize, after its
 * barrier and after the node's progress thread has been joined (§6.4). A
 * re-init after fini is a fresh session.
 *
 * init starts whatever the backend needs - for AM, the connection area, the
 * wires and the progress engine the agent thread will run - and it must be
 * complete before it returns, because the progress thread starts next and polls
 * it. fini takes the rest down once that thread has been joined. What this node
 * still owes the network is drained before then, on the thread, by
 * osoe_transport_agent_exit (§6.4: a node must not stop its engine while it owes
 * a peer bytes).
 */
void osoe_transport_init(void);
void osoe_transport_fini(void);

struct osoe_transport_context *osoe_transport_context_default(void);

/*
 * Ready what another node's lead needs to reach this node, and describe it in
 * `blob`: OSOE_JOB_BACKEND_BLOB_BYTES long (job.h), zeroed. Called by this
 * node's lead during the job rendezvous, before osoe_transport_init, and only in
 * a job of more than one node. The rendezvous carries every node's blob to every
 * node (osoe_job_backend_blob) - out of band, as §6.2 says an RDMA backend's
 * queue-pair and key material must travel. AM listens for its node-pair
 * connections here and writes the listener's port. This is the call the job
 * rendezvous's blob was left waiting for "with the backend that needs one" (job.h
 * at step 2); it adds to the seam rather than changing it.
 */
void osoe_transport_advertise(void *blob);

/* ---- The progress thread (§3.12) ------------------------------------- */

/*
 * Every PE starts one agent thread, and the library lends it to the backend
 * through these three calls and nothing else (agent.c). They are the boundary's
 * progress call, which §10 once recorded as unnecessary on the ground that
 * every place the SHMEM layer blocks is a backend function. That was true of
 * application threads and false of the node: the barrier blocks on an epoch
 * counter outside any backend (base/epoch.h), and what moves it is the node's
 * progress thread, which is the library's and not the backend's.
 *
 * pass: one turn of whatever the backend runs on this thread - on AM, one pass
 * of the lead's engine over every connection, and the internode arrivals posted
 * to it (internode.h). Returns true if anything moved, which is what tells the
 * agent whether it may sleep (§3.10). A backend with nothing to run here returns
 * false, as AM does on every PE but the node's lead.
 *
 * wait: sleep until the agent's wake word no longer holds `seen`, until the
 * backend has work for this thread, or until at most OSOE_SWEEP_NS has passed.
 * Returns true if it was woken rather than timed out, which the agent counts
 * (§3.14). The caller loads its wake word into `seen` **before** calling pass,
 * so that work posted during the pass fails the compare and the agent does not
 * sleep on it (§4.9); agent.c does, and says so.
 *
 * The backend owns the wait because only the backend can wait on its own events
 * and the wake word in one call. §3.10's lead blocks in one io_uring submission
 * carrying both its socket completions and an IORING_OP_FUTEX_WAIT on the word;
 * an RDMA backend sleeping on a completion channel is the same shape. Until
 * phase 1 step 3 this was `idle_ns` - how long agent.c's own futex_wait may
 * sleep - which is the step-2 stand-in's shape and no ring's (a finding about
 * the seam, recorded in §10).
 *
 * exit: the library takes the thread back. Called once, on the agent thread,
 * after its last pass and before it returns. The backend finishes whatever it
 * runs there - on AM, what this node still owes the network (§6.4) and the
 * close of every connection - because what the thread owns only the thread can
 * finish: a ring created with IORING_SETUP_SINGLE_ISSUER refuses every other
 * thread's submissions (-EEXIST), and when its submitter exits, its armed
 * receives are cancelled with the bytes left in the socket (both measured on
 * 6.12). A backend with nothing to finish returns at once.
 *
 * Called only on the agent thread, never concurrently with each other, and
 * only between osoe_transport_init and osoe_transport_fini.
 */
bool osoe_transport_agent_pass(void);
bool osoe_transport_agent_wait(_Atomic uint32_t *wake, uint32_t seen);
void osoe_transport_agent_exit(void);

/*
 * abort: the node's abort word is set and this process is about to end
 * (§3.15, abort.h). The backend tells the network what only it can - on AM,
 * the lead sends each peer an abortive goodbye, so that the job's other nodes
 * learn the abort and its status before they see this node's streams end - and
 * returns once it has, or once OSOE_ABORT_GRACE_SECONDS have passed. Called on
 * the agent thread, instead of exit, and at most once.
 */
void osoe_transport_agent_abort(void);

/* ---- Locality -------------------------------------------------------- */

/*
 * The address of (pe, region, offset) in this process, or NULL if the
 * target is not reachable by ordinary loads and stores. Non-NULL only where
 * the backend orders the calling thread's stores through this address
 * against its own writes by the next fence or wait (§4.6). osoe's intranode
 * backend does, since its writes apply in the calling thread; the AM backend
 * does not, so every PE it carries - including a local PE under the
 * force-remote flag (§8.1) - gets NULL. This is the one locality function:
 * shmem_ptr, shmem_team_ptr and shmem_addr_accessible consult it and nothing
 * else.
 */
void *osoe_transport_direct(int pe, enum osoe_region region, uint64_t offset);

/* ---- Data operations ------------------------------------------------- */

/* Flags for put and rmw. */
enum {
    /*
     * Place this operation at the target only after every write issued
     * earlier on the same (context, pe) has been placed. put-with-signal is a
     * put followed by an rmw carrying this flag, so the signal never
     * announces bytes that have not landed (invariant 5). Free on backends
     * whose placement is already in issue order, which both of osoe's are.
     */
    OSOE_TRANSPORT_FENCED = 1u << 0,

    /* rmw only: the caller needs the previous value, so the call returns
     * only once the operation has executed. Without it, an rmw may return
     * before it executes (spec §9.7). */
    OSOE_TRANSPORT_FETCH = 1u << 1,

    /*
     * More operations follow on this context immediately: a backend may hold
     * this one back and submit it with the next. Purely a hint. It changes
     * nothing about completion, ordering, or when src may be reused, and a
     * backend that ignores it is correct - which is how the intranode backend
     * implements it, since every operation is applied before it returns.
     * Any fence, wait, get, fetching rmw, or operation without this flag
     * submits whatever is held.
     *
     * A backend may defer an operation only if it has taken its own copy of
     * the source bytes. Put's contract is unconditional - src may be reused
     * the moment the call returns - so deferring a large zero-copy put is
     * wrong however the flag is set. That rule is the flag's whole safety
     * argument, and it is the backend's to keep.
     *
     * **Its first caller is put_signal** (§4.10, and signal.c), which is the
     * one routine that can set it honestly in this step: the signal follows the
     * put on the same context and to the same PE, so "more follows" is a fact
     * rather than a guess. The intranode path still ignores it, correctly -
     * there is nothing to defer when every put is a memcpy applied before the
     * call returns.
     *
     * On the AM path what it defers is the *wake*, not the seal, and that is
     * now what it does rather than what it would do. §3.9's aggregation
     * self-clocks off the send completion, so an operation on an idle
     * connection is sealed and submitted at once and nothing waits for a buffer
     * to fill; the only thing an application thread controls is whether it asks
     * the lead to run (§3.12). So the saving is futex syscalls on a burst, the
     * bytes are copied and committed before the call returns either way, and a
     * held wake costs at most one safety-sweep interval because §3.10 has the
     * lead submit a sealed buffer regardless of signalling. am.wakes_deferred
     * counts them, since a flag whose whole effect is to change nothing
     * observable needs a counter to be testable at all.
     *
     * It exists for a backend that posts a list of work requests under one
     * doorbell rather than ringing one per put, which is where the small-put
     * message rate is won - and that is also the backend for which the
     * source-bytes rule above binds, since such a request references the
     * caller's src rather than a copy. Its drivers are the 1.6 session routines
     * (§7.6), the strided and block RMA loops, and the collectives' own loops.
     * The strided put loop is its second caller (rma-typed.c): every element
     * but the last.
     */
    OSOE_TRANSPORT_MORE = 1u << 2,
};

/*
 * Put: write len bytes from src to (pe, region, offset).
 *
 * Returns at local completion: src may be reused at once. The bytes are
 * applied at the target at some later time, and are complete and visible to
 * every PE once an osoe_transport_wait covering this operation returns. Bytes of one
 * put may land in any order, and two puts may land out of order (spec
 * §9.6.1.1). A put is not atomic with an rmw on the same bytes; the spec
 * makes that mix undefined (spec §3.2).
 *
 * Ordering: none between writes to the same PE except as fences provide -
 * OSOE_TRANSPORT_FENCED, or an osoe_transport_fence between them.
 *
 * Notification: best effort (see osoe_transport_notify).
 *
 * Intranode: memcpy into the target's heap mapping, then osoe_transport_notify.
 * Applied before return.
 */
void osoe_transport_put(struct osoe_transport_context *context, int pe, enum osoe_region region,
                 uint64_t offset, const void *src, size_t len, unsigned flags);

/*
 * Get: read len bytes from (pe, region, offset) into dst.
 *
 * Blocking, and complete per operation: dst holds the bytes when the call
 * returns, whether dst is private or symmetric memory. The bytes may be
 * sampled at any moment between the call and its return; a get racing a
 * write to the same bytes is undefined (§4.6 deferred sampling). Gets are
 * never fenced (spec §9.12.1).
 *
 * Intranode: memcpy out of the target's heap mapping.
 * AM: the reply lands in this PE's landing zone and the calling thread copies
 * it out (§3.12).
 *
 * `flags`: none is defined for get yet, and every caller passes 0. The
 * parameter exists so that the two that are coming do not change a signature
 * every backend implements (roce-seam F9): OSOE_TRANSPORT_MORE, which a gather
 * loop or a strided get sets so that a backend posting work requests can ring
 * one doorbell for the batch; and the nonblocking flag below. A backend must
 * accept flags it does not act on, exactly as for put.
 *
 * The nonblocking form, completed by osoe_transport_wait, arrives with shmem_get_nbi.
 * It is a flag on this call, not a separate one, and it yields no handle: a
 * nonblocking operation is completed by the existing osoe_transport_wait and nothing
 * else. The spec defines _nbi completion only at quiet, so there is no
 * per-operation wait call to serve, §4.6's bytes_issued/bytes_landed pair is
 * already this design, and the core-API study found counters sufficient in all
 * six production transports it surveyed. Recorded here before a benchmark
 * applies pressure for per-operation handles.
 */
void osoe_transport_get(struct osoe_transport_context *context, int pe, enum osoe_region region,
                 uint64_t offset, void *dst, size_t len, unsigned flags);

/* Read-modify-write operations on one word. */
enum osoe_rmw_op {
    OSOE_RMW_WRITE,     /* t = operand                                    */
    OSOE_RMW_READ,      /* no change; with OSOE_TRANSPORT_FETCH                  */
    OSOE_RMW_CSWAP,     /* if (t == compare) t = operand                  */
    OSOE_RMW_ADD,       /* t += operand, modulo 2^(8 * width)             */
    OSOE_RMW_AND,       /* t &= operand                                   */
    OSOE_RMW_OR,        /* t |= operand                                   */
    OSOE_RMW_XOR,       /* t ^= operand                                   */
};

/*
 * rmw: apply op to the width-byte word at (pe, region, offset), width 4 or 8.
 * Returns the word's previous value if flags has OSOE_TRANSPORT_FETCH, else 0.
 *
 * Precondition: offset is a multiple of width within the region; the object
 * is of an AMO type, so C alignment already guarantees it.
 *
 * Applied exactly once, never replayed. Exclusive, as spec §3.2 requires,
 * against every other rmw of the same width on the same word issued through
 * contexts in the same atomicity domain - the domain of context's team - and
 * against waiters and tests on that word. Nothing more is promised: not
 * across domains, widths, or against puts, gets, and plain loads and stores.
 * osoe's intranode and AM backends happen to be exclusive across domains too
 * (§4.1); callers must not rely on it, so that a backend may execute one
 * domain on a NIC and another on the CPU.
 *
 * Notification: exact. Every applied rmw, by any path, notifies.
 *
 * Completion: with OSOE_TRANSPORT_FETCH, complete per operation on return. Without
 * it, as a put.
 *
 * Intranode: one C11 atomic operation on the shared mapping (§3.16: CAS and
 * fetch_add name seq_cst, a plain write is a seq_cst store), then
 * osoe_transport_notify. The progress thread applies a remote rmw with the same
 * instruction on the same cache line, so the two unify (§4.1).
 */
uint64_t osoe_transport_rmw(struct osoe_transport_context *context, int pe, enum osoe_region region,
                     uint64_t offset, enum osoe_rmw_op op, unsigned width,
                     uint64_t operand, uint64_t compare, unsigned flags);

/*
 * Load: the width-byte word at (my PE, region, offset), width 2, 4 or 8,
 * zero-extended. For shmem_test*, the wait_until predicate, and
 * shmem_signal_fetch - every place the SHMEM layer reads its own variable
 * that remote atomics write.
 *
 * Precondition: offset is a multiple of width, as for rmw.
 *
 * Coherent: exclusive against every rmw of the same width on the word (spec
 * §3.2), so the value is one some rmw left there, never a torn mix. A backend
 * whose NIC applies rmw out of the CPU's sight synchronizes here.
 *
 * Intranode and AM: one acquire load. Every writer is a CPU atomic on the
 * shared mapping (§4.1), and a load of a word no larger than a pointer is a
 * single instruction on every target.
 */
uint64_t osoe_transport_load(enum osoe_region region, uint64_t offset, unsigned width);

/* ---- Range attributes (§4.4, §4.10) ---------------------------------- */

/* What a range of this PE's own memory is used for. */
enum {
    /* rmw may target these bytes: spec §9.3.5's SHMEM_MALLOC_ATOMICS_REMOTE. */
    OSOE_TRANSPORT_RANGE_ATOMICS = 1u << 0,
    /* signals live here: SHMEM_MALLOC_SIGNAL_REMOTE, so put_signal's rmw half
     * and the signal waits of §4.9 land in it. */
    OSOE_TRANSPORT_RANGE_SIGNAL  = 1u << 1,
};

/*
 * Tell the backend that (region, offset, len) of *this PE's own* memory carries
 * the named attributes. The allocator calls it for every hinted allocation
 * (§4.4); nothing else does.
 *
 * Advisory in both directions. A backend may ignore it entirely, an operation
 * on bytes outside any declared range is still correct, and nothing above this
 * header asks what a backend did with it - which is what keeps it from becoming
 * the capability query §4.10 rejects. The intranode backend ignores it, because
 * every operation is already a memory operation on a mapping it has.
 *
 * **Not a collective, and it must not block.** Every PE calls it with identical
 * arguments in the same order as a consequence of the allocator's determinism
 * (§4.4, invariant 6), never by coordination. A backend that put a barrier here
 * would be synchronizing for no reason.
 *
 * **Repeated or overlapping calls take the union, never a replacement**, so
 * that shmem_realloc preserving an allocation's hints (spec §9.3.3) survives a
 * grown or moved block. There is deliberately no call to withdraw a range:
 * freeing a block does not un-declare its bytes, because over-declaring is
 * safe - the attribute is a statement about what may happen there, and a
 * backend that registered the range keeps a registration it did not need. A
 * withdrawal would instead have to be ordered against operations still in
 * flight toward those bytes, which is a synchronization this call must not have.
 *
 * It exists for a backend that must prepare memory before the network can
 * touch it: an RDMA backend registers only the ranges that need NIC atomics or
 * counters rather than the whole heap, and without it cannot know which words a
 * PE may wait on, so it cannot afford exact notification and every blocked wait
 * degenerates to polling (§4.10).
 */
void osoe_transport_set_range(enum osoe_region region, uint64_t offset, uint64_t len,
                       unsigned attributes);

/* ---- Ordering and completion ----------------------------------------- */

/*
 * Fence: every write issued on context to any PE before this call is placed at
 * its target before any write issued on context to the same PE after it (spec
 * §9.12.1). On contexts without SHMEM_CTX_NOSTORE, also makes the calling
 * thread's earlier memory stores visible to other cores (§4.6 rule 2); a
 * NOSTORE context may skip that. Orders delivery; does not wait for
 * completion, and need not send anything (§4.10).
 *
 * Intranode: one release fence; writes are already placed.
 * AM: the release fence, then seal and flush the dirty set with ack
 * requested; the dirty set is kept (§4.6).
 */
void osoe_transport_fence(struct osoe_transport_context *context);

/*
 * Wait: return once every write and nonblocking get issued on context - to the
 * listed PEs, or to every PE if pes is NULL - is complete, and visible to
 * every PE; and, on contexts without SHMEM_CTX_NOSTORE, the calling thread's
 * earlier memory stores are visible to other cores (spec §9.12.2, §9.12.3). With pes non-NULL and npes 0, returns
 * at once. shmem_quiet and shmem_pe_quiet; shmem_barrier_all begins with it
 * on the default context (§5.6).
 *
 * May over-wait: a PE list may wait on gets to every PE (§4.6).
 *
 * Intranode: one serializing fence; nothing is ever outstanding.
 * AM: the serializing fence, flush the dirty set restricted to the listed
 * PEs' nodes, wait for acknowledgements and bytes_landed == bytes_issued,
 * copy out completed gets, clear what was waited on (§4.6).
 */
void osoe_transport_wait(struct osoe_transport_context *context, const int *pes, size_t npes);

/* ---- Notification (§4.9) --------------------------------------------- */

/* A published watch: one per waiting thread. */
struct osoe_transport_watch;

/*
 * Waiter side. A PE waits only on its own symmetric variables. One watch
 * covers a byte range, so the multiple-element waits (spec §9.11.2-§9.11.7),
 * whose variables are one array, publish one watch:
 *
 *     struct osoe_transport_watch *w = osoe_transport_watch(region, offset, len);
 *                                               // BEFORE the first check
 *     for (;;) {
 *         uint32_t seq = osoe_transport_watch_seq(w);  // BEFORE loading variables
 *         if (predicate(variables)) break;
 *         osoe_transport_watch_wait(w, seq, &sweep_deadline);
 *     }
 *     osoe_transport_unwatch(w);
 *
 * Wakes may be spurious; the loop re-checks. osoe_transport_watch and
 * osoe_transport_unwatch publish with seq_cst stores (§3.16: two-sided).
 * osoe_transport_watch_wait returns false on a deadline expiry, which the caller
 * counts (§4.9). Under SHMEM_THREAD_SINGLE a PE holds at most one watch.
 */
struct osoe_transport_watch *osoe_transport_watch(enum osoe_region region, uint64_t offset,
                                    size_t len);
uint32_t osoe_transport_watch_seq(struct osoe_transport_watch *w);
bool osoe_transport_watch_wait(struct osoe_transport_watch *w, uint32_t seq,
                        const struct osoe_deadline *deadline);
void osoe_transport_unwatch(struct osoe_transport_watch *w);

/*
 * Writer side, called by backends, never by the SHMEM layer: after applying
 * a write to (pe, region, offset, len), wake pe's waiters whose watched range
 * overlaps it.
 *
 * Exact for rmw. The waiter stores its watch and then loads the variable;
 * the writer stores the variable and then loads the watch. That is a
 * two-sided protocol (§3.16): both stores and both loads are seq_cst, an rmw's
 * locked instruction counting as the store.
 *
 * Best effort for puts. A backend that sees a put calls this straight after
 * the copy, with no serializing fence: on x86 a memcpy's stores can still be
 * in the store buffer when the watch is loaded, and the wake can be lost. The
 * waiter's safety-sweep deadline bounds the loss. Spec 1.5 change #17
 * requires wait variables to be updated with AMOs, so only a non-conforming
 * program, or the deprecated short waits that have no AMO type, can see the
 * delay. A backend whose puts the CPU never sees (RDMA) need not notify for
 * puts at all; for rmw it bridges its NIC events into this call.
 */
void osoe_transport_notify(int pe, enum osoe_region region, uint64_t offset, size_t len);

#endif /* OSOE_TRANSPORT_H */
