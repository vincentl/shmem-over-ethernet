/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Spec 9.1.1, 9.1.4, 9.1.5, 9.2.1, 9.2.2 and 9.1.13 in one sequence. */
#include <shmem.h>
#include <stdio.h>

static int failures;
#define EXPECT(what, cond) do { if (!(cond)) { failures++; \
    printf("PE %d: FAILED: %s\n", shmem_my_pe(), what); } } while (0)

int main(void)
{
    int flag = -1, level = -1, provided = -1;
    shmem_query_initialized(&flag);
    EXPECT("not initialized before init", flag == 0);
    shmem_query_thread(&level);            /* "may be called at any time" */

    EXPECT("init_thread returns 0", shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided) == 0);
    EXPECT("provided is a defined level",
           provided >= SHMEM_THREAD_SINGLE && provided <= SHMEM_THREAD_MULTIPLE);
    shmem_query_thread(&level);
    EXPECT("query_thread equals provided", level == provided);
    int first = provided;

    shmem_init();                          /* second, nested initialization */
    long *object = shmem_calloc(1, sizeof(long));
    shmem_finalize();                      /* not the last: a barrier, nothing freed */
    shmem_query_initialized(&flag);
    EXPECT("still initialized after a non-last finalize", flag != 0);
    long one = 1;
    shmem_putmem(object, &one, sizeof one, (shmem_my_pe() + 1) % shmem_n_pes());
    shmem_barrier_all();
    EXPECT("heap survives a non-last finalize", *object == 1);

    EXPECT("init_thread(SINGLE) nested returns 0",
           shmem_init_thread(SHMEM_THREAD_SINGLE, &provided) == 0);
    EXPECT("nested provided is at least what was asked", provided >= SHMEM_THREAD_SINGLE);
    shmem_query_thread(&level);
    EXPECT("query_thread equals the nested provided", level == provided);
    shmem_finalize();

    shmem_free(object);
    shmem_finalize();                      /* the last */
    shmem_query_initialized(&flag);
    EXPECT("uninitialized after the last finalize", flag == 0);

    /* Re-initialization (9.1.1), this time at a lower level, then start_pes on
     * top of it, which must not need a finalize of its own (9.1.13). */
    EXPECT("re-init returns 0", shmem_init_thread(SHMEM_THREAD_FUNNELED, &provided) == 0);
    EXPECT("re-init provides at least FUNNELED", provided >= SHMEM_THREAD_FUNNELED);
    object = shmem_malloc(sizeof(long));
    EXPECT("allocation after re-init", object != NULL);
    shmem_free(object);
    start_pes(0);
    start_pes(0);                          /* "has no subsequent effect" */
    shmem_finalize();                      /* matches the init_thread; start_pes finalizes at exit */
    shmem_query_initialized(&flag);
    EXPECT("start_pes keeps the library initialized until exit", flag != 0);
    EXPECT("_my_pe agrees", _my_pe() == shmem_my_pe() && _num_pes() == shmem_n_pes());
    if (shmem_my_pe() == 0)
        printf("lifecycle: %d failures (first provided level %d)\n", failures, first);
    return failures != 0;
}
