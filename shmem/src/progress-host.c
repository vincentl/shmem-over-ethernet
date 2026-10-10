/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The node's progress engine, as a PE runs it (§3.12, §5.3).
 *
 * See progress-host.h for what this is and why it is separate from progress.c.
 */
#define _GNU_SOURCE
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "base/deadline.h"
#include "base/environment.h"
#include "base/epoch.h"
#include "base/futex.h"
#include "base/stall-watch.h"
#include "base/sweep.h"
#include "base/wait-word.h"
#include "abort.h"
#include "agent.h"
#include "am-state.h"
#include "blocked.h"
#include "connection-area.h"
#include "deadlock.h"
#include "fatal.h"
#include "internode.h"
#include "job.h"
#include "listener.h"
#include "metrics.h"
#include "node.h"
#include "owner-map.h"
#include "progress.h"
#include "progress-host.h"
#include "state.h"
#include "transport.h"
#include "wire.h"

struct osoe_am_state osoe_am;

/* ---- the four host callbacks (progress.h) ------------------------------ */

/*
 * (pe, region, offset) as an address in this process, or NULL if this node
 * does not hold that PE. Every local heap is mapped in every process of the
 * node (§4.1), so for a local PE it is pointer arithmetic.
 *
 * The bounds check is not defensive tidiness: a message naming an offset past
 * the end of a region is a peer that disagrees about the job - a different
 * SHMEM_SYMMETRIC_SIZE, or a different build - and writing it would corrupt
 * whatever follows the heap rather than say so (§3.15).
 */
static void *resolve(void *context, uint32_t pe, uint16_t region, uint64_t offset,
                     uint32_t length)
{
    (void)context;
    if (region != OSOE_REGION_HEAP && region != OSOE_REGION_INTERNAL &&
        region != OSOE_REGION_STATICS)
        osoe_fatal("a peer addressed region %u, which this build does not carry; "
                   "the nodes are running different libraries", region);
    if (pe >= osoe.bootstrap.world_size)
        osoe_fatal("a peer addressed PE %u in a job of %u", pe,
                   osoe.bootstrap.world_size);
    int32_t local = osoe_node_local_id(&osoe.node, pe);
    if (local < 0)
        return NULL;
    if (region == OSOE_REGION_INTERNAL) {
        /* The library's own region, the tail of the same mapping
         * (internal-region.h): sized by the fan-out, the pipeline's depth,
         * the small collectives' bytes, the team count and the job's node
         * count, which every node agrees on and every PE of a node, and by the
         * thread count, which every PE of a node agrees on (node.c) - and the
         * target is this node's - so this PE's size is the target's. */
        if (offset > osoe.node.internal_bytes || length > osoe.node.internal_bytes - offset)
            osoe_fatal("a peer addressed %u bytes at offset %llu of PE %u's internal "
                       "region, which is %llu bytes; the nodes disagree about "
                       "OSOE_COLLECTIVE_FANOUT, OSOE_COLLECTIVE_PIPELINE, "
                       "OSOE_SMALL_COLLECTIVE_BYTES or OSOE_MAX_TEAMS, "
                       "or run different libraries",
                       length,
                       (unsigned long long)offset, pe,
                       (unsigned long long)osoe.node.internal_bytes);
        return (char *)osoe.node.heap_of[local] + osoe.node.internal_offset + offset;
    }
    if (region == OSOE_REGION_STATICS) {
        /* The program's statics (§4.3), which every PE of the job has alike if
         * it runs one program linked by oshcc. */
        uint64_t bytes = osoe.node.statics_bytes;
        if (offset > bytes || length > bytes - offset)
            osoe_fatal("a peer addressed %u bytes at offset %llu of PE %u's symmetric "
                       "statics, which are %llu bytes; the nodes run different programs, "
                       "or one not linked with oshcc", length,
                       (unsigned long long)offset, pe, (unsigned long long)bytes);
        return osoe_node_statics_at(&osoe.node, (uint32_t)local,
                                    (uint32_t)local == osoe.bootstrap.local_id, offset);
    }
    if (offset > osoe.node.heap_size || length > osoe.node.heap_size - offset)
        osoe_fatal("a peer addressed %u bytes at offset %llu of PE %u's heap, which is "
                   "%llu bytes; the nodes disagree about SHMEM_SYMMETRIC_SIZE",
                   length, (unsigned long long)offset, pe,
                   (unsigned long long)osoe.node.heap_size);
    return (char *)osoe.node.heap_of[local] + offset;
}

/* The local id of `pe`, or -1: a reply for a PE not on this node is refused. */
static int32_t local_of(uint32_t pe)
{
    return pe < osoe.bootstrap.world_size ? osoe_node_local_id(&osoe.node, pe) : -1;
}

/*
 * A local PE's get `token`, blocking or not, in flight: found by the extent
 * its token names, and only if the record there holds that very token and is
 * still in flight - a reply to a get already landed or copied out, or to none,
 * is refused (progress.h). The piece's bytes are the extents from there.
 */
static unsigned char *landing_request(void *context, uint32_t pe, uint64_t token,
                                      uint32_t *length)
{
    (void)context;
    int32_t local = local_of(pe);
    uint64_t index = token & OSOE_LANDING_EXTENT_MASK;
    if (local < 0 || index >= osoe.node.landing_requests)
        return NULL;
    struct osoe_landing_record *r = &osoe_node_landing_records(&osoe.node, (uint32_t)local)[index];
    if (atomic_load_explicit(&r->state, memory_order_acquire) != OSOE_LANDING_ISSUED ||
        atomic_load_explicit(&r->token, memory_order_acquire) != token)
        return NULL;
    uint32_t bytes = atomic_load_explicit(&r->length, memory_order_acquire);
    if ((uint64_t)index * OSOE_LANDING_EXTENT_BYTES + bytes > osoe.node.landing_arena_bytes)
        return NULL;
    *length = bytes;
    return osoe_node_landing_arena(&osoe.node, (uint32_t)local) +
           (size_t)index * OSOE_LANDING_EXTENT_BYTES;
}

/*
 * A nonblocking get's reply has landed whole (landing_request has checked it).
 * Its record goes to LANDED first, with release, so that a second reply naming
 * it is refused and any of the PE's threads may now claim it for copying out
 * (transport-internode.c); then the context block the token names counts its bytes -
 * the value half of base/wait-word.h's two-sided wait, a seq_cst add, woken
 * only if a thread waits there - and the zone's `freed` word is bumped too,
 * since a thread waiting for room may copy this reply out and make some. The
 * add also releases the reply's bytes, which the parser wrote before this
 * call, to the thread that reads the counter and then them.
 */
static void landed(void *context, uint32_t pe, uint64_t token, uint32_t bytes)
{
    (void)context;
    int32_t local = local_of(pe);
    uint32_t block = (uint32_t)((token >> OSOE_LANDING_OWNER_SHIFT) & OSOE_LANDING_OWNER_MASK);
    if (local < 0 || block >= osoe.node.contexts_per_pe)
        osoe_fatal("a nonblocking get's reply for PE %u names context block %u of %u: its "
                   "token was accepted, so this node made it, and it made none past its pool",
                   pe, block, osoe.node.contexts_per_pe);
    struct osoe_landing_record *r =
        &osoe_node_landing_records(&osoe.node, (uint32_t)local)[token & OSOE_LANDING_EXTENT_MASK];
    /* seq_cst: the value of two wait-word waits, the context's and the zone's
     * `freed`, whose wakes are skipped when nobody is registered
     * (base/wait-word.h; review of slice 2, S1). */
    atomic_store_explicit(&r->state, OSOE_LANDING_LANDED, memory_order_seq_cst);
    struct osoe_context_block *c = osoe_node_context(&osoe.node, (uint32_t)local, block);
    atomic_fetch_add_explicit(&c->bytes_landed, bytes, memory_order_seq_cst);
    osoe_wait_word_wake_all(&c->counter_wake);
    osoe_wait_word_wake_all(&osoe_node_landing(&osoe.node, (uint32_t)local)->freed);
}

/*
 * A blocking get's reply, or a fetching AMO's value, for the thread the token
 * names (§4.11 item 5): refused unless that thread armed its slot for this very
 * token (§3.4's trust boundary; second review, D4). A get's record goes to
 * LANDED_BLOCKING, which no other thread claims, so that a duplicate is
 * refused; then the length or value, then `ready`, then the wake - the thread
 * loads the wake word before the flag (§4.6), so a bump between its load and
 * its wait returns at once instead of sleeping stale.
 */
