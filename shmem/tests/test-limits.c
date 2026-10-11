/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * shmem_init raises two of the process's soft resource limits to their hard
 * ones (init.c; the README's "What shmem_init changes in the process"): open
 * files, for a lead's sockets and pidfds, and locked memory, which io_uring
 * charges its rings to on Linux 6.14 and later and the zero-copy send its lent
 * pages to (the author's decision of 2026-10-11, after the ninth review's
 * P9-1). Each is lowered here first, below its hard limit, so that the raise is
 * seen whatever limits the shell gave - and, on a kernel that charges rings, a
 * job of two nodes would be refused its rings at a locked-memory limit of
 * nothing if init did not raise it.
 */
#include <stdbool.h>
#include <stdio.h>
#include <sys/resource.h>

#include <shmem.h>

#include "check.h"

/* Lower the soft limit to `low` if the hard one is above it; true if it was. */
static bool lower(int resource, rlim_t low)
{
    struct rlimit limit;
    CHECK_FATAL(getrlimit(resource, &limit) == 0);
    if (limit.rlim_max <= low)
        return false;
    limit.rlim_cur = low;
    CHECK_FATAL(setrlimit(resource, &limit) == 0);
    return true;
}

static void raised(int resource, const char *name)
{
    struct rlimit limit;
    CHECK_FATAL(getrlimit(resource, &limit) == 0);
    CHECK(limit.rlim_cur == limit.rlim_max);
    if (limit.rlim_cur != limit.rlim_max)
        fprintf(stderr, "%s: soft limit %llu, hard %llu after shmem_init\n", name,
                (unsigned long long)limit.rlim_cur, (unsigned long long)limit.rlim_max);
}

int main(void)
{
    bool files = lower(RLIMIT_NOFILE, 64);
    bool locked = lower(RLIMIT_MEMLOCK, 0);
    shmem_init();
    if (files)
        raised(RLIMIT_NOFILE, "RLIMIT_NOFILE");
    if (locked)
        raised(RLIMIT_MEMLOCK, "RLIMIT_MEMLOCK");
    shmem_barrier_all();
    if (shmem_my_pe() == 0)
        printf("LIMITS RAISED OK (open files %s, locked memory %s)\n",
               files ? "lowered and raised" : "already at the hard limit",
               locked ? "lowered and raised" : "already at the hard limit");
    shmem_finalize();
    return CHECK_DONE();
}
