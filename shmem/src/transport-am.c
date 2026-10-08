/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The transport boundary (§4.10): dispatch on locality, and the part of the
 * transport every job uses.
 *
 * This file owns §4.10's entry points. Each operation goes one of two ways on
 * the answer §8.1's single locality function gives: a PE reachable by ordinary
 * loads and stores takes the intranode path (transport-local.h) and is applied
 * in the calling thread; every other PE goes to the internode backend
 * (internode-backend.h) - on AM, the far half of §3's layer
 * (transport-internode.c).
 *
 * **That is one backend with two paths, not two backends.** §4.10 chooses
 * backends at build time and has no table of function pointers, and this keeps
 * both properties: the branch below is on where a PE *is*, which is a fact
 * about the job rather than a configuration, and it is the same branch
 * osoe_transport_direct already answers for shmem_ptr. A job of one logical
 * node never takes the far path at all, and enters the internode backend only
 * for the check every PE makes before its node joins and through internode.h's
 * four calls every job makes (internode-backend.h): what its operations run is
 * this file and the intranode path.
 *
 * What is here: the contexts - their pool in the node segment, their modes,
 * and the mutex a shared one's waits take (§4.11) - and the fence and wait of
 * a job of one node, which are the calling thread's fences alone. In a job of
 * more than one node a context also has the internode backend's state, its
 * dirty set (§4.6), and its fence and wait are the internode backend's.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "am-configuration.h"
#include "base/deadline.h"
#include "base/environment.h"
#include "base/futex.h"
#include "base/order.h"
#include "fatal.h"
#include "internode-backend.h"
#include "job.h"
#include "metrics.h"
#include "node.h"
#include "perturb.h"
#include "state.h"
#include "transport.h"
#include "transport-context.h"
#include "transport-local.h"
#include "wire-settings.h"

/*
 * Whether the job spans more than one logical node: settled at the very start
 * of osoe_transport_init, before anything that reads it. It was once set inside
 * the engine's own start-up, *after* the per-context ordering state was sized
 * from it, so the first internode put wrote through a null dirty set.
 */
static bool internode;

/*
 * Every context this PE may have, OSOE_MAX_CONTEXTS of them, element i owning
 * block i of this PE's pool in the node segment (§4.11; phase 1 step 8).
 * Element 0 is the default context, live from init to fini; the others are
 * live between osoe_transport_context_create and _destroy, and an element's
 * block is claimed and released with it. The pool's state words are the
 * truth about which blocks are taken - a compare-and-swap, as §4.11 item 4
 * says, though below MULTIPLE nothing contends for them.
 */
static struct osoe_transport_context *contexts;
static uint32_t context_count;

/* Make element `index` live on its block, which the caller has claimed: in a
 * job of more than one node, with the internode backend's state for it. */
static bool context_open(struct osoe_transport_context *c, uint32_t index, unsigned options)
{
    *c = (struct osoe_transport_context){ .block = index, .options = options, .live = true,
                                          .concurrent =
                                              (options & OSOE_TRANSPORT_CONTEXT_CONCURRENT) != 0 };
    pthread_mutex_init(&c->mutex, NULL);
    return !internode || osoe_internode_backend_context_open(c);
}

void osoe_transport_threads(bool concurrent)
{
    contexts[0].concurrent = concurrent;
}

/*
 * Two test hooks for a shared context's protocols (phase 1 step 8 slice 3;
 * docs/decisions/2026-09-26-evening.md item 12): OSOE_CHECK_QUIET, which the
 * internode backend's waits check their promises by (transport-internode.c),
 * and OSOE_TEST_PERTURB, whose points the protocols' steps share
 * (perturb.h). Read here, by every PE of every job, so that a bad value is
 * refused whatever the job's shape. Off, each is one load of a flag a point.
 * Neither changes what is correct.
 */
static bool environment_flag(const char *name)
{
    uint32_t v = 0;
    if (osoe_environment_u32(name, &v) == OSOE_ENVIRONMENT_INVALID || v > 1)
        osoe_fatal("%s='%s' is not 0 or 1", name, getenv(name));
    return v == 1;
}