static bool completed(void *context, uint32_t pe, uint64_t token, uint32_t length,
                      uint64_t value)
{
    (void)context;
    int32_t local = local_of(pe);
    uint32_t thread = (uint32_t)((token >> OSOE_LANDING_OWNER_SHIFT) & OSOE_LANDING_OWNER_MASK);
    if (local < 0 || thread >= osoe.node.threads_per_pe)
        return false;
    struct osoe_thread_record *t = osoe_node_thread(&osoe.node, (uint32_t)local, thread);
    if (atomic_load_explicit(&t->armed_token, memory_order_acquire) != token)
        return false;
    uint64_t index = token & OSOE_LANDING_EXTENT_MASK;
    if (index != OSOE_LANDING_NO_EXTENT) {
        if (index >= osoe.node.landing_requests)
            return false;
        atomic_store_explicit(&osoe_node_landing_records(&osoe.node, (uint32_t)local)[index].state,
                              OSOE_LANDING_LANDED_BLOCKING, memory_order_release);
    }
    atomic_store_explicit(&t->length, length, memory_order_release);
    atomic_store_explicit(&t->value, value, memory_order_release);
    atomic_store_explicit(&t->token, token, memory_order_release);
    atomic_store_explicit(&t->ready, 1, memory_order_release);
    atomic_fetch_add_explicit(&t->wake, 1, memory_order_seq_cst);
    osoe_futex_wake(&t->wake, INT_MAX, OSOE_FUTEX_SHARED);
    return true;
}

/* §4.9's watch half. The same function the intranode backend calls, for the
 * reason §4.1 gives about one mechanism rather than two that agree. */
static void notify(void *context, uint32_t pe, uint16_t region, uint64_t offset,
                   uint32_t length)
{
    (void)context;
    osoe_transport_notify((int)pe, (enum osoe_region)region, offset, length);
}

/*
 * A team the engine meets for the first time (progress.h): its record in its
 * block on this node (node.h), loaded as it was published - `published` with
 * acquire, then the rest (§3.16) - and turned into the engine's terms: the
 * node numbers its messages go to, and the team's name on each of those nodes,
 * its number and the local id there of its local lead, rather than the
 * neighbours' leads' ranks the record keeps (the team redesign's step D); and
 * the block's generation, which the engine learnt it at.
 * False for a team with no record here, or one spanning a single node: the
 * engine then ends the job naming it, as it did an unregistered team. The
 * record's counts are this library's own, but they are read from shared
 * memory, so a child count past the fan-out is refused rather than followed.
 */
static bool team_of_record(void *context, uint32_t team, struct osoe_progress_team *learnt)
{
    (void)context;
    struct osoe_team_block *b = osoe_node_team(&osoe.node, team);
    if (!b || !atomic_load_explicit(&b->record.published, memory_order_acquire))
        return false;
    const struct osoe_team_record *r = &b->record;
    uint32_t number = atomic_load_explicit(&b->name.number, memory_order_acquire);
    if (r->nodes < 2)
        return false;
    uint32_t fanout = osoe.bootstrap.collective_fanout;
    if (r->children > fanout || r->index >= r->nodes)
        osoe_fatal("team %u's record on this node says %u children at index %u of %u nodes, "
                   "at a fan-out of %u", number, r->children, r->index, r->nodes, fanout);
    learnt->tree = (struct osoe_collective_tree){
        .count = r->nodes,
        .index = r->index,
        .fanout = fanout,
        .parent = r->parent == UINT32_MAX ? OSOE_COLLECTIVE_TREE_NONE
                                          : osoe_node_of(&osoe.job, r->parent),
        .children = r->children,
    };
    learnt->number = number;
    learnt->parent_lead = r->parent == UINT32_MAX ? 0
                                                  : osoe_job_local_index(&osoe.job, r->parent);
    for (uint32_t j = 0; j < r->children; j++) {
        learnt->tree.child[j] = osoe_node_of(&osoe.job, r->child[j]);
        learnt->child_lead[j] = osoe_job_local_index(&osoe.job, r->child[j]);
    }
    learnt->published = &b->tree;
    learnt->generation = atomic_load_explicit(&b->name.generation, memory_order_acquire);
    return true;
}

/* A team's block on this node by the name a message carries, and the block's
 * generation (progress.h): read with acquire (§3.16), though the map entry's
 * acquire already carries the name that was written before it (node.h). */
static uint32_t team_by_name(void *context, uint32_t lead, uint32_t number,
                             uint32_t *generation)
{
    (void)context;
    uint32_t block = osoe_node_team_find(&osoe.node, lead, number);
    if (block != UINT32_MAX)
        *generation = atomic_load_explicit(&osoe_node_team(&osoe.node, block)->name.generation,
                                           memory_order_acquire);
    return block;
}

/*
 * §5.3's release, applied on this node. The engine knows only (team, epoch);
 * what a release *does* - advance the team's counter and wake its members - is
 * the collective layer's, reached through internode.h's upcall. Before slice 3
 * this function switched on the team number and bumped the node header's
 * counters itself, so the backend knew which futex words the SHMEM layer waits
 * on - the knowledge §10 once said it could not do without.
 */
static void release(void *context, uint32_t team, uint32_t epoch)
{
    (void)context;
    osoe_internode_released(team, epoch);
}

/* A deadlock detector message from another node's lead (progress.h). */
static void deadlock_arrived(void *context, uint32_t node, const struct osoe_deadlock_message *m)
{
    (void)context;
    osoe_deadlock_message(node, m);
}

/* OSOE_TEST_ABORT_DELAY_MS, in nanoseconds, or 0: a peer's abortive goodbye
 * acted on that late, and a helper's own abort begun that late (abort_arrived,
 * helper_main), so that a test sees what the agent's wait for its helpers'
 * goodbyes is for (abort_owners_model.py's no_wait). */
static uint64_t abort_delay_ns;

/* A peer's abortive goodbye (progress.h): this node aborts too, on behalf of
 * the same PE and with the same status and disposition - unless it already has,
 * and then the first declaration stands (node.c) - and its agent, woken by the
 * declaration, tells its own peers and ends (abort.h). */
static void abort_arrived(void *context, uint32_t pe, int32_t status, uint32_t disposition)
{
    (void)context;
    if (abort_delay_ns) {
        struct timespec late = { (time_t)(abort_delay_ns / 1000000000),
                                 (long)(abort_delay_ns % 1000000000) };
        nanosleep(&late, NULL);
    }
    osoe_node_declare_abort(&osoe.node, pe, status,
                            disposition == OSOE_ABORT_GLOBAL_EXIT ? OSOE_ABORT_GLOBAL_EXIT
                                                                  : OSOE_ABORT_FATAL);
}

/* Whether this node's abort is declared (progress.h): one acquire load. */
static bool node_aborting(void *context)
{
    (void)context;
    return osoe_abort_pending();
}

/* ---- waking the engine (§3.12) ------------------------------------------ */

/* The word owner `owner` sleeps on: a helper's in the area, and the lead's
 * agent's its PE slot's, which stop, abort and the posted arrivals bump too. */
static _Atomic uint32_t *wake_word(uint32_t owner)
{
    return owner == 0 ? &osoe.node.slots[0].agent_wake : osoe_am.area.owner[owner].wake;
}

/*
 * Wake owner `owner` only if it sleeps (§3.10; lead_wake_model.py, the owner a
 * parameter). The caller has already set its connection's dirty bit in the
 * owner's bitmap (osoe_connection_area_mark), a seq_cst RMW; this load of the
 * owner's flag is the second half of the writer's side of a two-sided
 * protocol, so it is seq_cst too (§3.16). If it reads 0 - the owner has not
 * raised its flag, or another writer claimed this sleep's wake - the owner's
 * seq_cst load of its bits after raising it will see this writer's bit, or
 * the wake already claimed is on its way. If it wins the flag, the bump
 * changes the word the owner sleeps on, so a futex_wait that has not started
 * yet returns at once.
 *
 * An awake owner costs a writer one load of a line it shares with every other
 * writer, read-mostly, instead of a locked add on it and a futex_wake syscall.
 */
static void wake_owner(uint32_t owner)
{
    _Atomic uint32_t *flag = osoe_am.area.owner[owner].sleeping;
    /*
     * Claim the wake: only the writer whose CAS takes the flag from 1 to 0
     * makes the syscall; the rest see 0 and skip until the owner raises it
     * again. Without the claim every writer during the owner's wake-up latency
     * saw 1, and a burst of puts still woke the lead for 93% of them (the
     * model's `every_writer`). The load first, so an awake owner's line stays
     * shared rather than taken exclusive by every writer's CAS.
     */
    uint32_t sleeping = 1;
    if (atomic_load_explicit(flag, memory_order_seq_cst) == 0 ||
        !atomic_compare_exchange_strong_explicit(flag, &sleeping, 0, memory_order_seq_cst,
                                                 memory_order_seq_cst)) {
        osoe_count(OSOE_C_AM_WAKES_SKIPPED, 1);
        return;
    }
    osoe_count(OSOE_C_AM_WAKES_SENT, 1);
    _Atomic uint32_t *word = wake_word(owner);
    atomic_fetch_add_explicit(word, 1, memory_order_seq_cst);
    osoe_futex_wake(word, 1, OSOE_FUTEX_SHARED);
}

/*
 * The owners this thread has held a wake back from (osoe_progress_host_defer),
 * a bit each: owner 0's bit 0 when the node runs no helpers, and helper h's
 * bit h - 1 otherwise, since then owner 0 holds no data wire. Masked by the
 * owners the node has, as a session's leftover can name one this one lacks.
 */
static _Thread_local uint64_t deferred;

