/*
 * shmem_init_thread and shmem_query_thread (spec §9.2; design §4.6).
 *
 * Two claims to check. The easy one is arithmetic: the level provided is the
 * least of what was asked and what osoe has, the four constants increase, and
 * the level is fixed by the first initialization of a session.
 *
 * The one worth a test is that SHMEM_THREAD_SERIALIZED is really provided -
 * that a second thread of a PE can call the library at all, so long as no two
 * calls overlap. That exercises §3.14's per-thread counter blocks (the second
 * thread claims its own) and, under the tsan preset, is the one place a race
 * between an application thread and the agent would be visible: ThreadSanitizer
 * instruments one process, which is exactly this case and not the cross-PE ones
 * (§8.2).
 *
 * Collectives stay on the main thread. The spec makes ordering collectives the
 * programmer's responsibility at SERIALIZED (§9.2 semantics item 5), and a
 * barrier from an arbitrary thread is the program's bug, not the library's.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { ROUNDS = 200 };

static pthread_mutex_t serialize = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *counter;
static int right;

/* Every library call from either thread happens under this mutex, which is
 * exactly what SHMEM_THREAD_SERIALIZED asks the program to arrange. */
static void *helper(void *arg)
{
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&serialize);
        shmem_uint64_atomic_add(counter, 1, right);
        pthread_mutex_unlock(&serialize);
    }
    return NULL;
}

int main(void)
{
    int level = -1;

    /* Callable before any initialization (§9.2.2). */
    shmem_query_thread(&level);
    CHECK(level >= SHMEM_THREAD_SINGLE && level <= SHMEM_THREAD_MULTIPLE);
    CHECK(SHMEM_THREAD_SINGLE < SHMEM_THREAD_FUNNELED);
    CHECK(SHMEM_THREAD_FUNNELED < SHMEM_THREAD_SERIALIZED);
    CHECK(SHMEM_THREAD_SERIALIZED < SHMEM_THREAD_MULTIPLE);

    /* More than osoe has: the answer is what it has, and it is not a failure. */
    int provided = -1;
    CHECK(shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided) == 0);
    CHECK(provided == SHMEM_THREAD_SERIALIZED);
    shmem_query_thread(&level);
    CHECK(level == provided);

    /* A nested initialization does not change the level, whatever it asks for
     * (§9.2.1). */
    int nested = -1;
    CHECK(shmem_init_thread(SHMEM_THREAD_SINGLE, &nested) == 0);
    CHECK(nested == SHMEM_THREAD_SERIALIZED);
    shmem_query_thread(&level);
    CHECK(level == SHMEM_THREAD_SERIALIZED);
    shmem_finalize();                   /* the nested one */

    int me = shmem_my_pe(), n = shmem_n_pes();
    right = (me + 1) % n;
    counter = shmem_malloc(sizeof *counter);
    CHECK(counter != NULL);
    *counter = 0;
    shmem_barrier_all();

    /* SERIALIZED for real: two threads of this PE, never concurrent. */
    pthread_t t;
    CHECK(pthread_create(&t, NULL, helper, NULL) == 0);
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&serialize);
        shmem_uint64_atomic_add(counter, 1, right);
        pthread_mutex_unlock(&serialize);
    }
    CHECK(pthread_join(t, NULL) == 0);

    shmem_barrier_all();
    /* Each PE's counter was incremented 2 * ROUNDS times by its left
     * neighbour's two threads. */
    CHECK(*counter == 2 * ROUNDS);

    shmem_free(counter);
    CHECK_INVARIANTS();
    shmem_finalize();

    /* A fresh session may ask for less, and get it. */
    CHECK(shmem_init_thread(SHMEM_THREAD_FUNNELED, &provided) == 0);
    CHECK(provided == SHMEM_THREAD_FUNNELED);
    shmem_query_thread(&level);
    CHECK(level == SHMEM_THREAD_FUNNELED);
    CHECK_INVARIANTS_SESSION(1);
    shmem_finalize();

    /* And after the last finalize, the answer is the one shmem_init gives. */
    shmem_query_thread(&level);
    CHECK(level == SHMEM_THREAD_SERIALIZED);
    return CHECK_DONE();
}
