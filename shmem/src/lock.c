/*
 * Distributed locks (spec §9.13; design §4.12): shmem_set_lock,
 * shmem_test_lock and shmem_clear_lock.
 *
 * **First come, first served, so a queue.** The spec guarantees waiting PEs
 * "set the lock in a first-come, first-served manner", which no test-and-set
 * with back-off gives; this is the MCS queue lock, model-checked in
 * tools/modelcheck/lock_model.py - mutual exclusion, the order of arrival, and
 * no state from which a PE cannot finish, against seven breakages that each
 * fail one of them.
 *
 * **The application's long is the implementation's, on every PE** - the spec
 * makes any other access to an in-use lock undefined - and it is two 32-bit
 * words. The first, on the lock's home PE, is the queue's tail: the last
 * waiter's PE + 1, or 0 when the lock is free. The second, on every PE, is that
 * PE's node in the queue: its successor's PE + 1, and GRANTED once its
 * predecessor hands the lock over. So a waiter spins on nothing: it waits on its
 * own word, which the region wait sleeps on (region-wait.h), and the only
 * remote traffic is the swap into the tail, one link, and one grant.
 *
 *   acquire  my node := 0; prev := swap(tail, me + 1); if prev: or(prev's node,
 *            me + 1), wait for GRANTED on my node, and-not(my node, GRANTED)
 *   release  quiet; n := my node's successor; if none: cas(tail, me + 1, 0)
 *            frees it, or else wait for the successor to finish linking; then
 *            or(n's node, GRANTED)
 *   test     my node := 0; cas(tail, 0, me + 1) - 0 if that took the lock
 *
 * **The home is spread by address**: the lock's offset in longs, modulo the
 * PEs, so an array of locks - uts's one per PE - puts its tails on as many PEs
 * as it has locks, not all on PE 0. Every PE computes it the same way from the
 * symmetric offset.
 *
 * **Each PE remembers which locks it holds** (a short list: a PE holds few at
 * once), so a clear of a lock it does not hold, and a set of one it holds
 * already - which the spec leaves undefined - end the job by name, rather than
 * hanging in a queue nobody will grant.
 *
 * Every word is touched by the transport's four-byte rmw or its coherent load,
 * which are exclusive against one another (§4.10); every rmw on this PE's own
 * node fetches, so it is complete when it returns on any path, force-remote
 * included.
 *
 * Complexity: O(1) remote operations per acquire and per release, and O(1)
 * wakes; O(locks held) to find a lock in the held list.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <pshmem.h>
#include <shmem.h>

#include "export.h"
#include "fatal.h"
#include "lock.h"
#include "region-wait.h"
#include "state.h"
#include "symmetric.h"
#include "transport.h"

enum { GRANTED = 0x80000000u, SUCCESSOR = 0x7fffffffu };

/* Where a lock's two words are, and whose tail it is. */
struct lock_place {
    uint64_t tail;                      /* offset of the tail word, on the home PE */
    uint64_t node;                      /* offset of this PE's node word */
    int home;
};

static struct lock_place place_of(const char *routine, long *lock)
{
    uint64_t offset = osoe_symmetric_word_offset(routine, lock, sizeof(long));
    struct lock_place p = {
        .tail = offset,
        .node = offset + sizeof(uint32_t),
        .home = (int)((offset / sizeof(long)) % osoe.bootstrap.world_size),
    };
    return p;
}

/* The locks this PE holds, by offset. One thread at a time calls in (the
 * thread levels osoe provides, §4.6), so the list needs no lock of its own. */
static uint64_t *held;
static size_t held_count, held_capacity;

static bool holds(uint64_t offset)
{
    for (size_t i = 0; i < held_count; i++)
        if (held[i] == offset)
            return true;
    return false;
}

static void remember(uint64_t offset)
{
    if (held_count == held_capacity) {
        size_t capacity = held_capacity ? 2 * held_capacity : 8;
        uint64_t *grown = realloc(held, capacity * sizeof *grown);
        if (!grown)
            osoe_fatal("out of memory for the locks this PE holds");
        held = grown;
        held_capacity = capacity;
    }
    held[held_count++] = offset;
}