static uint64_t owner_bit(uint32_t owner, uint32_t helpers)
{
    return helpers ? UINT64_C(1) << (owner - 1) : 1;
}

void osoe_progress_host_wake_deferred(void)
{
    if (!osoe_am.internode)
        return;
    uint32_t helpers = osoe_am.area.helpers;
    uint64_t owners = helpers ? (helpers == 64 ? UINT64_MAX : (UINT64_C(1) << helpers) - 1) : 1;
    uint64_t bits = deferred & owners;
    deferred = 0;
    for (; bits != 0; bits &= bits - 1) {
        uint32_t bit = (uint32_t)__builtin_ctzll(bits);
        wake_owner(helpers ? bit + 1 : 0);
    }
}

/* The owner of the connection toward `peer`, named as the writer's mark names
 * it (connection-area.h). */
static uint32_t owner_of_peer(uint32_t peer)
{
    uint32_t index;
    return osoe_connection_area_owner(&osoe_am.area, peer, &index);
}

void osoe_progress_host_wake(uint32_t peer)
{
    if (!osoe_am.internode)
        return;
    deferred |= owner_bit(owner_of_peer(peer), osoe_am.area.helpers);
    osoe_progress_host_wake_deferred();
}

void osoe_progress_host_defer(uint32_t peer)
{
    if (osoe_am.internode)
        deferred |= owner_bit(owner_of_peer(peer), osoe_am.area.helpers);
}

void osoe_progress_host_wake_all(void)
{
    if (!osoe_am.internode)
        return;
    uint32_t helpers = osoe_am.area.helpers;
    deferred = 0;
    for (uint32_t owner = helpers ? 1 : 0; owner <= helpers; owner++)
        wake_owner(owner);
}

/*
 * The limit on the kernel's unsent bytes the application thread asks
 * (osoe_progress_host_limit_unsent): a generation above the bytes plus one,
 * never cleared, so that every owner takes it for its own wires and keeps the
 * request it last took (struct osoe_am_owner) - with no helpers, the agent for
 * every wire, as before there were owners. Asked as a writer's dirty bit is - a
 * seq_cst store, then each owner's claimed wake - and taken as the bits are, at
 * a pass and again after the owner raises its `sleeping`, so that a request
 * never waits for a sweep (lead_wake_model.py's protocol, with the request as
 * one more bit). The two flags beside it are written before the store and read
 * after a take. The lead's application thread is the word's one writer.
 */
#define UNSENT_GENERATION_SHIFT 33      /* above the bytes plus one: up to 2^32 */
static _Atomic uint64_t unsent_request;
static uint32_t unsent_generation;      /* the application thread's */
static bool unsent_by_hand, unsent_say;
static uint64_t unsent_said;            /* the agent's: the request it last said */

static uint32_t unsent_bytes(uint64_t asked)
{
    return (uint32_t)((asked & ((UINT64_C(1) << UNSENT_GENERATION_SHIFT) - 1)) - 1);
}

/* On owner `owner`'s thread: its data wires' limit, as asked, and how many it
 * reached, published for the agent's line (say_unsent) - the request after
 * them, with release. */
static void take_unsent_request(uint32_t owner)
{
    struct osoe_am_owner *h = &osoe_am.owner[owner];
    uint64_t asked = atomic_load_explicit(&unsent_request, memory_order_acquire);
    if (asked == h->unsent_taken)
        return;
    h->unsent_taken = asked;
    struct osoe_progress_owner *o = &osoe_am.progress.owner[owner];
    uint32_t bytes = unsent_bytes(asked), limited = 0;
    for (uint32_t i = 0; i < o->count; i++)
        limited += osoe_wire_limit_unsent(&o->wire[i], bytes);
    atomic_store_explicit(&h->unsent_reached, limited, memory_order_release);
    atomic_store_explicit(&h->unsent_applied, asked, memory_order_release);
}

/* Whether a request has come since owner `owner` last took one: looked at
 * after its flag is raised, as its dirty bits are. */
static bool unsent_asked(uint32_t owner)
{
    return atomic_load_explicit(&unsent_request, memory_order_seq_cst) !=
           osoe_am.owner[owner].unsent_taken;
}

/* On the agent: the node's line under SHMEM_DEBUG, once, when every owner has
 * applied the request it took - with the wires they reached, which
 * shmem.unsent.* counts. A later pass says it when a helper has not yet. */
static void say_unsent(void)
{
    uint64_t asked = osoe_am.owner[0].unsent_taken;
    if (asked == 0 || asked == unsent_said || !unsent_say)
        return;
    uint32_t limited = 0;
    for (uint32_t owner = 0; owner <= osoe_am.area.helpers; owner++) {
        struct osoe_am_owner *h = &osoe_am.owner[owner];
        if (atomic_load_explicit(&h->unsent_applied, memory_order_acquire) != asked)
            return;
        limited += atomic_load_explicit(&h->unsent_reached, memory_order_acquire);
    }
    unsent_said = asked;
    uint32_t bytes = unsent_bytes(asked);
    if (bytes)
        fprintf(stderr, "osoe: node %u limits the unsent bytes of each of its %u connections "
                        "to %u%s\n", osoe.bootstrap.node, limited, bytes,
                unsent_by_hand ? " (OSOE_WIRE_UNSENT_BYTES)" : "");
    else
        fprintf(stderr, "osoe: node %u leaves the unsent bytes of its %u connections to the "
                        "system (OSOE_WIRE_UNSENT_BYTES=0)\n", osoe.bootstrap.node, limited);
}

/*
 * The check keepalive cannot make (progress.h's osoe_progress_check_silent):
 * the budget keepalive is given - idle, then count probes an interval apart -
 * and at most one look an interval, so a vanished peer with data in flight is
 * found as soon as an idle one would be. 0: keepalive is off, and so is this.
 */
static uint32_t silent_budget_ms;
static uint64_t silent_interval_ns, next_silent_check_ns;

static void silent_check_start(const struct osoe_bootstrap *b)
{
    next_silent_check_ns = 0;
    if (b->keepalive_off) {
        silent_budget_ms = 0;
        return;
    }
    uint32_t idle = b->keepalive_idle_s ? b->keepalive_idle_s : OSOE_DEFAULT_KEEPALIVE_IDLE_S;
    uint32_t interval = b->keepalive_interval_s ? b->keepalive_interval_s
                                                : OSOE_DEFAULT_KEEPALIVE_INTERVAL_S;
    uint32_t count = b->keepalive_count ? b->keepalive_count : OSOE_DEFAULT_KEEPALIVE_COUNT;
    silent_budget_ms = (idle + interval * count) * 1000;
    silent_interval_ns = (uint64_t)interval * 1000000000;
}

/*
 * Read from another process, since only the lead runs the engine. The value is
 * the lead's own bookkeeping and is written by that thread alone, so a reader
 * sees either the old value or the new one - and the old one only makes a wait
 * go round again, which is why this needs no protocol of its own.
 */
uint64_t osoe_progress_host_acked(uint32_t peer)
{
    struct osoe_connection *connection = osoe_connection_to(&osoe_am.area, peer);
    return connection ? osoe_connection_acked(connection) : 0;
}

/*
 * The agent's requests to a helper (helper_request_model.py): a request word
 * holds a sequence above its kind, and is never 0, which is no request; an
 * answer is the word of the request it answers, its verdict in the top bit.
 * A helper's stop goes as far as acknowledged, then done: its wires closed, or
 * aborted, and its thread about to return.
 */
enum { REQUEST_IDLE = 1, REQUEST_QUIET = 2, REQUEST_STOP = 3, REQUEST_KIND_BITS = 2 };
#define ANSWER_YES (UINT32_C(1) << 31)
enum { HELPER_ACKNOWLEDGED = 1, HELPER_DONE = 2 };

static uint32_t request_sequence;       /* the agent's: the last request's */
static uint32_t deaf_helper;            /* OSOE_TEST_DEAF_HELPER, or 0 */

/* ---- an owner's sleep (§3.10) ------------------------------------------- */

/*
 * An owner's half of the sleep protocol (§3.10; lead_wake_model.py, the owner
 * a parameter), on its thread. A pass that moved nothing is about to be
 * followed by the owner's wait on its wake word - in its ring, at the value it
 * loaded before the pass. So raise its `sleeping` first - seq_cst, the store
 * side of the two-sided protocol - and then look at its dirty bits again with
 * seq_cst loads: a writer whose bit landed after the pass's exchange either
 * shows here, and the owner goes round again, or read the flag as 1 and bumps
 * the wake word and wakes it, which completes the ring's armed wait. The flag
 * comes down at the next pass, which is after the wait.
 */
static void lower_flag(uint32_t owner)
{
    struct osoe_am_owner *h = &osoe_am.owner[owner];
    if (h->flag_raised) {
        atomic_store_explicit(osoe_am.area.owner[owner].sleeping, 0, memory_order_seq_cst);
        h->flag_raised = false;
    }
}

