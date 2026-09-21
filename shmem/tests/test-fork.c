/* A child forked after shmem_init must not tear anything down (§6.4). */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

int main(void)
{
    shmem_init();

    pid_t child = fork();
    if (child == 0) {
        shmem_finalize();               /* must do nothing in a child */
        shmem_finalize();
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* The node's control segment has no name - it was removed during init
     * (§3.14) - and is still this job's: the child's finalizes did nothing to
     * it, which CHECK_INVARIANTS below shows by reading it through this PE's
     * own descriptor. */
    char path[128];
    snprintf(path, sizeof path, "/dev/shm/osoe.%s.%s.0.0", getenv("OSOE_JOB_ID"),
             getenv("OSOE_STEP_ID"));
    struct stat st;
    CHECK(stat(path, &st) != 0);
    int flag = 0;
    shmem_query_initialized(&flag);
    CHECK(flag != 0);

    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