static void forget(uint64_t offset)
{
    for (size_t i = 0; i < held_count; i++)
        if (held[i] == offset) {
            held[i] = held[--held_count];
            return;
        }
}

static uint32_t rmw(int pe, uint64_t offset, enum osoe_rmw_op op, uint32_t operand,
                    uint32_t compare, unsigned flags)
{
    return (uint32_t)osoe_transport_rmw(osoe_transport_context_default(), pe,
                                        OSOE_REGION_HEAP, offset, op, 4, operand, compare,
                                        flags);
}

static void refuse_held(const char *routine, const struct lock_place *p)
{
    if (holds(p->tail))
        osoe_fatal("%s: this PE holds the lock already, and OpenSHMEM's lock is not "
                   "reentrant (spec §9.13)", routine);
}

OSOE_API void pshmem_set_lock(long *lock)
{
    const char *routine = "shmem_set_lock";
    struct lock_place p = place_of(routine, lock);
    refuse_held(routine, &p);
    int me = (int)osoe.bootstrap.rank;

    /* My node is empty before anyone can find me through the tail. */
    rmw(me, p.node, OSOE_RMW_WRITE, 0, 0, OSOE_TRANSPORT_FETCH);
    uint32_t previous = rmw(p.home, p.tail, OSOE_RMW_WRITE, (uint32_t)me + 1, 0,
                            OSOE_TRANSPORT_FETCH);
    if (previous != 0) {
        rmw((int)previous - 1, p.node, OSOE_RMW_OR, (uint32_t)me + 1, 0, 0);
        osoe_region_wait(routine, OSOE_REGION_HEAP, p.node, 4, false,
                         OSOE_COMPARISON_GREATER_OR_EQUAL, GRANTED, OSOE_BLOCKED_DETAIL_PE,
                         previous - 1);
        /* And-not, not a store: a successor may have linked meanwhile. */
        rmw(me, p.node, OSOE_RMW_AND, SUCCESSOR, 0, OSOE_TRANSPORT_FETCH);
    }
    remember(p.tail);
}

OSOE_API int pshmem_test_lock(long *lock)
{
    const char *routine = "shmem_test_lock";
    struct lock_place p = place_of(routine, lock);
    refuse_held(routine, &p);
    int me = (int)osoe.bootstrap.rank;

    rmw(me, p.node, OSOE_RMW_WRITE, 0, 0, OSOE_TRANSPORT_FETCH);
    if (rmw(p.home, p.tail, OSOE_RMW_CSWAP, (uint32_t)me + 1, 0, OSOE_TRANSPORT_FETCH) != 0)
        return 1;
    remember(p.tail);
    return 0;
}

OSOE_API void pshmem_clear_lock(long *lock)
{
    const char *routine = "shmem_clear_lock";
    struct lock_place p = place_of(routine, lock);
    if (!holds(p.tail))
        osoe_fatal("%s: this PE does not hold the lock it clears", routine);
    int me = (int)osoe.bootstrap.rank;

    /* "after performing a quiet operation on the default context": the
     * critical region's writes complete before anyone can enter it. */
    pshmem_quiet();
    uint32_t successor = (uint32_t)osoe_transport_load(OSOE_REGION_HEAP, p.node, 4) & SUCCESSOR;
    if (successor == 0) {
        if (rmw(p.home, p.tail, OSOE_RMW_CSWAP, 0, (uint32_t)me + 1, OSOE_TRANSPORT_FETCH) ==
            (uint32_t)me + 1) {
            forget(p.tail);
            return;
        }
        /* Someone swapped in behind this PE and has not linked yet; who, the
         * link will say, so the wait records nothing it cannot know. */
        successor = (uint32_t)osoe_region_wait(routine, OSOE_REGION_HEAP, p.node, 4, false,
                                               OSOE_COMPARISON_NOT_EQUAL, 0,
                                               OSOE_BLOCKED_DETAIL_NONE, 0) & SUCCESSOR;
    }
    rmw((int)successor - 1, p.node, OSOE_RMW_OR, GRANTED, 0, 0);
    forget(p.tail);
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