/* Raise the flag, and say whether a writer's bit is set after all. */
static bool raise_flag_and_look(uint32_t owner)
{
    const struct osoe_connection_area_owner *words = &osoe_am.area.owner[owner];
    atomic_store_explicit(words->sleeping, 1, memory_order_seq_cst);
    osoe_am.owner[owner].flag_raised = true;
    for (uint32_t w = 0; w < words->dirty_words; w++)
        if (atomic_load_explicit(&words->dirty[w], memory_order_seq_cst) != 0)
            return true;
    return false;
}

/* ---- lifecycle --------------------------------------------------------- */

/*
 * The descriptors owner `owner`'s pool holds (§3.8; osoe_progress_host_start
 * says why): C and the slack for each connection it holds, and, owner 0's, one
 * a peer for the detector and the degree for each of `teams` team blocks.
 */
static uint32_t descriptors_of(uint32_t owner, uint32_t peers, uint32_t helpers, uint32_t teams)
{
    uint32_t n = osoe_owner_wires(owner, peers, helpers) *
                 (osoe_am.area.credits + OSOE_REPLY_SLACK);
    if (owner == 0)
        n += peers + OSOE_COLLECTIVE_DEGREE(osoe.bootstrap.collective_fanout) * teams;
    return n;
}

/*
 * Each owner's slice of an array the node allocates once - its connections,
 * busy words and reply descriptors - starts a line of its own (the adversarial
 * read of Part 3, S2): with helpers the next owner's slice is another thread's,
 * written at every visit. The step, in elements, that keeps a slice of
 * `element`-byte items on a line boundary, and `n` rounded up to it.
 */
static uint64_t line_step(uint64_t element)
{
    uint64_t a = element, b = OSOE_CACHELINE;
    while (b) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return OSOE_CACHELINE / a;
}

static uint64_t to_step(uint64_t n, uint64_t step)
{
    return (n + step - 1) / step * step;
}

/* `count` elements of `element` bytes, zeroed, starting a line. */
static void *lines_of(uint64_t count, uint64_t element)
{
    uint64_t bytes = to_step(count * element, OSOE_CACHELINE);
    void *p = aligned_alloc(OSOE_CACHELINE, bytes ? bytes : OSOE_CACHELINE);
    if (p)
        memset(p, 0, bytes);
    return p;
}

/* This lead's node-pair listener, from advertise to the mesh (osoe_wires_open
 * closes it). Outside osoe_am, which osoe_transport_init starts afresh after the
 * rendezvous that carried its port. */
static struct osoe_listener mesh_listener = { .fd = -1 };

void osoe_progress_host_advertise(void *blob)
{
    osoe_listener_open_any_port(&mesh_listener, &osoe.bootstrap, osoe.node.hdr->session, NULL);
    uint16_t port = mesh_listener.port;
    memcpy(blob, &port, sizeof port);
}

void osoe_progress_host_start(uint32_t session)
{
    /* A session's own. */
    atomic_store_explicit(&unsent_request, 0, memory_order_seq_cst);
    unsent_generation = 0;
    unsent_said = 0;
    request_sequence = 0;
    /* Only in a job of more than one node, from the internode backend's start
     * (transport-internode.c), which has set osoe_am.internode and
     * osoe_am.lead before anything reads them. */
    /* Every local PE maps the area: its application threads reserve into these
     * buffers like any other writer (§3.5, §3.12) - laid out for the node's
     * OSOE_PROGRESS_HELPERS (owner-map.h), which the node's own rendezvous has
     * checked its PEs agree on (node.c). */
    osoe_connection_area_open(&osoe_am.area, &osoe.bootstrap, osoe.job.node_count,
                              osoe.bootstrap.progress_helpers, session,
                              osoe.node.hdr->creator_pid);
    if (!osoe_am.lead)
        return;

    uint32_t peers = osoe.job.node_count - 1, helpers = osoe_am.area.helpers;
    /* The test hook (OSOE_TEST_DEAF_HELPER): a helper that never answers its
     * stop, which the agent must end the job naming (wait_acknowledged). */
    deaf_helper = 0;
    if (osoe_environment_u32("OSOE_TEST_DEAF_HELPER", &deaf_helper) == OSOE_ENVIRONMENT_INVALID ||
        deaf_helper > helpers)
        osoe_fatal("OSOE_TEST_DEAF_HELPER='%s' names none of this node's %u progress helpers",
                   getenv("OSOE_TEST_DEAF_HELPER"), helpers);
    uint32_t delay_ms = 0;
    if (osoe_environment_u32("OSOE_TEST_ABORT_DELAY_MS", &delay_ms) == OSOE_ENVIRONMENT_INVALID ||
        delay_ms > 1000 * OSOE_ABORT_GRACE_SECONDS / 2)
        osoe_fatal("OSOE_TEST_ABORT_DELAY_MS='%s' is not a delay from 0 to %u milliseconds",
                   getenv("OSOE_TEST_ABORT_DELAY_MS"), 1000u * OSOE_ABORT_GRACE_SECONDS / 2);
    abort_delay_ns = (uint64_t)delay_ms * 1000000;
    /*
     * §3.8's two arguments. C per connection bounds the replies - the origin
     * takes a credit per get or fetching AMO and returns it when the reply
     * lands (argument 1). degree x teams, node-wide, bounds the collective
     * forwards: while collectives are blocking, a node of the internode tree
     * has at most one ARRIVE or its k RELEASEs queued per team (argument 2,
     * reply.h). The pool is node-wide, so an idle connection costs nothing.
     *
     * Before slice 2 this was peers x C, with no term for the forwards, so a
     * node holding C gets toward a peer had nothing left for its own barrier
     * arrival and the request was dropped (second review, D1). Slice 2 added
     * the forwards per connection, which was the star's shape - its root
     * released every peer at once. No legal program can empty the pool, and
     * progress.c ends the job naming the peer if one does.
     */
    /*
     * The forwards' term counts the teams this node can hold at once, since
     * its engine forwards for those and no others, and the pool is node-wide,
     * so it costs a descriptor per team per degree and not one per connection:
     * OSOE_TEAM_BLOCKS since the team redesign's step D (D5), its pool of team
     * blocks - a team's members are released only once its RELEASEs have left
     * the control queues (release_local, progress.c, holds the local release
     * until then), so a block given back and claimed again finds none of the
     * old team's still queued. Until step D's second part it
     * was OSOE_MAX_TEAMS, the teams that could exist job-wide, which a node
     * could not exceed while numbers were job-wide; a node may now hold more
     * teams than any one PE's numbers. 9,216 descriptors at the defaults,
     * about 432 KiB. The per-team state below is sized by the pool too, since
     * it is indexed by block.
     */
    /*
     * And one a peer for the deadlock detector's messages (§3.15), of which a
     * connection has at most one queued: the root asks each node once a
     * collection and a node answers once, each only after what came before
     * has been answered (deadlock.c).
     *
     * A pool an owner (owner-map.h): the replies' term over the connections it
     * holds, and the forwards' and the detector's, the control plane's, owner
     * 0's. With no helpers, owner 0's is the node-wide pool these arguments
     * were made for.
     */
    uint32_t teams = osoe.bootstrap.team_blocks, owners = helpers + 1;
    uint64_t connection_step = line_step(sizeof *osoe_am.connection);
    uint64_t busy_step = line_step(sizeof *osoe_am.busy);
    uint64_t descriptor_step = line_step(sizeof *osoe_am.descriptor);
    uint64_t connections = 0, descriptors = 0, busy_words = 0;
    for (uint32_t o = 0; o < owners; o++) {
        connections += to_step(osoe_owner_wires(o, peers, helpers), connection_step);
        descriptors += to_step(descriptors_of(o, peers, helpers, teams), descriptor_step);
        busy_words += to_step((osoe_owner_wires(o, peers, helpers) + 63) / 64, busy_step);
    }
    /* On lines of their own (am-state.h): a size that is a multiple of the
     * alignment, as aligned_alloc asks, since the type's alignment is a line. */
    osoe_am.owner = aligned_alloc(OSOE_CACHELINE, owners * sizeof *osoe_am.owner);
    if (osoe_am.owner)
        memset(osoe_am.owner, 0, owners * sizeof *osoe_am.owner);
    struct osoe_ring **rings = calloc(owners, sizeof *rings);
    /* Each owner's engine on lines of its own too: its type's alignment is a
     * line, for the counters' record it carries (progress.h). */
    struct osoe_progress_owner *engine_owners =
        aligned_alloc(OSOE_CACHELINE, owners * sizeof *engine_owners);
    osoe_am.wire = calloc(2 * (size_t)peers, sizeof *osoe_am.wire);
    osoe_am.connection = lines_of(connections, sizeof *osoe_am.connection);
    osoe_am.control = calloc(peers, sizeof *osoe_am.control);
    osoe_am.control_busy = calloc((peers + 63) / 64, sizeof *osoe_am.control_busy);
    osoe_am.busy = lines_of(busy_words, sizeof *osoe_am.busy);
    osoe_am.descriptor = lines_of(descriptors, sizeof *osoe_am.descriptor);
    osoe_am.team = calloc(teams, sizeof *osoe_am.team);
    osoe_am.pending = calloc(teams, sizeof *osoe_am.pending);
    if (!osoe_am.owner || !rings || !engine_owners || !osoe_am.wire ||
        !osoe_am.connection || !osoe_am.control || !osoe_am.control_busy || !osoe_am.busy ||
        !osoe_am.descriptor || !osoe_am.team || !osoe_am.pending)
        osoe_fatal("out of memory setting up the AM backend for %u peer nodes", peers);
    for (uint32_t o = 0; o < owners; o++)
        rings[o] = &osoe_am.owner[o].ring;

    /*
     * The node-pair connections, one per peer, grouped by owner (owner-map.h) -
     * which with no helpers is peer-slot order, node order with this node left
     * out, as osoe_progress_slot computes it (§3.4) - and the ring an owner that
     * carries its own, disabled until the owner enables it on its first pass:
     * each owner is its ring's only submitter (ring.h). Each peer's address is
     * the rendezvous's, and its port the one its blob advertised (§6.2). O(N)
     * once, for the eager mesh.
     */
    struct sockaddr_in *peers_address = calloc(osoe.job.node_count, sizeof *peers_address);
    if (!peers_address)
        osoe_fatal("out of memory for %u node addresses", osoe.job.node_count);
    bool one_machine = true;
    for (uint32_t n = 0; n < osoe.job.node_count; n++) {
        uint16_t port;
        memcpy(&port, osoe_job_backend_blob(&osoe.job, n), sizeof port);
        peers_address[n] = (struct sockaddr_in){ .sin_family = AF_INET,
                                                 .sin_port = htons(port),
                                                 .sin_addr.s_addr = osoe_job_node_address(&osoe.job, n) };
        one_machine = one_machine && (ntohl(peers_address[n].sin_addr.s_addr) >> 24) == 127;
    }
    /*
     * Keepalive off by default where every node has a loopback address - one
     * machine's logical nodes (osoe-launch, §8.1) - and OSOE_KEEPALIVE is not
     * set: their kernel is one, which closes a dead node's connections itself
     * and answers for a stopped one, so a probe there can only be lost, and an
     * overloaded machine's loopback backlog loses them, ending healthy jobs of
     * tens of logical nodes as vanished hosts (the review of v0.2 step 4's
     * merge point, correctness Q2-1; the author, 2026-10-10). Before the wires
     * open and the silent-peer check starts, which both read it; a job across
     * machines, tier 3's namespaces among them, keeps it on.
     */
    if (one_machine && !osoe.bootstrap.keepalive_set)
        osoe.bootstrap.keepalive_off = true;
    struct osoe_mesh mesh = { &mesh_listener, peers_address };
    osoe_wires_open(osoe_am.wire, rings, helpers, &osoe.bootstrap, session, &mesh);
    free(peers_address);
    free(rings);

    struct osoe_progress_host host = { resolve, notify, release, NULL, abort_arrived,
                                       landing_request, landed, completed, deadlock_arrived,
                                       node_aborting };
    osoe_progress_init(&osoe_am.progress, &osoe_am.area, engine_owners, &host);
    uint64_t connections_before = 0, descriptors_before = 0, busy_before = 0;
    for (uint32_t o = 0; o < owners; o++) {
        uint32_t wires = osoe_owner_wires(o, peers, helpers);
        uint32_t count = descriptors_of(o, peers, helpers, teams);
        osoe_progress_attach(&osoe_am.progress, o, &osoe_am.owner[o].ring,
                             &osoe_am.wire[osoe_ring_first(o, peers, helpers)],
                             &osoe_am.connection[connections_before],
                             &osoe_am.busy[busy_before],
                             &osoe_am.descriptor[descriptors_before], count);
        connections_before += to_step(wires, connection_step);
        descriptors_before += to_step(count, descriptor_step);
        busy_before += to_step((wires + 63) / 64, busy_step);
        osoe_am.owner[o].flag_raised = false;
    }
    /* Owner 0's control wires, after its data wires on its ring (owner-map.h). */
    osoe_progress_attach_control(&osoe_am.progress,
                                 &osoe_am.wire[osoe_control_ring_index(0, peers, helpers)],
                                 osoe_am.control, osoe_am.control_busy);
    osoe_progress_attach_teams(&osoe_am.progress, osoe_am.team, osoe_am.pending, teams,
                               team_of_record, team_by_name);
    /* Count into the control segment, where osoe-stat and the tests read it:
     * a record an owner, which says the wires it holds (node.h, layout 36). */
    if (osoe.node.owner_count != owners)
        osoe_fatal("the control segment holds %u owners' records, and this node runs %u owners",
                   osoe.node.owner_count, owners);
    for (uint32_t o = 0; o < owners; o++) {
        struct osoe_am_owner_metrics *record = &osoe.node.owner_metrics[o];
        engine_owners[o].metrics = record;
        atomic_store_explicit(&record->data_wires, osoe_owner_wires(o, peers, helpers),
                              memory_order_release);
        atomic_store_explicit(&record->control_wires, o == 0 ? peers : 0, memory_order_release);
    }
    /* §3.14's connection scope, in the same segment: the engine writes it,
     * osoe-stat and the tests read it. Each team's tree state goes to its own
     * block, which the engine learns with the team (team_of_record). */
    osoe_am.progress.connection_metrics = osoe.node.connection_metrics;
    for (uint32_t c = 0; c < osoe.node.connection_count; c++)
        atomic_store_explicit(&osoe.node.connection_metrics[c].peer,
                              c < osoe.job.node ? c : c + 1, memory_order_release);
    silent_check_start(&osoe.bootstrap);
}

