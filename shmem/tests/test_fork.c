/* A child forked after shmem_init must not tear anything down (§6.4). */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <shmem.h>

#include "check.h"

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

    /* The node's control segment still has its name (§6.2). */
    char path[128];
    snprintf(path, sizeof path, "/dev/shm/osoe.%s.%s.0.0", getenv("OSOE_JOB_ID"),
             getenv("OSOE_STEP_ID"));
    struct stat st;
    CHECK(stat(path, &st) == 0);
    int flag = 0;
    shmem_query_initialized(&flag);
    CHECK(flag != 0);

    shmem_finalize();
    return CHECK_DONE();
}
