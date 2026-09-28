/* set-slice TID NS: ask the scheduler for an NS-nanosecond slice for thread TID,
 * a SCHED_OTHER thread staying SCHED_OTHER at its nice (EEVDF's custom slice,
 * sched_attr::sched_runtime, Linux 6.12 and later). Prints before and after. */
#define _GNU_SOURCE
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct attributes {
    uint32_t size, policy;
    uint64_t flags;
    int32_t nice;
    uint32_t priority;
    uint64_t runtime, deadline, period;
    uint32_t utilization_min, utilization_max;
};

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    pid_t tid = (pid_t)atoi(argv[1]);
    struct attributes a;
    memset(&a, 0, sizeof a);
    if (syscall(SYS_sched_getattr, tid, &a, sizeof a, 0) != 0) {
        perror("sched_getattr");
        return 1;
    }
    printf("tid %d before: policy %u nice %d slice %llu ns; ", tid, a.policy, a.nice,
           (unsigned long long)a.runtime);
    a.size = sizeof a;
    a.policy = SCHED_OTHER;
    a.flags = 0;
    a.runtime = strtoull(argv[2], NULL, 10);
    if (syscall(SYS_sched_setattr, tid, &a, 0) != 0) {
        perror("sched_setattr");
        return 1;
    }
    memset(&a, 0, sizeof a);
    syscall(SYS_sched_getattr, tid, &a, sizeof a, 0);
    printf("after: policy %u nice %d slice %llu ns\n", a.policy, a.nice, (unsigned long long)a.runtime);
    return 0;
}
