/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Part 4's stream (docs/fast-link.md, §5): every PE puts, or gets, BYTES-byte
 * blocks to the PEs of every other node in turn for SECONDS, a quiet each time
 * 8 MiB has gone, so that every peer of every node carries traffic - a node's
 * progress helpers divide its peers, not its streams (owner-map.h), and one
 * stream between two nodes keeps one helper busy whatever their count.
 *
 * PE 0 prints a line for each node and one for the job:
 *
 *   multistream node=N pes=P mib_per_s=R progress_cpu_s=C program_cpu_s=Q
 *               progress_cpu_s_per_gib=G
 *   multistream job mode=put bytes=B seconds=S nodes=N pes=P helpers=H
 *               mib_per_s=R progress_cpu_s_per_gib=G
 *
 * where progress is the CPU the node's threads named osoe-agent and
 * osoe-helper-N spent over the window, read from /proc/self/task, and program
 * the rest of its PEs' threads: a byte's cost in the library's progress, which
 * the helpers move off the PEs' cores. A node's rate counts what its PEs sent
 * (put) or fetched (get). The helper count printed is OSOE_PROGRESS_HELPERS as
 * the job was given it.
 *
 * MULTISTREAM_STAT, if set, is osoe-stat's path: PE 0 prints its node's
 * counters (osoe-stat -d -j) before and after the window, between marker
 * lines, for the zero-copy send's and the receive pool's counts.
 *
 * usage: multistream put|get BYTES SECONDS
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <shmem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* Nanoseconds on a CPU of this process's threads: those that run the library's
 * progress (osoe-agent, osoe-helper-N) in progress, the rest in program. Read
 * from schedstat, or from stat's ticks where the kernel has no schedstat. */
static void thread_cpu(uint64_t *progress, uint64_t *program)
{
    *progress = *program = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d)
        return;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        if (e->d_name[0] == '.')
            continue;
        char path[128], name[64] = "", line[1024];
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(name, sizeof name, f))
            name[strcspn(name, "\n")] = 0;
        fclose(f);
        uint64_t ns = 0;
        snprintf(path, sizeof path, "/proc/self/task/%s/schedstat", e->d_name);
        if ((f = fopen(path, "r")) != NULL) {
            unsigned long long on_cpu = 0;
            if (fscanf(f, "%llu", &on_cpu) == 1)
                ns = on_cpu;
            fclose(f);
        } else {
            snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
            if ((f = fopen(path, "r")) != NULL) {
                if (fgets(line, sizeof line, f)) {
                    /* After the name: state, fields 4 to 13, then utime and stime. */
                    char *p = strrchr(line, ')');
                    unsigned long utime = 0, stime = 0;
                    int field = 2;
                    for (char *token = p ? strtok(p + 1, " ") : NULL; token;
                         token = strtok(NULL, " ")) {
                        if (++field == 14)
                            utime = strtoul(token, NULL, 10);
                        else if (field == 15) {
                            stime = strtoul(token, NULL, 10);
                            break;
                        }
                    }
                    ns = (uint64_t)((double)(utime + stime) * 1e9 /
                                    (double)sysconf(_SC_CLK_TCK));
                }
                fclose(f);
            }
        }
        if (strcmp(name, "osoe-agent") == 0 || strncmp(name, "osoe-helper-", 12) == 0)
            *progress += ns;
        else
            *program += ns;
    }
    closedir(d);
}

/* PE 0's node's counters, between two marker lines on PE 0's output, when
 * MULTISTREAM_STAT names osoe-stat: `osoe-stat -d -j JOB` finds the job's segments
 * on this node through its live PEs, and the output goes back with the job's
 * own, so nothing is left on a node without a shared file system. */
static void dump(const char *which)
{
    const char *stat = getenv("MULTISTREAM_STAT");
    const char *job = getenv("SLURM_JOB_ID") ? getenv("SLURM_JOB_ID") : getenv("OSOE_JOB_ID");
    if (!stat || !job)
        return;
    printf("counters %s begin\n", which);
    fflush(stdout);
    char command[4096];
    snprintf(command, sizeof command, "%s -d -j %s 2>&1", stat, job);
    if (system(command) != 0)
        printf("counters %s: %s failed\n", which, command);
    fflush(stdout);
    printf("counters %s end\n", which);
    fflush(stdout);
}

/* Per node, at PE 0, indexed by the node's first PE: bytes moved, nanoseconds
 * of progress and of program CPU, and its PEs. */
static uint64_t node_bytes[4096], node_progress[4096], node_program[4096], node_pes[4096];