/*
 * transport.h: what the backend needs before the node joins. First whether
 * the internode backend can run a job of this many nodes here - on AM, the
 * io_uring a job of more than one node opens - which every local PE answers
 * alike, so a refusal tells no one: the refusal's last word is disarmed before
 * the fatal, as init's futex check is, since a lead's would leave its node a
 * tombstone in /dev/shm for nothing (node.h). The transport, not a backend,
 * keeps that rule. Then the settings, as values, in every build and whatever
 * the job's shape, so that a bad one is refused the same way on one node and
 * on many: the AM layer's sizes, which lay out every PE's landing zone in the
 * node segment in any job (node.c), and the wire's.
 */
void osoe_transport_settings_check(const struct osoe_bootstrap *b)
{
    char why[512];
    if (osoe_internode_backend_check(b, why, sizeof why)) {
        osoe_node_refusal_disarm();
        osoe_fatal("%s", why);
    }
    struct osoe_am_configuration am;
    osoe_am_configuration_read(&am);
    osoe_wire_settings_check(b);
}

/*
 * §4.10's lifecycle. The one fact about the job comes first, because the
 * contexts below are opened by it; then, in a job of more than one node, the
 * internode backend, which must be complete before this returns - the agent
 * thread starts next and polls it - and which the default context then opens
 * on.
 */
void osoe_transport_init(void)
{
    internode = osoe.job.node_count > 1;
    bool check_quiet = environment_flag("OSOE_CHECK_QUIET");
    osoe_perturb_init();

    context_count = osoe.node.contexts_per_pe;
    contexts = calloc(context_count, sizeof *contexts);
    if (!contexts)
        osoe_fatal("out of memory for %u contexts", context_count);
    /* The default context's block is block 0, claimed here as any other is
     * claimed at creation. The segment is this session's, made zeroed by the
     * lead, so the claim can fail only on a segment that is not. */
    uint32_t expected = OSOE_CONTEXT_FREE;
    struct osoe_context_block *b = osoe_node_context(&osoe.node, osoe.bootstrap.local_id, 0);
    if (!atomic_compare_exchange_strong_explicit(&b->state, &expected, OSOE_CONTEXT_LIVE,
                                                 memory_order_seq_cst, memory_order_seq_cst))
        osoe_fatal("this PE's context block 0 is already live in a fresh control segment");
    if (internode)
        osoe_internode_backend_start(osoe.node.hdr->session, check_quiet);
    if (!context_open(&contexts[0], 0, 0))
        osoe_fatal("out of memory for the ordering state of %u peer nodes",
                   osoe.job.node_count - 1);
}

/* transport.h: the link is known; only a job of more than one node has
 * connections to size by it. */
void osoe_transport_link(uint64_t bytes_per_second)
{
    if (internode)
        osoe_internode_backend_link(bytes_per_second);
}

/* After the agent has been joined (transport.h). The internode backend
 * drained what this node owed the network and closed its connections on that
 * thread, in its agent exit; what is left is memory. */
void osoe_transport_fini(void)
{
    if (internode)
        osoe_internode_backend_stop();
    /* Every created context was destroyed before the finalize barrier
     * (context.c), so only the default one is live here; a block left live
     * would be a defect, and the segment goes with the session either way. */
    for (uint32_t i = 0; i < context_count; i++)
        if (contexts[i].live) {
            pthread_mutex_destroy(&contexts[i].mutex);
            if (contexts[i].internode)
                osoe_internode_backend_context_close(&contexts[i]);
            atomic_store_explicit(&osoe_transport_context_block(&contexts[i])->state,
                                  OSOE_CONTEXT_FREE, memory_order_release);
            contexts[i].live = false;
        }
    free(contexts);
    contexts = NULL;
    context_count = 0;
    internode = false;
}

/* §6.2's blob: the internode backend's, since only a job of more than one node
 * rendezvouses (job.c). */
void osoe_transport_advertise(void *blob)
{
    osoe_internode_backend_advertise(blob);
}

/*
 * The agent thread, lent to the backend (transport.h, §3.12). A job of one
 * node has nothing of the internode backend's to run on it: a pass moves
 * nothing, and the wait is a plain futex wait on the agent's wake word, which
 * ends by a sweep interval at the latest (§4.10).
 */