/* The segments owner `o` has sent on every wire it holds, data and control:
 * what moves while its peers read, which the no-progress rule watches. */
static uint64_t segments_sent(struct osoe_progress_owner *o)
{
    return osoe_counter_word_read(&o->metrics->segments_sent) +
           osoe_counter_word_read(&o->metrics->control_segments_sent);
}

/*
 * A sweep of owner `o`'s close (§4.10). On the agent, what every library wait
 * does: the node's abort ends the process there (sweep.c), as it did before
 * there were helpers - telling no peer, and waiting for no helper, which races
 * the process's end with its own goodbyes (the adversarial read of Part 3, N1:
 * no peer is told less than it was with no helpers). On a helper, whether the
 * node's abort has come, which the helper answers by aborting its own wires
 * (helper_stop): only the agent ends the process (abort.c), and a helper that
 * ended it would leave its own peers untold even where the agent tells its own.
 */
static bool sweep_finds_abort(struct osoe_progress_owner *o)
{
    if (o->index != 0)
        return osoe_abort_pending();
    osoe_sweep_expired("shmem_finalize");
    return false;
}

/* " (progress helper h)" after a fatal's words for owner `o`, or nothing for
 * the agent, whose words they are. */
static const char *owner_said(const struct osoe_progress_owner *o, char *buffer, size_t size)
{
    if (o->index == 0)
        return "";
    snprintf(buffer, size, " (progress helper %u)", o->index);
    return buffer;
}

/*
 * Put everything this node still owes the network on the wire, before the
 * engine that would have sent it goes away: owner `o`'s part, on its thread -
 * the agent's in its exit (transport.h), a helper's at its stop (helper_stop).
 *
 * **A node must not stop its engine while it owes a peer bytes** (§6.4), and
 * the barrier does not establish that it does not. §5.3's root releases its own
 * node the instant the last node arrives, and only *enqueues* the releases that
 * free the others - the drain and flush that put them on the wire are later
 * passes. So the root's own PEs can leave the final barrier, reach
 * shmem_finalize and stop the agent while the message every other node is
 * waiting for is still sitting in a buffer that only that agent sends. Every
 * other node then waits forever for a node that has exited.
 *
 * Measured before this existed: 3 hangs in 12 runs of the suite, all on the
 * map where node 0 holds three PEs and the others hold two and one - the root
 * having the most PEs is what makes it likeliest to be the last to arrive and
 * the first to leave. The two PEs of node 0 that were not the lead exited
 * cleanly; every other rank was killed at the launcher's timeout, which is the
 * signature to recognise it by.
 *
 * This is the owner's last turn, so it is the only thread touching its part
 * of the engine and polls it directly (§3.12): owner `o`'s wires, on its
 * thread - every wire, on the lead's agent, while the node runs no helpers.
 *
 * False if a helper found the node's abort at a sweep, before the flush was
 * done (sweep_finds_abort).
 */
static bool flush_before_close(struct osoe_progress_owner *o)
{
    /*
     * Not a bare spin: a pass that moved nothing sleeps a moment and sweeps, as
     * every wait does (§4.10), and the wait gives up only by the no-progress
     * rule (§3.15) - watching this node's segments sent, which is what moves
     * while the peers are reading. Before slice 2 it spun on the CPU and died
     * after the bootstrap timeout (second review, atomics F7).
     */
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin(&watch, segments_sent(o), osoe.bootstrap.no_progress_ns);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    while (!osoe_progress_idle(o)) {
        if (!osoe_progress_poll(o))
            osoe_ring_wait(o->ring, OSOE_SWEEP_NS);
        if (!osoe_deadline_expired(&sweep))
            continue;
        sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        if (sweep_finds_abort(o))
            return false;
        uint64_t sent = segments_sent(o);
        char said[32];
        if (osoe_stall_watch_stalled(&watch, sent))
            osoe_fatal("shmem_finalize: no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT) "
                       "flushing this node's last messages - segments sent has stayed at "
                       "%llu; a peer node has stopped reading its wire%s",
                       (unsigned long long)osoe_stall_watch_seconds(&watch),
                       (unsigned long long)sent, owner_said(o, said, sizeof said));
    }
    return true;
}

