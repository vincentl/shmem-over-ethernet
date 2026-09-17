/* Lifecycle: shmem_init, shmem_finalize, shmem_query_initialized, shmem_my_pe,
 * shmem_n_pes, including repeated and nested init/finalize (§6.4, spec §9.1). */
#include <stdlib.h>

#include <shmem.h>

#include "check.h"

static void check_running(int want_n)
{
    int flag = -1;
    shmem_query_initialized(&flag);
    CHECK(flag != 0);
    CHECK(shmem_n_pes() == want_n);
    CHECK(shmem_my_pe() >= 0 && shmem_my_pe() < want_n);
    const char *procid = getenv("OSOE_PROCID");
    CHECK(procid && shmem_my_pe() == atoi(procid));
}

int main(void)
{
    /* Read after the checks below would be too late without a launcher:
     * shmem_init must be the one to report a missing environment. */
    const char *tasks = getenv("OSOE_STEP_NUM_TASKS");
    int n = tasks ? atoi(tasks) : -1;
    int flag = -1;

    shmem_query_initialized(&flag);
    CHECK(flag == 0);

    for (int session = 0; session < 3; session++) {
        shmem_init();
        check_running(n);

        shmem_init();                   /* nested: counts, no new bootstrap */
        check_running(n);
        shmem_finalize();               /* non-last */
        check_running(n);

        shmem_finalize();               /* last: tears down */
        shmem_query_initialized(&flag);
        CHECK(flag == 0);
    }

    shmem_finalize();                   /* without init: no effect */
    shmem_query_initialized(&flag);
    CHECK(flag == 0);
    return CHECK_DONE();
}