bool osoe_transport_agent_pass(void)
{
    return internode && osoe_internode_backend_agent_pass();
}

bool osoe_transport_agent_wait(_Atomic uint32_t *wake, uint32_t seen)
{
    if (internode)
        return osoe_internode_backend_agent_wait(wake, seen);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    return osoe_futex_wait(wake, seen, &sweep, OSOE_FUTEX_SHARED) == 0;
}

void osoe_transport_agent_exit(void)
{
    if (internode)
        osoe_internode_backend_agent_exit();
}

void osoe_transport_agent_abort(void)
{
    if (internode)
        osoe_internode_backend_agent_abort();
}

/* A job of one node has no network, and is idle and quiet with nothing
 * applied (transport.h). */
bool osoe_transport_agent_node_idle(uint64_t *applied)
{
    if (internode)
        return osoe_internode_backend_node_idle(applied);
    *applied = 0;
    return true;
}

bool osoe_transport_agent_node_quiet(uint64_t *applied)
{
    if (internode)
        return osoe_internode_backend_node_quiet(applied);
    *applied = 0;
    return true;
}

void osoe_transport_agent_deadlock_send(uint32_t node, const struct osoe_deadlock_message *m)
{
    if (!internode)
        osoe_fatal("a deadlock detector message toward node %u from a node with no network",
                   node);
    osoe_internode_backend_deadlock_send(node, m);
}

struct osoe_transport_context *osoe_transport_context_default(void)
{
    return &contexts[0];
}

/*
 * transport.h: a context from this PE's pool. The first free block from 1 up
 * is claimed by a compare-and-swap on its state word - O(OSOE_MAX_CONTEXTS)
 * per creation, a setup-time cost, and the segment's words rather than a
 * process-memory free list so that the pool has one truth (§4.11 item 4).
 * NULL when every block is live or the internode backend's memory for it is
 * short; a claim whose memory then fails is given back.
 */
struct osoe_transport_context *osoe_transport_context_create(unsigned options)
{
    for (uint32_t i = 1; i < context_count; i++) {
        struct osoe_context_block *b = osoe_node_context(&osoe.node, osoe.bootstrap.local_id, i);
        uint32_t expected = OSOE_CONTEXT_FREE;
        if (!atomic_compare_exchange_strong_explicit(&b->state, &expected, OSOE_CONTEXT_LIVE,
                                                     memory_order_seq_cst, memory_order_seq_cst))
            continue;
        struct osoe_transport_context *c = &contexts[i];
        if (context_open(c, i, options))
            return c;
        c->live = false;
        atomic_store_explicit(&b->state, OSOE_CONTEXT_FREE, memory_order_release);
        return NULL;
    }
    return NULL;
}

/* transport.h: the implicit wait of spec §9.5.3, then the block and the
 * memory go back. The counters in the block are cumulative and never reset,
 * so a later claimant of the block starts from equal counters, whatever they
 * count to. */
void osoe_transport_context_destroy(struct osoe_transport_context *c, const char *routine)
{
    osoe_transport_wait(c, NULL, 0, routine);
    pthread_mutex_destroy(&c->mutex);
    if (c->internode)
        osoe_internode_backend_context_close(c);
    c->live = false;
    atomic_store_explicit(&osoe_transport_context_block(c)->state, OSOE_CONTEXT_FREE,
                          memory_order_release);
}

/*
 * §8.1's single locality function, seen from the boundary (§4.10).
 *
 * Non-NULL says this backend orders the calling thread's stores through the
 * address against its own writes by the next fence or wait (§4.6), which is
 * true exactly of the intranode path, since it applies every write in the
 * calling thread. So the answer is NULL for a PE on another logical node -
 * whose heap this process never mapped, since PEs rendezvous only with the PEs
 * in their own node's OSOE_GTIDS - and also for a node-mate under §8.1's
 * force-remote flag, which denies the pointer without changing the route.
 *
 * **The route is the other predicate** (job.h): the operations above dispatch on
 * osoe_routes_in_process, so under the flag a put to a node-mate is still a
 * memcpy in this thread. One predicate answering both questions is what sent
 * that put to a connection that does not exist (§8.1).
 *
 * shmem_ptr and shmem_team_ptr consult this and nothing else (§4.8);
 * shmem_addr_accessible does not, and must not - accessibility is reachability
 * through OpenSHMEM, which every PE of the job has.
 */