int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[1], "put") != 0 && strcmp(argv[1], "get") != 0)) {
        fprintf(stderr, "usage: multistream put|get BYTES SECONDS\n");
        return 2;
    }
    int get = strcmp(argv[1], "get") == 0;
    size_t bytes = (size_t)strtoull(argv[2], NULL, 0);
    double seconds = atof(argv[3]);
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    if (npes > 4096 || bytes == 0) {
        if (me == 0)
            fprintf(stderr, "multistream: at most 4096 PEs, and a block of a byte or more\n");
        shmem_global_exit(2);
    }
    unsigned char *window = shmem_malloc(bytes);
    unsigned char *local = malloc(bytes);
    int *targets = malloc(sizeof *targets * (size_t)npes);
    if (!window || !local || !targets) {
        fprintf(stderr, "multistream: PE %d: out of memory for %zu bytes\n", me, bytes);
        shmem_global_exit(2);
    }
    memset(local, me & 0xff, bytes);
    memset(window, 0, bytes);
    /* The PEs of other nodes, from this PE's place among them, so that the
     * node's PEs start on different peers. */
    int count = 0;
    for (int k = 1; k < npes; k++) {
        int p = (me + k) % npes;
        if (shmem_ptr(window, p) == NULL)
            targets[count++] = p;
    }
    int first = shmem_team_translate_pe(SHMEM_TEAM_SHARED, 0, SHMEM_TEAM_WORLD);
    int pes_per_node = shmem_team_n_pes(SHMEM_TEAM_SHARED);
    size_t window_bytes = (size_t)8 << 20;
    size_t per_quiet = bytes >= window_bytes ? 1 : window_bytes / bytes;
    shmem_barrier_all();

    if (me == 0)
        dump("start");
    uint64_t progress0, program0;
    thread_cpu(&progress0, &program0);
    double start = now(), stop = start + seconds;
    uint64_t moved = 0;
    if (count > 0) {
        size_t since_quiet = 0;
        for (int t = 0; now() < stop; t = (t + 1) % count) {
            if (get)
                shmem_getmem_nbi(local, window, bytes, targets[t]);
            else
                shmem_putmem(window, local, bytes, targets[t]);
            moved += bytes;
            if (++since_quiet == per_quiet) {
                shmem_quiet();
                since_quiet = 0;
            }
        }
        shmem_quiet();
    }
    double elapsed = now() - start;
    uint64_t progress1, program1;
    thread_cpu(&progress1, &program1);
    shmem_barrier_all();
    if (me == 0)
        dump("end");

    shmem_uint64_atomic_add(&node_bytes[first], moved, 0);
    shmem_uint64_atomic_add(&node_progress[first], progress1 - progress0, 0);
    shmem_uint64_atomic_add(&node_program[first], program1 - program0, 0);
    shmem_uint64_atomic_add(&node_pes[first], 1, 0);
    /* A reduction's source and destination are symmetric. */
    static double this_elapsed, slowest;
    this_elapsed = elapsed;
    shmem_double_max_reduce(SHMEM_TEAM_WORLD, &slowest, &this_elapsed, 1);
    shmem_barrier_all();

    if (me == 0) {
        const char *helpers = getenv("OSOE_PROGRESS_HELPERS");
        uint64_t total = 0, progress = 0;
        int nodes = 0;
        for (int n = 0; n < npes; n++) {
            uint64_t pes = shmem_uint64_atomic_fetch(&node_pes[n], 0);
            if (pes == 0)
                continue;
            nodes++;
            uint64_t b = shmem_uint64_atomic_fetch(&node_bytes[n], 0);
            uint64_t c = shmem_uint64_atomic_fetch(&node_progress[n], 0);
            uint64_t q = shmem_uint64_atomic_fetch(&node_program[n], 0);
            double gib = (double)b / (double)(1u << 30);
            printf("multistream node=%d pes=%llu mib_per_s=%.1f progress_cpu_s=%.3f "
                   "program_cpu_s=%.3f progress_cpu_s_per_gib=%.4f\n", nodes - 1,
                   (unsigned long long)pes, (double)b / (1 << 20) / slowest, (double)c * 1e-9,
                   (double)q * 1e-9, gib > 0 ? (double)c * 1e-9 / gib : 0.0);
            total += b;
            progress += c;
        }
        double gib = (double)total / (double)(1u << 30);
        printf("multistream job mode=%s bytes=%zu seconds=%.2f nodes=%d pes=%d "
               "pes_per_node=%d helpers=%s mib_per_s=%.1f progress_cpu_s_per_gib=%.4f\n",
               argv[1], bytes, slowest, nodes, npes, pes_per_node, helpers ? helpers : "unset",
               (double)total / (1 << 20) / slowest, gib > 0 ? (double)progress * 1e-9 / gib : 0.0);
        fflush(stdout);
    }
    shmem_barrier_all();
    shmem_free(window);
    free(local);
    free(targets);
    shmem_finalize();
    return 0;
}
