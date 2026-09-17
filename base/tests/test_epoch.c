/* Epoch counters: equality waits and bumps (§5.4). */
#include <pthread.h>
#include <stdint.h>
#include <time.h>

#include "base/epoch.h"
#include "base/counters.h"
#include "base/order.h"
#include "check.h"

enum { THREADS = 4, ROUNDS = 2000 };

static struct osoe_epoch arrived, released;
static _Atomic uint32_t reached[THREADS];

/* The §5.5 single-node barrier: each member arrives; the member whose arrival
 * reaches the epoch's target releases it; everyone waits for the release. */
static void *member(void *arg)
{
    unsigned me = (unsigned)(uintptr_t)arg;
    for (uint32_t k = 0; k < ROUNDS; k++) {
        if (osoe_epoch_bump_eq(&arrived, (k + 1) * THREADS)) {
            CHECK(osoe_epoch_bump_eq(&released, k + 1));
            osoe_epoch_wake_all(&released);
        }
        osoe_epoch_wait_eq(&released, k + 1, "test_epoch");
        atomic_store_explicit(&reached[me], k + 1, memory_order_release);
    }
    return NULL;
}

static void *late_bumper(void *arg)
{
    struct osoe_epoch *e = arg;
    struct timespec ts = { 0, 20 * 1000 * 1000 };
    for (int i = 0; i < 3; i++) {
        nanosleep(&ts, NULL);
        osoe_epoch_bump_eq(e, 0);
        osoe_epoch_wake_all(e);
    }
    return NULL;
}

int main(void)
{
    struct osoe_epoch e;
    osoe_epoch_init(&e);
    CHECK(!osoe_epoch_bump_eq(&e, 2));
    CHECK(osoe_epoch_bump_eq(&e, 2));
    osoe_epoch_wait_eq(&e, 2, "test_epoch");              /* already equal: returns */

    /* Wraparound: equality holds across 2^32. */
    struct osoe_epoch w;
    atomic_store_explicit(&w.count_compare_only_by_equality, UINT32_MAX, memory_order_release);
    CHECK(osoe_epoch_bump_eq(&w, 0));

    /* A waiter blocks until the counter reaches exactly its target. */
    struct osoe_epoch slow;
    osoe_epoch_init(&slow);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, late_bumper, &slow) == 0);
    osoe_epoch_wait_eq(&slow, 3, "test_epoch");
    pthread_join(t, NULL);

    /* Many barrier epochs across threads with skew between them. */
    osoe_epoch_init(&arrived);
    osoe_epoch_init(&released);
    pthread_t th[THREADS];
    for (unsigned i = 0; i < THREADS; i++)
        CHECK(pthread_create(&th[i], NULL, member, (void *)(uintptr_t)i) == 0);
    for (unsigned i = 0; i < THREADS; i++)
        pthread_join(th[i], NULL);
    for (unsigned i = 0; i < THREADS; i++)
        CHECK(atomic_load_explicit(&reached[i], memory_order_acquire) == ROUNDS);
    CHECK(osoe_epoch_bump_eq(&released, ROUNDS + 1));    /* exactly ROUNDS releases */

    /* Counter blocks: single-writer add and read. */
    struct osoe_counter_block b = { 0 };
    osoe_counter_add(&b, 0, 5);
    osoe_counter_add(&b, 0, 7);
    osoe_counter_add(&b, OSOE_COUNTERS_PER_BLOCK - 1, 1);
    CHECK(osoe_counter_read(&b, 0) == 12);
    CHECK(osoe_counter_read(&b, OSOE_COUNTERS_PER_BLOCK - 1) == 1);

    osoe_serializing_store();               /* links and runs */
    return CHECK_DONE();
}