void *osoe_transport_direct(int pe, enum osoe_region region, uint64_t offset)
{
    if (pe < 0 || !osoe_direct_access(&osoe.job, (uint32_t)pe))
        return NULL;
    return osoe_transport_local_address(pe, region, offset);
}

/* ---- §4.10's data operations ------------------------------------------- */

void osoe_transport_put(struct osoe_transport_context *context, int pe, enum osoe_region region,
                 uint64_t offset, const void *src, size_t len, unsigned flags)
{
    if (osoe_routes_in_process(&osoe.job, (uint32_t)pe)) {
        osoe_transport_local_put(pe, region, offset, src, len, flags);
        return;
    }
    osoe_count(OSOE_C_TRANSPORT_PUT_CALLS, 1);
    osoe_count(OSOE_C_TRANSPORT_PUT_BYTES, len);
    /* No lock on a shared context: issue is lock-free (§4.11 item 4). */
    osoe_internode_backend_put(context, pe, region, offset, src, len, flags);
}

void osoe_transport_get(struct osoe_transport_context *context, int pe, enum osoe_region region,
                 uint64_t offset, void *dst, size_t len, unsigned flags)
{
    if (osoe_routes_in_process(&osoe.job, (uint32_t)pe)) {
        /* Applied before it returns, nonblocking or not: complete early, which
         * a nonblocking get may be (transport.h). */
        osoe_transport_local_get(pe, region, offset, dst, len);
        return;
    }
    osoe_count(OSOE_C_TRANSPORT_GET_CALLS, 1);
    osoe_count(OSOE_C_TRANSPORT_GET_BYTES, len);
    osoe_internode_backend_get(context, pe, region, offset, dst, len, flags);
}

uint64_t osoe_transport_rmw(struct osoe_transport_context *context, int pe, enum osoe_region region,
                     uint64_t offset, enum osoe_rmw_op op, unsigned width,
                     uint64_t operand, uint64_t compare, unsigned flags)
{
    if (osoe_routes_in_process(&osoe.job, (uint32_t)pe))
        return osoe_transport_local_rmw(pe, region, offset, op, width, operand, compare,
                                        flags);
    osoe_count(OSOE_C_TRANSPORT_RMW_CALLS, 1);
    if (flags & OSOE_TRANSPORT_FETCH)
        osoe_count(OSOE_C_TRANSPORT_RMW_FETCHING, 1);
    return osoe_internode_backend_rmw(context, pe, region, offset, op, width, operand, compare,
                                      flags);
}

/* ---- Ordering and completion (§4.6) ------------------------------------ */

/*
 * §4.6: fence orders writes becoming visible. A fence comes first on both
 * paths, and it is what puts the calling thread's ordinary memory stores
 * under fence and quiet as Table 14 requires (§4.6): a release fence here,
 * since fence orders stores against stores, and the serializing fence in
 * wait, since quiet also orders them against later loads (base/order.h). It is
 * not decoration: on the intranode path it is the entire implementation. In a
 * job of more than one node the internode backend then does what a fence owes
 * the network.
 *
 * No lock on either mode: a shared context's fence takes none (§4.11 item 2).
 */
void osoe_transport_fence(struct osoe_transport_context *context)
{
    osoe_count(OSOE_C_TRANSPORT_FENCE_CALLS, 1);
    /* Also on a NOSTORE context: spec §9.5 lets fence skip ordering the
     * application's own stores there, but on the intranode path this fence is
     * also what orders the context's puts, each a memcpy in this thread
     * (base/order.h), and those it must still order (transport.h). */
    osoe_release_fence();
    if (internode)
        osoe_internode_backend_fence(context);
}