/*
 * The close (§6.4, close_model.py): every connection sends its goodbye and ends
 * its stream, and the lead keeps reading - and applying, since a peer may still
 * be paying an acknowledgement - until every peer's stream has ended after its
 * own goodbye. Only then are the sockets closed: closing one while the peer
 * still sends makes the kernel reset it, and the peer's send fails on a node
 * that did nothing wrong. The no-progress rule watches how many connections
 * have ended (§3.15); by default a peer that never finalizes is waited for.
 * Owner `o`'s connections, on its thread, and then its ring. False, with the
 * ring still open, if a helper found the node's abort at a sweep.
 */
static bool close_connections(struct osoe_progress_owner *o)
{
    osoe_progress_close(o);
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin(&watch, 0, osoe.bootstrap.no_progress_ns);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    while (!osoe_progress_closed(o)) {
        /* A poll can take a peer's end of stream and say it moved nothing, so
         * the condition is asked again before the sleep, which would otherwise
         * last the whole sweep (the release fixes' review, conformance F1). */
        if (!osoe_progress_poll(o) && !osoe_progress_closed(o))
            osoe_ring_wait(o->ring, OSOE_SWEEP_NS);
        if (!osoe_deadline_expired(&sweep))
            continue;
        sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        if (sweep_finds_abort(o))
            return false;
        uint32_t ended = 0;
        for (uint32_t i = 0; i < o->count; i++)
            ended += osoe_wire_ended(&o->wire[i]);
        for (uint32_t slot = 0; slot < o->controls; slot++)
            ended += osoe_wire_ended(&o->control_wire[slot]);
        char said[32];
        if (osoe_stall_watch_stalled(&watch, ended))
            osoe_fatal("shmem_finalize: no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT) "
                       "closing this node's connections - %u of %u wires have ended at the "
                       "far end; the other peers have not finalized%s",
                       (unsigned long long)osoe_stall_watch_seconds(&watch), ended,
                       o->count + o->controls, owner_said(o, said, sizeof said));
    }
    for (uint32_t i = 0; i < o->count; i++)
        osoe_wire_close(&o->wire[i]);
    for (uint32_t slot = 0; slot < o->controls; slot++)
        osoe_wire_close(&o->control_wire[slot]);
    osoe_ring_close(o->ring);
    return true;
}

/* ---- helpers (owner-map.h; v0.2 step 4, Part 3) ------------------------ */

static uint32_t request_kind(uint32_t word)
{
    return word & ((UINT32_C(1) << REQUEST_KIND_BITS) - 1);
}

/* Bump helper `helper`'s wake word and wake it: its wait in its ring ends,
 * whichever value it loaded before (§4.9). Unclaimed, unlike a writer's wake
 * (wake_owner): what nudges a helper is rare. */
static void nudge(uint32_t helper)
{
    _Atomic uint32_t *word = wake_word(helper);
    atomic_fetch_add_explicit(word, 1, memory_order_seq_cst);
    osoe_futex_wake(word, 1, OSOE_FUTEX_SHARED);
}

/*
 * Ask helper `helper` something, on the agent, the word's one writer: the
 * request stored, seq_cst, then the helper nudged - so a helper that loaded
 * its wake word before the bump sleeps on a value it no longer holds, and one
 * that loads it after loads the request after too. Each request a new
 * sequence, so that an answer names the one it answers.
 */
static void post(uint32_t helper, uint32_t kind)
{
    struct osoe_am_owner *h = &osoe_am.owner[helper];
    request_sequence = (request_sequence + 1) & ((ANSWER_YES - 1) >> REQUEST_KIND_BITS);
    h->posted = request_sequence << REQUEST_KIND_BITS | kind;
    atomic_store_explicit(&h->request, h->posted, memory_order_seq_cst);
    nudge(helper);
}

/* A helper's stop gone as far as `state`: published, then the agent woken,
 * which waits for it on this word (wait_acknowledged) or on its own
 * (wait_helpers). */
static void say_stop(struct osoe_am_owner *h, uint32_t state)
{
    atomic_store_explicit(&h->stop, state, memory_order_release);
    osoe_futex_wake(&h->stop, INT_MAX, OSOE_FUTEX_PRIVATE);
    _Atomic uint32_t *agent = wake_word(0);
    atomic_fetch_add_explicit(agent, 1, memory_order_seq_cst);
    osoe_futex_wake(agent, 1, OSOE_FUTEX_SHARED);
}

/* A quiet or an idle request answered, after a whole pass begun after the
 * helper read it (helper_main): the verdict of the owner's own view (§3.15)
 * and the request it answers, one word. True when a quiet request found the
 * owner not quiet: its view may have asked a peer to acknowledge - marked a
 * connection busy - which a pass seals, so the helper goes round again before
 * it sleeps, as the agent's pass follows its own view (agent.c). */
static bool answer(uint32_t helper, uint32_t request)
{
    struct osoe_progress_owner *o = &osoe_am.progress.owner[helper];
    uint64_t applied;
    bool quiet = request_kind(request) == REQUEST_QUIET;
    bool yes = quiet ? osoe_progress_quiet(o, &applied) : osoe_progress_idle(o);
    atomic_store_explicit(&osoe_am.owner[helper].answer, request | (yes ? ANSWER_YES : 0),
                          memory_order_release);
    return quiet && !yes;
}

/*
 * Owner `o`'s part of the job's end, on its thread: tell every peer of its
 * wires - an abortive goodbye on each that still has someone to tell - and keep
 * receiving until each peer's kernel has acknowledged it, since this process's
 * end with input unread is a reset, which would drop it
 * (osoe_progress_aborted). Bounded by `grace`, since a peer that is stopped
 * with a full receive window could hold the bytes forever, and this process is
 * ending either way: a peer told nothing takes its end of stream for a death,
 * which is what it is for that peer.
 */
static void abort_wires(struct osoe_progress_owner *o, const struct osoe_deadline *grace)
{
    /* The kernel takes this node's last bytes whole, the abortive goodbye among
     * them, even toward a peer that has stopped reading: that peer then reads
     * the job's status rather than a stream cut short (the second adversarial
     * read of v0.2 step 4, N6). */
    for (uint32_t i = 0; i < o->count; i++)
        osoe_wire_lift_unsent(&o->wire[i]);
    for (uint32_t slot = 0; slot < o->controls; slot++)
        osoe_wire_lift_unsent(&o->control_wire[slot]);
    uint32_t pe = 0;
    int status = EXIT_FAILURE;
    enum osoe_abort_disposition how = osoe_node_abort_read(&osoe.node, &pe, &status);
    if (!o->ring->enabled)
        osoe_ring_enable(o->ring);
    osoe_progress_abort(o, pe, status, (uint32_t)how, how == OSOE_ABORT_GLOBAL_EXIT);
    /* Each wait asks its condition again after a poll that moved nothing, as
     * close_connections does: an end of stream taken by the poll would
     * otherwise be seen a sweep late, which under srun -K1 is the window in
     * which the first node to end can have another node's PEs killed. */
    while (!osoe_progress_aborted(o) && !osoe_deadline_expired(grace))
        if (!osoe_progress_poll(o) && !osoe_progress_aborted(o))
            osoe_ring_wait(o->ring, OSOE_SWEEP_NS);
}

/* A helper that has seen the node's abort, at a turn or at a sweep of its
 * close: its own wires' goodbyes within its grace, then done, and its thread
 * returns; the process ends by the agent's hand (abort.c), which waits for it
 * within the same grace (osoe_progress_host_abort). */
static void helper_abort(uint32_t helper)
{
    struct osoe_deadline grace = osoe_deadline_in(UINT64_C(1000000000) * OSOE_ABORT_GRACE_SECONDS);
    abort_wires(&osoe_am.progress.owner[helper], &grace);
    say_stop(&osoe_am.owner[helper], HELPER_DONE);
}

/* A helper's stop (osoe_progress_host_exit): acknowledged at once, then its
 * wires flushed and closed, each wait bounded by the no-progress rule as the
 * agent's are, then done. The node's abort, found at a sweep of either, aborts
 * them instead. */
static void helper_stop(uint32_t helper)
{
    struct osoe_progress_owner *o = &osoe_am.progress.owner[helper];
    say_stop(&osoe_am.owner[helper], HELPER_ACKNOWLEDGED);
    if (flush_before_close(o) && close_connections(o))
        say_stop(&osoe_am.owner[helper], HELPER_DONE);
    else
        helper_abort(helper);
}

