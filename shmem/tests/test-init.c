/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Lifecycle: shmem_init, shmem_finalize, shmem_query_initialized, shmem_my_pe,
 * shmem_n_pes, including repeated and nested init/finalize (§6.4, spec §9.1). */
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

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

int main(int argc, char **argv)
{
    /* How many sessions: 3, or a number given alone - 40 for P19's test, whose
     * 33rd found no candidate port free before SO_REUSEADDR. */
    int sessions = 3;
    if (argc == 2 && argv[1][0] && strspn(argv[1], "0123456789") == strlen(argv[1]))
        sessions = atoi(argv[1]);
    /* Read after the checks below would be too late without a launcher:
     * shmem_init must be the one to report a missing environment. */
    const char *tasks = getenv("OSOE_STEP_NUM_TASKS");
    int n = tasks ? atoi(tasks) : -1;
    int flag = -1;

    shmem_query_initialized(&flag);
    CHECK(flag == 0);

    for (int session = 0; session < sessions; session++) {
        shmem_init();
        check_running(n);

        shmem_init();                   /* nested: counts, no new bootstrap */
        check_running(n);
        shmem_finalize();               /* non-last */
        check_running(n);

        CHECK_INVARIANTS_SESSION((unsigned)session);
        shmem_finalize();               /* last: tears down */
        shmem_query_initialized(&flag);
        CHECK(flag == 0);
    }

    shmem_finalize();                   /* without init: no effect */
    shmem_query_initialized(&flag);
    CHECK(flag == 0);
    return CHECK_DONE();
}
