/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Distributed locks (spec §9.13; design §4.12): shmem_set_lock,
 * shmem_test_lock and shmem_clear_lock.
 *
 * **First come, first served, so a queue.** The spec guarantees waiting PEs
 * "set the lock in a first-come, first-served manner", which no test-and-set
 * with back-off gives; this is the MCS queue lock, model-checked in
 * tools/modelcheck/lock_model.py - mutual exclusion, the order of arrival, and
 * no state from which a PE cannot finish, against eight breakages that each
 * fail one of them.
 *
 * **The application's long is the implementation's, on every PE** - the spec
 * makes any other access to an in-use lock undefined - and it is two 32-bit
 * words. The first, on the lock's home PE, is the queue's tail: the last
 * waiter's PE + 1, or 0 when the lock is free. The second, on every PE, is that
 * PE's node in the queue: its successor's PE + 1, and GRANTED once its
 * predecessor hands the lock over. So a waiter spins on nothing: it waits on its
 * own word, which the region wait sleeps on (region-wait.h), and the only
 * remote traffic is the swap into the tail, one link and one grant - and, for a
 * release that finds no successor, the compare-and-swap that frees the tail.
 *
 *   acquire  my node := 0; prev := swap(tail, me + 1); if prev: or(prev's node,
 *            me + 1), wait for GRANTED on my node, and-not(my node, GRANTED)
 *   release  quiet; n := my node's successor; if none: cas(tail, me + 1, 0)
 *            frees it, or else wait for my node to be nonzero - the successor
 *            still linking - and take n from that; then or(n's node, GRANTED)
 *   test     my node := 0; cas(tail, 0, me + 1) - 0 if that took the lock
 *
 * The release's wait is for the whole word to be nonzero, which is the link
 * only because the and-not cleared GRANTED when this PE entered: the model
 * checks that, and a word holding GRANTED and no successor here is fatal.
 *
 * **Only the home's tail must be zero when a lock is first used** - each PE
 * zeroes its own node before it can be found - and a tail holding what the
 * library cannot have written there, the mark of a lock not zeroed on its home
 * PE, is fatal by name rather than a link sent to no waiter.
 *
 * **The home is spread by a hash of the address**: the lock's region and
 * offset in longs, mixed, modulo the PEs, so locks at nearby offsets - an
 * array of them, or several allocated one after another - put their tails on
 * PEs spread over the job and so over its nodes, not on the first few PEs,
 * which a block map puts all on node 0. Every PE computes it the same way from
 * the symmetric location. A lock may be in the heap or among the program's
 * statics (§4.3) - the spec's own examples declare `static long lock = 0;`.
 *
 * **Each PE remembers which locks it holds** (a short list: a PE holds few at
 * once), so a clear of a lock it does not hold, and a set or a test of one it
 * holds already - which the spec leaves undefined - end the job by name, rather
 * than hanging in a queue nobody will grant or wiping a successor's link.
 *
 * Every word is touched by the transport's four-byte rmw or its coherent load,
 * which are exclusive against one another (§4.10); every rmw on this PE's own
 * node fetches, so it is complete when it returns on every backend, including
 * one that sends an rmw on the PE's own memory through a NIC.
 *
 * Complexity: O(1) remote operations per acquire and per release, and O(1)
 * wakes; O(locks held) to find a lock in the held list.
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <pshmem.h>
#include <shmem.h>

#include "base/print-line.h"
#include "blocked.h"
#include "export.h"
#include "fatal.h"
#include "lock.h"
#include "region-wait.h"
#include "state.h"
#include "symmetric.h"
#include "transport.h"

/* A node word's two parts. Macros, not an enumeration: GRANTED does not fit
 * an int. */
#define GRANTED   UINT32_C(0x80000000)
#define SUCCESSOR UINT32_C(0x7fffffff)

_Static_assert(sizeof(long) == 2 * sizeof(uint32_t),
               "a lock's long holds the tail and the node word side by side");

/* Where a lock's two words are - in the heap or among the program's statics
 * (§4.3), the same region on every PE - and whose tail it is. */
struct lock_place {
    enum osoe_region region;
    uint64_t tail_offset;               /* the tail word, on the home PE */
    uint64_t node_offset;               /* this PE's node word */
    uint64_t key;                       /* the lock, in the held list */
    int home;
};

/* splitmix64's finalizer: every bit of the offset moves every bit of the
 * result, so nearby offsets - which the allocator aligns to 16 bytes - spread. */
static uint64_t mix(uint64_t x)
{
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

static struct lock_place place_of(const char *routine, long *lock)
{
    struct osoe_symmetric_location at = osoe_symmetric_word_locate(routine, lock, sizeof(long));
    /* Offsets repeat from one region to the next, so the region is in the key
     * and the hash; no region's offsets reach the top two bits. */
    uint64_t key = (uint64_t)at.region << 62 | at.offset / sizeof(long);
    struct lock_place p = {
        .region = at.region,
        .tail_offset = at.offset,
        .node_offset = at.offset + sizeof(uint32_t),
        .key = key,
        .home = (int)(mix(key) % osoe.bootstrap.world_size),
    };
    return p;
}

/* The locks this PE holds, by key. Under SHMEM_THREAD_MULTIPLE several threads
 * of the PE may call in at once, so the list takes a mutex (phase 1 step 8
 * slice 2): set_lock and clear_lock already cost a network round trip, so it
 * costs nothing measurable. The lock is the PE's, as the spec's locks are. */
static uint64_t *held;
static size_t held_count, held_capacity;
static pthread_mutex_t held_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool holds(uint64_t key)
{
    pthread_mutex_lock(&held_mutex);
    bool found = false;
    for (size_t i = 0; i < held_count && !found; i++)
        found = held[i] == key;
    pthread_mutex_unlock(&held_mutex);
    return found;
}

static void remember(uint64_t key)
{
    pthread_mutex_lock(&held_mutex);
    if (held_count == held_capacity) {
        size_t capacity = held_capacity ? 2 * held_capacity : 8;
        uint64_t *grown = realloc(held, capacity * sizeof *grown);
        if (!grown)
            osoe_fatal("out of memory for the locks this PE holds");
        held = grown;
        held_capacity = capacity;
    }
    held[held_count++] = key;
    pthread_mutex_unlock(&held_mutex);
}

static void forget(uint64_t key)
{
    pthread_mutex_lock(&held_mutex);
    for (size_t i = 0; i < held_count; i++)
        if (held[i] == key) {
            held[i] = held[--held_count];
            break;
        }
    pthread_mutex_unlock(&held_mutex);
}

static uint32_t rmw(const struct lock_place *p, int pe, uint64_t offset, enum osoe_rmw_op op,
                    uint32_t operand, uint32_t compare, unsigned flags)
{
    return (uint32_t)osoe_transport_rmw(osoe_transport_context_default(), pe, p->region,
                                        offset, op, 4, operand, compare, flags);
}

static void refuse_held(const char *routine, const struct lock_place *p)
{
    if (holds(p->key))
        osoe_fatal("%s: this PE holds the lock already, and OpenSHMEM's lock is not "
                   "reentrant (spec §9.13)", routine);
}

/* A tail naming no PE of the job, or naming this PE, which is not waiting: the
 * library cannot have written either, so the lock was not zero on its home PE
 * before its first use. */
static void refuse_uninitialized(const char *routine, uint32_t tail, int me)
{
    if (tail > osoe.bootstrap.world_size || tail == (uint32_t)me + 1)
        osoe_fatal("%s: the lock's queue names PE %ld, which is not waiting for it; a "
                   "lock must be zero on every PE before its first use (spec §9.13)",
                   routine, (long)tail - 1);
}

OSOE_API void pshmem_set_lock(long *lock)
{
    const char *routine = "shmem_set_lock";
    struct lock_place p = place_of(routine, lock);
    refuse_held(routine, &p);
    int me = (int)osoe.bootstrap.rank;

    /* My node is empty before anyone can find me through the tail. */
    rmw(&p, me, p.node_offset, OSOE_RMW_WRITE, 0, 0, OSOE_TRANSPORT_FETCH);
    uint32_t previous = rmw(&p, p.home, p.tail_offset, OSOE_RMW_WRITE, (uint32_t)me + 1, 0,
                            OSOE_TRANSPORT_FETCH);
    if (previous != 0) {
        refuse_uninitialized(routine, previous, me);
        rmw(&p, (int)previous - 1, p.node_offset, OSOE_RMW_OR, (uint32_t)me + 1, 0, 0);
        osoe_region_wait(routine, p.region, p.node_offset, 4, false,
                         OSOE_COMPARISON_GREATER_OR_EQUAL, GRANTED, OSOE_BLOCKED_DETAIL_PE,
                         previous - 1);
        /* And-not, not a store: a successor may have linked meanwhile. */
        rmw(&p, me, p.node_offset, OSOE_RMW_AND, SUCCESSOR, 0, OSOE_TRANSPORT_FETCH);
    }
    remember(p.key);
}

OSOE_API int pshmem_test_lock(long *lock)
{
    const char *routine = "shmem_test_lock";
    struct lock_place p = place_of(routine, lock);
    refuse_held(routine, &p);
    int me = (int)osoe.bootstrap.rank;

    rmw(&p, me, p.node_offset, OSOE_RMW_WRITE, 0, 0, OSOE_TRANSPORT_FETCH);
    uint32_t tail = rmw(&p, p.home, p.tail_offset, OSOE_RMW_CSWAP, (uint32_t)me + 1, 0,
                        OSOE_TRANSPORT_FETCH);
    if (tail != 0) {
        refuse_uninitialized(routine, tail, me);
        return 1;
    }
    remember(p.key);
    return 0;
}

OSOE_API void pshmem_clear_lock(long *lock)
{
    const char *routine = "shmem_clear_lock";
    struct lock_place p = place_of(routine, lock);
    if (!holds(p.key))
        osoe_fatal("%s: this PE does not hold the lock it clears", routine);
    int me = (int)osoe.bootstrap.rank;

    /* "after performing a quiet operation on the default context": the
     * critical region's writes complete before anyone can enter it. The
     * library's quiet, as a barrier's is, so the application's shmem_quiet
     * count stays its own and a PE stuck here is blocked in shmem_clear_lock. */
    osoe_blocked_enter(routine, OSOE_BLOCKED_DETAIL_NONE, 0);
    osoe_transport_wait(osoe_transport_context_default(), NULL, 0, "shmem_quiet");
    osoe_blocked_leave();

    uint32_t node = (uint32_t)osoe_transport_load(p.region, p.node_offset, 4);
    uint32_t successor = node & SUCCESSOR;
    if (successor == 0) {
        if (rmw(&p, p.home, p.tail_offset, OSOE_RMW_CSWAP, 0, (uint32_t)me + 1,
                OSOE_TRANSPORT_FETCH) == (uint32_t)me + 1) {
            forget(p.key);
            return;
        }
        /* Someone swapped in behind this PE and has not linked yet; who, the
         * link will say, so the wait records nothing it cannot know. */
        node = (uint32_t)osoe_region_wait(routine, p.region, p.node_offset, 4, false,
                                          OSOE_COMPARISON_NOT_EQUAL, 0,
                                          OSOE_BLOCKED_DETAIL_NONE, 0);
        successor = node & SUCCESSOR;
    }
    if (successor == 0 || successor > osoe.bootstrap.world_size)
        osoe_fatal("%s: this PE's queue node reads %#x, which no lock routine leaves while "
                   "the lock is held; the lock was written other than through them "
                   "(spec §9.13)", routine, (unsigned)node);
    rmw(&p, (int)successor - 1, p.node_offset, OSOE_RMW_OR, GRANTED, 0, 0);
    forget(p.key);
}

void osoe_lock_warn_held(void)
{
    if (held_count)
        osoe_print_line("osoe: PE %u: shmem_finalize while holding %zu lock%s; a PE waiting "
                        "for one never reaches the barrier this finalize waits in (spec §9.13)\n",
                        osoe.bootstrap.rank, held_count, held_count == 1 ? "" : "s");
}

void osoe_lock_session_end(void)
{
    free(held);
    held = NULL;
    held_count = held_capacity = 0;
}

/* The weak `shmem_` aliases (export.h). */
OSOE_ALIAS(set_lock)
OSOE_ALIAS(test_lock)
OSOE_ALIAS(clear_lock)