/*
 * A helper's thread (owner-map.h; the progress helpers proposal's §3.2): the
 * agent's pass over its own wires, with Part 1's per-owner sleep protocol, and
 * between passes the agent's requests (helper_request_model.py). Started by
 * the agent, so it begins with the agent's signal mask, every asynchronous
 * signal blocked, and its CPUs; it asks the agent's slice itself.
 */
static void *helper_main(void *arg)
{
    uint32_t helper = (uint32_t)(uintptr_t)arg;
    struct osoe_am_owner *h = &osoe_am.owner[helper];
    struct osoe_progress_owner *o = &osoe_am.progress.owner[helper];
    osoe_agent_place_this_thread();
    osoe_metrics_unpooled();
    char name[16];
    snprintf(name, sizeof name, "osoe-helper-%u", helper);
    (void)pthread_setname_np(pthread_self(), name);
    /* Named before its first turn, so that the deadlock detector leaves it out
     * of the lead PE's threads (deadlock.c). */
    atomic_store_explicit(&o->metrics->tid, (int32_t)syscall(SYS_gettid), memory_order_release);
    osoe_ring_enable(o->ring);          /* its only submitter from here (ring.h) */
    uint32_t taken = 0, due = 0;
    uint64_t turns = 0, abort_seen_ns = 0;
    for (;;) {
        /* The wake word before the request and the abort word, as every wait
         * loads its word before its pass (§4.9): a request stored, or an abort
         * declared, before the word was bumped is never slept through. Under
         * OSOE_TEST_ABORT_DELAY_MS the helper goes on passing that long first,
         * its wires read and excused as an ending node's are (progress.h's
         * host aborting). */
        uint32_t seen = atomic_load_explicit(wake_word(helper), memory_order_acquire);
        uint32_t request = atomic_load_explicit(&h->request, memory_order_seq_cst);
        if (osoe_abort_pending()) {
            uint64_t now = osoe_now_ns();
            if (abort_seen_ns == 0)
                abort_seen_ns = now;
            if (now - abort_seen_ns >= abort_delay_ns) {
                helper_abort(helper);
                return NULL;
            }
        }
        lower_flag(helper);
        take_unsent_request(helper);
        if (request != taken) {
            taken = request;
            if (request_kind(request) != REQUEST_STOP)
                due = request;
            else if (helper != deaf_helper) {
                helper_stop(helper);
                return NULL;
            }
        }
        bool moved = osoe_progress_poll(o);
        /* After a whole pass begun after the request was read: the pass
         * between a sample's halves, owner by owner (deadlock_owners_model.py). */
        if (due) {
            moved |= answer(helper, due);
            due = 0;
        }
        /* Counted seq_cst, after the turn and before the next turn's load of
         * the request: how the agent tells a helper that ignores its stop from
         * one that has not yet looked (wait_acknowledged). */
        atomic_store_explicit(&h->turns, ++turns, memory_order_seq_cst);
        if (moved || raise_flag_and_look(helper) || unsent_asked(helper))
            continue;
        osoe_ring_wait_wake(o->ring, wake_word(helper), seen, OSOE_SWEEP_NS);
    }
}

/* Start every helper not yet started, on the agent after its own start (the
 * progress helpers proposal's §3.7) - at its first pass, or at its exit or its
 * abort if it never passed - so that each begins with the agent's signal mask
 * and CPUs, and enables its own ring. */
static void start_helpers(void)
{
    for (uint32_t helper = 1; helper <= osoe_am.area.helpers; helper++) {
        struct osoe_am_owner *h = &osoe_am.owner[helper];
        if (h->started)
            continue;
        int r = pthread_create(&h->thread, NULL, helper_main, (void *)(uintptr_t)helper);
        if (r != 0)
            osoe_fatal("cannot start progress helper %u of %u: %s", helper,
                       osoe_am.area.helpers, strerror(r));
        h->started = true;
    }
}

/*
 * The agent's wait for helper `helper` to acknowledge its stop
 * (helper_request_model.py): the helper's count of turns read after the stop
 * was stored, and a nudge for each count seen. A helper that has turned twice
 * since without acknowledging has read the stop and ignored it - the turn in
 * progress at the post may have loaded the request before it, the next loads it
 * after - a defect, which ends the job naming it. One that does not turn at all
 * is waited for as every wait is, sweeping, under the no-progress rule, with a
 * notice: nothing here gives up because time has passed (§3.15).
 */
static void wait_acknowledged(uint32_t helper)
{
    struct osoe_am_owner *h = &osoe_am.owner[helper];
    uint64_t posted_at = atomic_load_explicit(&h->turns, memory_order_seq_cst);
    uint64_t nudged_at = UINT64_MAX;
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin_noticed(&watch, posted_at, osoe.bootstrap.no_progress_ns);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    while (atomic_load_explicit(&h->stop, memory_order_acquire) < HELPER_ACKNOWLEDGED) {
        uint64_t turns = atomic_load_explicit(&h->turns, memory_order_seq_cst);
        if (turns - posted_at >= 2) {
            if (atomic_load_explicit(&h->stop, memory_order_acquire) >= HELPER_ACKNOWLEDGED)
                break;
            osoe_fatal("shmem_finalize: progress helper %u of node %u has gone round its loop "
                       "%llu times since it was asked to stop, and has not answered: a defect "
                       "of the library's", helper, osoe.bootstrap.node,
                       (unsigned long long)(turns - posted_at));
        }
        if (turns != nudged_at) {
            nudged_at = turns;
            nudge(helper);
        }
        osoe_futex_wait(&h->stop, 0, &sweep, OSOE_FUTEX_PRIVATE);
        if (!osoe_deadline_expired(&sweep))
            continue;
        sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        osoe_sweep_expired("shmem_finalize");
        if (osoe_stall_watch_stalled(&watch, turns))
            osoe_fatal("shmem_finalize: no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): "
                       "progress helper %u of node %u has not gone round its loop since it "
                       "was asked to stop", (unsigned long long)osoe_stall_watch_seconds(&watch),
                       helper, osoe.bootstrap.node);
        if (osoe_stall_watch_notice_due(&watch))
            osoe_notice("shmem_finalize on node %u has waited %llu s for progress helper %u, "
                        "which has not gone round its loop since it was asked to stop; still "
                        "waiting", osoe.bootstrap.node,
                        (unsigned long long)osoe_stall_watch_waited_seconds(&watch), helper);
    }
}

/* Whether every helper is done. */
static bool helpers_done(void)
{
    for (uint32_t helper = 1; helper <= osoe_am.area.helpers; helper++)
        if (atomic_load_explicit(&osoe_am.owner[helper].stop, memory_order_acquire) < HELPER_DONE)
            return false;
    return true;
}

/*
 * The agent's wait for every helper to be done, on its own wake word, which a
 * helper bumps after saying so (say_stop): within `grace` on an abort, with
 * nothing else to look for; and otherwise as long as the helpers' closes take,
 * each bounded by its own no-progress rule (close_connections), sweeping.
 */
static void wait_helpers(const struct osoe_deadline *grace)
{
    _Atomic uint32_t *word = wake_word(0);
    for (;;) {
        uint32_t seen = atomic_load_explicit(word, memory_order_acquire);
        if (helpers_done() || (grace && osoe_deadline_expired(grace)))
            return;
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        osoe_futex_wait(word, seen, grace ? grace : &sweep, OSOE_FUTEX_SHARED);
        if (!grace && osoe_deadline_expired(&sweep))
            osoe_sweep_expired("shmem_finalize");
    }
}

static void join_helpers(void)
{
    for (uint32_t helper = 1; helper <= osoe_am.area.helpers; helper++) {
        struct osoe_am_owner *h = &osoe_am.owner[helper];
        if (!h->started)
            continue;
        int r = pthread_join(h->thread, NULL);
        if (r != 0)
            osoe_fatal("cannot join progress helper %u: %s", helper, strerror(r));
        h->started = false;
    }
}

/* On an abort, once the helpers have had their grace: each that is done is
 * returning, and is joined, so that it is no finished thread the process's end
 * leaves unjoined - ThreadSanitizer reports one; each that is not is left to
 * the process's end, detached, since joining it could wait for ever. */
static void leave_helpers(void)
{
    for (uint32_t helper = 1; helper <= osoe_am.area.helpers; helper++) {
        struct osoe_am_owner *h = &osoe_am.owner[helper];
        if (!h->started)
            continue;
        if (atomic_load_explicit(&h->stop, memory_order_acquire) >= HELPER_DONE)
            (void)pthread_join(h->thread, NULL);
        else
            (void)pthread_detach(h->thread);
        h->started = false;
    }
}

/*
 * The helpers' answers to the deadlock detector's views (§3.15;
 * deadlock_owners_model.py), and a new request to each for the next call. An
 * answer counts only if it answers the request the last call posted, so that
 * the second half of a sample takes a verdict from a pass begun after its first
 * half asked; and only for a request of its kind, or a quiet one, which says
 * idle too. Each helper's applied count, published by it alone, is read after
 * its answer, so the activity the half reads covers whatever the answer saw;
 * counts only grow, so an unchanged sum is every count unchanged.
 */
