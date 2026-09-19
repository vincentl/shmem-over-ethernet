/*
 * transport.h - the boundary between the SHMEM layer and whatever carries
 * operations to other PEs (design §4.10).
 *
 * The SHMEM layer (§4) calls only what is declared here. It never calls the AM
 * layer (§2.1) directly, so a different backend - RDMA, say - replaces the
 * implementation of this header without touching §4.
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
 * hang and a clean abort. Neither osoe_transport_wait nor osoe_transport_get blocks on the
 * intranode backend, so nothing exercises this yet; it is stated here because
 * the first backend whose wait blocks will be written against this header.
 */
#ifndef OSOE_TRANSPORT_H
#define OSOE_TRANSPORT_H

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

/* Called once per session by the first shmem_init, after node-local
 * rendezvous and heap mapping, and by the last shmem_finalize, after its
 * barrier (§6.4). A re-init after fini is a fresh session. */
void osoe_transport_init(void);
void osoe_transport_fini(void);

struct osoe_transport_context *osoe_transport_context_default(void);

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
     * earlier on the same (ctx, pe) has been placed. put-with-signal is a
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
     * No caller sets this yet, and both of osoe's backends ignore it.
     *
     * On the intranode backend there is nothing to defer: every put is a memcpy
     * applied before the call returns. On the AM backend what it would defer is
     * the *wake*, not the seal - §3.9's aggregation self-clocks off the send
     * completion, so an operation on an idle connection is sealed and submitted
     * at once and nothing waits for a buffer to fill; the only thing an
     * application thread controls is whether it asks the lead for a wake
     * (§3.12). The saving is futex wakes on a burst, and ignoring the flag
     * costs at most one safety-sweep interval, because §3.10 requires a sealed
     * buffer to be submitted regardless of signalling.
     *
     * It exists for a backend that posts a list of work requests under one
     * doorbell rather than ringing one per put, which is where the small-put
     * message rate is won - and that is also the backend for which the
     * source-bytes rule above binds, since such a request references the
     * caller's src rather than a copy. Its drivers are the 1.6 session routines
     * (§7.6), the strided and block RMA loops, and the collectives' own loops.
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
void osoe_transport_put(struct osoe_transport_context *ctx, int pe, enum osoe_region region,
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
 * The nonblocking form, completed by osoe_transport_wait, arrives with shmem_get_nbi.
 * It is a flag on this call, not a separate one, and it yields no handle: a
 * nonblocking operation is completed by the existing osoe_transport_wait and nothing
 * else. The spec defines _nbi completion only at quiet, so there is no
 * per-operation wait call to serve, §4.6's bytes_issued/bytes_landed pair is
 * already this design, and the core-API study found counters sufficient in all
 * six production transports it surveyed. Recorded here before a benchmark
 * applies pressure for per-operation handles.
 */
void osoe_transport_get(struct osoe_transport_context *ctx, int pe, enum osoe_region region,
                 uint64_t offset, void *dst, size_t len);

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
 * contexts in the same atomicity domain - the domain of ctx's team - and
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
uint64_t osoe_transport_rmw(struct osoe_transport_context *ctx, int pe, enum osoe_region region,
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
 * Fence: every write issued on ctx to any PE before this call is placed at
 * its target before any write issued on ctx to the same PE after it (spec
 * §9.12.1). On contexts without SHMEM_CTX_NOSTORE, also makes the calling
 * thread's earlier memory stores visible to other cores (§4.6 rule 2); a
 * NOSTORE context may skip that. Orders delivery; does not wait for
 * completion, and need not send anything (§4.10).
 *
 * Intranode: one serializing store; writes are already placed.
 * AM: the serializing store, then seal and flush the dirty set with ack
 * requested; the dirty set is kept (§4.6).
 */
void osoe_transport_fence(struct osoe_transport_context *ctx);

/*
 * Wait: return once every write and nonblocking get issued on ctx - to the
 * listed PEs, or to every PE if pes is NULL - is complete, and visible to
 * every PE; and, on contexts without SHMEM_CTX_NOSTORE, the calling thread's
 * earlier memory stores are visible to other cores (spec §9.12.2, §9.12.3). With pes non-NULL and npes 0, returns
 * at once. shmem_quiet and shmem_pe_quiet; shmem_barrier_all begins with it
 * on the default context (§5.6).
 *
 * May over-wait: a PE list may wait on gets to every PE (§4.6).
 *
 * Intranode: one serializing store; nothing is ever outstanding.
 * AM: the serializing store, flush the dirty set restricted to the listed
 * PEs' nodes, wait for acknowledgements and bytes_landed == bytes_issued,
 * copy out completed gets, clear what was waited on (§4.6).
 */
void osoe_transport_wait(struct osoe_transport_context *ctx, const int *pes, size_t npes);

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
 * two-sided protocol (§3.16), and an rmw's locked instruction or seq_cst
 * store makes the writer's side sound.
 *
 * Best effort for puts. A backend that sees a put calls this straight after
 * the copy, with no serializing store: on x86 a memcpy's stores can still be
 * in the store buffer when the watch is loaded, and the wake can be lost. The
 * waiter's safety-sweep deadline bounds the loss. Spec 1.5 change #17
 * requires wait variables to be updated with AMOs, so only a non-conforming
 * program, or the deprecated short waits that have no AMO type, can see the
 * delay. A backend whose puts the CPU never sees (RDMA) need not notify for
 * puts at all; for rmw it bridges its NIC events into this call.
 */
void osoe_transport_notify(int pe, enum osoe_region region, uint64_t offset, size_t len);

#endif /* OSOE_TRANSPORT_H */
