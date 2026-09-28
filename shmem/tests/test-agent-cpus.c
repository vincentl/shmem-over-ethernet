/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Where each PE's agent may run (agent.c, OSOE_AGENT_CPUS): each PE binds
 * itself to one CPU before shmem_init, as Slurm binds a node's tasks when they
 * fill it - local id k to the k-th CPU it may use, round the CPUs there are -
 * and then finds its agent, the thread named osoe-agent. By default the agent
 * runs on its own PE's one CPU; with OSOE_AGENT_CPUS=node, on the CPUs of
 * every PE of its node, and on no other.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <shmem.h>

#define CHECK(c)                                                                            \
    do {                                                                                    \
        if (!(c)) {                                                                         \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);           \
            exit(1);                                                                        \
        }                                                                                   \
    } while (0)

/* The k-th CPU of `set`, counting round. */
static int nth_cpu(const cpu_set_t *set, int k)
{
    int count = CPU_COUNT(set);
    CHECK(count > 0);
    k %= count;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, set) && k-- == 0)
            return cpu;
    return -1;
}

/* The thread of this process named osoe-agent. */
static pid_t agent_thread(void)
{
    DIR *tasks = opendir("/proc/self/task");
    CHECK(tasks);
    pid_t found = 0;
    for (struct dirent *e; (e = readdir(tasks));) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char path[300], comm[32] = "";
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            if (!fgets(comm, sizeof comm, f))
                comm[0] = '\0';
            fclose(f);
        }
        if (strcmp(comm, "osoe-agent\n") == 0) {
            CHECK(found == 0);
            found = (pid_t)atoi(e->d_name);
        }
    }
    closedir(tasks);
    return found;
}

int main(void)
{
    const char *local = getenv("OSOE_LOCALID");
    CHECK(local);
    cpu_set_t may, own;
    CHECK(sched_getaffinity(0, sizeof may, &may) == 0);
    CPU_ZERO(&own);
    CPU_SET(nth_cpu(&may, atoi(local)), &own);
    CHECK(sched_setaffinity(0, sizeof own, &own) == 0);

    shmem_init();
    int locals = shmem_team_n_pes(SHMEM_TEAM_SHARED);
    cpu_set_t node;
    CPU_ZERO(&node);
    for (int k = 0; k < locals; k++)
        CPU_SET(nth_cpu(&may, k), &node);
    const char *mode = getenv("OSOE_AGENT_CPUS");
    const cpu_set_t *want = mode && strcmp(mode, "node") == 0 ? &node : &own;

    pid_t agent = agent_thread();
    CHECK(agent != 0);
    cpu_set_t has;
    CHECK(sched_getaffinity(agent, sizeof has, &has) == 0);
    CHECK(CPU_EQUAL(&has, want));
    cpu_set_t mine;                     /* the PE's own thread keeps its binding */
    CHECK(sched_getaffinity(0, sizeof mine, &mine) == 0);
    CHECK(CPU_EQUAL(&mine, &own));

    shmem_barrier_all();
    if (shmem_my_pe() == 0)
        printf("AGENT CPUS OK: %d local PEs, the agent on %d CPUs\n", locals, CPU_COUNT(want));
    shmem_finalize();
    return 0;
}