static bool helpers_say(uint32_t kind, uint64_t *applied)
{
    bool yes = true;
    for (uint32_t helper = 1; helper <= osoe_am.area.helpers; helper++) {
        struct osoe_am_owner *h = &osoe_am.owner[helper];
        uint32_t said = atomic_load_explicit(&h->answer, memory_order_acquire);
        *applied += osoe_counter_word_read(&osoe_am.progress.owner[helper].application_applied);
        uint32_t asked = request_kind(h->posted);
        yes = yes && h->posted != 0 && said == (h->posted | ANSWER_YES) &&
              (asked == kind || asked == REQUEST_QUIET);
        post(helper, kind);
    }
    return yes;
}

/* ---- the job's end ------------------------------------------------------ */

/* Whether world rank `rank` is one of this node's PEs. */
static bool local_rank(uint32_t rank)
{
    for (uint32_t i = 0; i < osoe.bootstrap.local_count; i++)
        if (osoe.bootstrap.gtids[i] == rank)
            return true;
    return false;
}

/*
 * The job is ending: tell every peer, on every wire, before this process ends
 * (abort.h) - owner 0 its own wires, and each helper its own, reading the
 * node's abort word at its next turn (helper_main), which the agent nudges it
 * to take, the declarer having woken the agents alone (node.c). The agent
 * waits for the helpers within its own grace. Called on the agent thread,
 * whose ring owner 0's is.
 */
void osoe_progress_host_abort(void)
{
    if (!osoe_am.lead)
        return;
    struct osoe_progress_owner *o = &osoe_am.progress.owner[0];
    start_helpers();                    /* an agent that never passed: each tells its own */
    for (uint32_t helper = 1; helper <= osoe_am.area.helpers; helper++)
        nudge(helper);
    struct osoe_deadline grace = osoe_deadline_in(UINT64_C(1000000000) * OSOE_ABORT_GRACE_SECONDS);
    abort_wires(o, &grace);
    wait_helpers(&grace);
    leave_helpers();
    uint32_t pe = 0;
    int status = EXIT_FAILURE;
    enum osoe_abort_disposition how = osoe_node_abort_read(&osoe.node, &pe, &status);
    /*
     * A global exit called on this node: the PE carrying its status leaves
     * last (abort.h), so this lead outlasts every other node first. Each other
     * lead ends only after its own node's PEs, and its end ends its stream, so
     * once every stream has ended nothing of the job runs but this node. Not
     * a node whose own goodbye named another caller: it carries that one, and
     * each waiting for the other would only run both to the bound. With
     * helpers, judged on the control wires alone, the ones owner 0 holds: a
     * process's end ends all its streams at once, and of two carriers the first
     * to begin tells the other on its control wire (carrier_model.py's
     * control_only).
     */
    if (how != OSOE_ABORT_GLOBAL_EXIT || !local_rank(pe))
        return;
    struct osoe_deadline last = osoe_deadline_in(UINT64_C(1000000000) *
                                                 OSOE_ABORT_LEAD_WAIT_SECONDS);
    while (!osoe_progress_peers_ended(o, pe) && !osoe_deadline_expired(&last))
        if (!osoe_progress_poll(o) && !osoe_progress_peers_ended(o, pe))
            osoe_ring_wait(o->ring, OSOE_SWEEP_NS);
}

/*
 * The lead's last turn on its agent thread: every helper is asked to stop -
 * each flushes and closes its own wires, beside the agent's close of the
 * control wires, each wire saying its own goodbye after its own last bytes (the
 * progress helpers proposal's §3.1) - and the agent waits for each to
 * acknowledge, then closes owner 0's, then waits for every helper to be done
 * and joins it.
 */
void osoe_progress_host_exit(void)
{
    if (!osoe_am.lead)
        return;
    struct osoe_progress_owner *o = &osoe_am.progress.owner[0];
    if (!o->ring->enabled)              /* an agent stopped before its first pass */
        osoe_ring_enable(o->ring);
    start_helpers();                    /* likewise: each closes its own wires */
    for (uint32_t helper = 1; helper <= osoe_am.area.helpers; helper++)
        post(helper, REQUEST_STOP);
    for (uint32_t helper = 1; helper <= osoe_am.area.helpers; helper++)
        wait_acknowledged(helper);
    /* A helper takes the unsent limit before it looks at its stop, so the
     * node's line is said here if no pass has said it. */
    say_unsent();
    flush_before_close(o);
    close_connections(o);
    wait_helpers(NULL);
    join_helpers();
}

void osoe_progress_host_limit_unsent(uint32_t bytes, bool by_hand, bool say)
{
    if (!osoe_am.lead)
        return;
    unsent_by_hand = by_hand;
    unsent_say = say;
    unsent_generation++;
    atomic_store_explicit(&unsent_request,
                          (uint64_t)unsent_generation << UNSENT_GENERATION_SHIFT |
                              ((uint64_t)bytes + 1),
                          memory_order_seq_cst);
    for (uint32_t owner = 0; owner <= osoe_am.area.helpers; owner++)
        wake_owner(owner);              /* each owner takes it for its own wires */
}

void osoe_progress_host_stop(void)
{
    if (osoe_am.lead) {
        free(osoe_am.owner);
        free(osoe_am.progress.owner);
        free(osoe_am.wire);
        free(osoe_am.control);
        free(osoe_am.control_busy);
        free(osoe_am.connection);
        free(osoe_am.busy);
        free(osoe_am.descriptor);
        free(osoe_am.team);
        free(osoe_am.pending);
        osoe_am.owner = NULL;
        osoe_am.progress.owner = NULL;
        osoe_am.wire = NULL;
        osoe_am.control = NULL;
        osoe_am.control_busy = NULL;
        osoe_am.connection = NULL;
        osoe_am.busy = NULL;
        osoe_am.descriptor = NULL;
        osoe_am.team = NULL;
        osoe_am.pending = NULL;
    }
    osoe_connection_area_close(&osoe_am.area);
    osoe_am.internode = false;
}

/*
 * The agent's pass: owner 0's, with the lead's own work besides - the requests
 * of its application thread, the silent-peer check and the arrivals posted for
 * the internode tree, the control plane's - and its half of the sleep protocol.
 * The unsent request is looked at again after the flag, as a writer's bit is.
 */
bool osoe_progress_host_poll(void)
{
    if (!osoe_am.lead)
        return false;
    struct osoe_progress_owner *o = &osoe_am.progress.owner[0];
    /* The first pass is on the agent thread, which enabling makes the ring's only
     * submitter (ring.h). It is also where the engine first meets the teams
     * whose records the node's lead wrote before this thread started, and
     * where the helpers start (start_helpers). */
    if (!o->ring->enabled) {
        osoe_ring_enable(o->ring);
        atomic_store_explicit(&o->metrics->tid, (int32_t)syscall(SYS_gettid),
                              memory_order_release);
        osoe_progress_meet_teams(&osoe_am.progress, OSOE_PREDEFINED_TEAMS);
        start_helpers();
    }
    lower_flag(0);
    take_unsent_request(0);
    say_unsent();
    if (silent_budget_ms) {
        uint64_t now = osoe_now_ns();
        if (now >= next_silent_check_ns) {
            next_silent_check_ns = now + silent_interval_ns;
            osoe_progress_check_silent(o, silent_budget_ms);
        }
    }
    bool moved = osoe_internode_am_take_arrivals();
    if (osoe_progress_poll(o) || moved)
        return true;
    if (raise_flag_and_look(0))
        return true;
    if (unsent_asked(0))
        return true;
    return false;
}

/* Owner 0's view, and with helpers each helper's latest answer (helpers_say),
 * which is the node's. */
bool osoe_progress_host_idle(uint64_t *applied)
{
    if (!osoe_am.lead) {
        *applied = 0;
        return true;
    }
    *applied = osoe_counter_word_read(&osoe_am.progress.owner[0].application_applied);
    bool idle = osoe_progress_idle(&osoe_am.progress.owner[0]);
    return helpers_say(REQUEST_IDLE, applied) && idle;
}

bool osoe_progress_host_quiet(uint64_t *applied)
{
    if (!osoe_am.lead) {
        *applied = 0;
        return true;
    }
    bool quiet = osoe_progress_quiet(&osoe_am.progress.owner[0], applied);
    return helpers_say(REQUEST_QUIET, applied) && quiet;
}

void osoe_progress_host_deadlock_send(uint32_t node, const struct osoe_deadlock_message *m)
{
    if (!osoe_am.lead)
        osoe_fatal("a deadlock detector message toward node %u from a PE that runs no engine",
                   node);
    osoe_progress_deadlock_send(&osoe_am.progress, node, m);
}

bool osoe_progress_host_wait(_Atomic uint32_t *wake, uint32_t seen)
{
    /* The lead waits in its ring, on its sockets and the word at once
     * (§3.10); every other agent of the job has only the word. Either way the
     * wait ends by a sweep interval at the latest (§4.10). */
    if (osoe_am.lead)
        return osoe_ring_wait_wake(&osoe_am.owner[0].ring, wake, seen, OSOE_SWEEP_NS);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    return osoe_futex_wait(wake, seen, &sweep, OSOE_FUTEX_SHARED) == 0;
}
