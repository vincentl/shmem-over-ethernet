/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The agent's scheduler slice (§3.11; the night of 2026-09-27, item 1): every
 * PE's agent thread asks for OSOE_AGENT_SLICE_US microseconds, 100 by default,
 * through EEVDF's custom slice. Each PE finds its own agent - the thread of its
 * process that is not the main one - and reads the slice back with
 * sched_getattr. A kernel without custom slices (before Linux 6.12) reports
 * none for the main thread either, and then there is nothing to check.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <shmem.h>

#include "check.h"

struct scheduling_attributes {
    uint32_t size, policy;
    uint64_t flags;
    int32_t nice;
    uint32_t priority;
    uint64_t runtime, deadline, period;
    uint32_t utilization_minimum, utilization_maximum;
};

static uint64_t slice_of(pid_t tid)
{
    struct scheduling_attributes a;
    memset(&a, 0, sizeof a);
    if (syscall(SYS_sched_getattr, tid, &a, sizeof a, 0) != 0)
        return 0;
    return a.runtime;
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe();
    const char *setting = getenv("OSOE_AGENT_SLICE_US");
    uint64_t want = (setting ? strtoull(setting, NULL, 10) : 100) * 1000;
    uint64_t main_slice = slice_of(0);
    if (main_slice == 0) {
        if (me == 0)
            printf("no custom slices on this kernel: nothing to check\n");
    } else {
        pid_t self = (pid_t)getpid(), agent = 0;
        DIR *tasks = opendir("/proc/self/task");
        CHECK_FATAL(tasks != NULL);
        for (struct dirent *e; (e = readdir(tasks));) {
            pid_t tid = (pid_t)atoi(e->d_name);
            if (tid > 0 && tid != self)
                agent = tid;
        }
        closedir(tasks);
        CHECK(agent != 0);
        uint64_t got = slice_of(agent);
        /* 0 asks for the kernel's own: the main thread's, which it never set. */
        uint64_t expect = want ? want : main_slice;
        if (got != expect)
            fprintf(stderr, "PE %d: the agent's slice is %llu ns, not %llu\n", me,
                    (unsigned long long)got, (unsigned long long)expect);
        CHECK(got == expect);
    }
    shmem_finalize();
    return CHECK_DONE();
}