/*
 * §4.10's wait: complete every operation this context has outstanding, to every
 * PE or to the named ones. The spec's quiet and pe_quiet are both this call
 * (§4.6), which is why the boundary has a wait rather than three routines.
 *
 * In a job of one node nothing is ever outstanding - every operation took the
 * intranode path and was applied before its call returned - so a wait is the
 * serializing fence that orders the calling thread's stores (§4.6), on a
 * NOSTORE context too, for fence's reason above, and on a shared context with
 * no mutex: there is no pair of get counters to close (§4.11 item 1), nor any
 * dirty set to clear. The counter says that nothing was outstanding rather
 * than being omitted, because in a job of more than one node the same name
 * means something quite different.
 *
 * Nothing on the project's machines fails without the fence: the counters
 * around it are each an acquire load and a release store - ldar and stlr at
 * GCC's default -march - which keep a store before a later load in aarch64's
 * hardware. Built with -march=armv8.3-a, whose acquire loads are ldapr, a
 * store-buffering litmus through shmem_quiet finds the forbidden outcome
 * without it, and on x86-64 its mfence is all that orders the two (the v0.2
 * step 1 review, correctness K2; a test is on the release checklist).
 *
 * `pes == NULL` means every PE; a non-NULL list with npes == 0 has already
 * returned in quiet.c, where the spec says it (§9.12.3).
 */
void osoe_transport_wait(struct osoe_transport_context *context, const int *pes, size_t npes,
                         const char *routine)
{
    if (internode) {
        osoe_internode_backend_wait(context, pes, npes, routine);
        return;
    }
    if (pes && npes == 0)
        return;
    osoe_count(OSOE_C_TRANSPORT_WAIT_CALLS, 1);
    osoe_serializing_fence();
    osoe_count(OSOE_C_TRANSPORT_WAIT_NOTHING_OUTSTANDING, 1);
}

/* In a job of one node no nonblocking get is ever in flight: each was applied
 * before its call returned (transport.h). */
void osoe_transport_wait_gets(struct osoe_transport_context *context, const char *routine)
{
    if (internode)
        osoe_internode_backend_wait_gets(context, routine);
}

/* ---- The two entry points that are this backend's, not a path's --------- */

/*
 * These are the backend's own, whichever path an operation takes, so they live
 * in the file a second backend replaces rather than in transport-local.c,
 * which a second backend keeps: an RDMA backend has its own set_range, and a
 * definition in the shared intranode file would collide with it at link time
 * (second review, roce-seam F9).
 */

/*
 * Every wait's predicate load (wait.c, signal.c). It follows the waiter's
 * seq_cst publication of its watch, and the writer's half is a seq_cst store or
 * rmw followed by notify's load of that watch: a two-sided protocol, so this
 * load is seq_cst (§3.16). Acquire is not enough - C11 does not order a seq_cst
 * store before a later acquire load, and GCC for Armv8.3 and later (a Pi 5's
 * Cortex-A76) emits it as ldapr, which passes the stlr: measured, 289 to 4,449
 * lost-wake outcomes in 2 million rounds (fence-litmus/store-buffer-litmus).
 * A seq_cst load is an ldar on every aarch64 target and a plain mov on x86-64.
 */
uint64_t osoe_transport_load(enum osoe_region region, uint64_t offset, unsigned width)
{
    /* This PE's own word: the address, not the pointer the application may
     * have (transport-local.h). A PE is direct to itself either way, and this
     * says so without depending on it. */
    void *ptr = osoe_transport_local_address((int)osoe.bootstrap.rank, region, offset);
    switch (width) {
    case 8:
        return atomic_load_explicit((_Atomic uint64_t *)ptr, memory_order_seq_cst);
    case 4:
        return atomic_load_explicit((_Atomic uint32_t *)ptr, memory_order_seq_cst);
    default:
        return atomic_load_explicit((_Atomic uint16_t *)ptr, memory_order_seq_cst);
    }
}

/*
 * Nothing to do: every operation on this backend is a memory operation on a
 * mapping the process already holds, so no range needs preparing (§4.10). The
 * call is still made by the allocator, because the contract is the backend's to
 * ignore, not the caller's to skip.
 */
void osoe_transport_set_range(enum osoe_region region, uint64_t offset, uint64_t len,
                       unsigned attributes)
{
    (void)region;
    (void)offset;
    (void)len;
    (void)attributes;
    osoe_count(OSOE_C_TRANSPORT_SET_RANGE_CALLS, 1);
}
