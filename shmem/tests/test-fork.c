/* SPDX-License-Identifier: LGPL-3.0-or-later */
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

    /* Neither the node's control segment nor any of its heaps has a name, as
     * soon as this PE's own init has returned - no barrier needed: the lead
     * removes them before it publishes that its agent runs, and every other
     * PE waits for that before it returns (§6.2, §3.15). The segment is still
     * this job's: the child's finalizes did nothing to it, which
     * CHECK_INVARIANTS below shows by reading it through this PE's own
     * descriptor. */
    char name[OSOE_SEGMENT_NAME_MAX], path[OSOE_SEGMENT_NAME_MAX + 32];
    osoe_test_segment_name(name, sizeof name, 0);
    struct stat st;
    snprintf(path, sizeof path, "/dev/shm%s", name);
    CHECK(stat(path, &st) != 0);
    for (int i = 0; i < shmem_team_n_pes(SHMEM_TEAM_SHARED); i++) {
        snprintf(path, sizeof path, "/dev/shm%s.heap.%d", name, i);
        CHECK(stat(path, &st) != 0);
    }
    int flag = 0;
    shmem_query_initialized(&flag);
    CHECK(flag != 0);

    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
