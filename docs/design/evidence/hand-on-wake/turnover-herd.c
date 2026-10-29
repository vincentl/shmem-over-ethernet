/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * P2's timing (README.md beside this): every PE of node 0 puts BYTES-byte
 * blocks to the one PE of node 1 for SECONDS, a quiet each 8 MiB, so that the
 * connection's buffer is full and every turnover has writers asleep for its
 * room - the release fixes' review's herd (S1, its q3-herd.c), whose figures
 * this prints again for OSOE_TURNOVER_WAKE=all against hand-on:
 *
 *   herd wake=W writers=N bytes=B seconds=S mib_per_s=R agent_cpu_s_per_gib=G
 *        sleeps_per_64kib=K waits_per_64kib=A hand_ons_per_64kib=H
 *        passes_per_64kib=P slowest_share=L fastest_share=F longest_put_ms=M
 *        median_longest_put_ms=D segment_kib=E
 *
 * mib_per_s is node 0's bytes over the window; agent_cpu_s_per_gib the CPU
 * node 0's lead spent in its threads named osoe-agent or osoe-helper-N over
 * the window, a GiB; sleeps_per_64kib the writers' main threads' voluntary
 * context switches over the window, a 64 KiB moved - a writer that finds the
 * buffer full sleeps, and under all sleeps again if the room was taken first;
 * waits, hand-ons and passes are node 0's am.reserve_stalls,
 * am.turnover_hand_ons and am.turnover_passes over the window, a 64 KiB. And
 * what the model cannot see, starvation: slowest_share and fastest_share are
 * the least and most bytes a writer moved over the writers' mean, and
 * longest_put_ms the longest any one put call of any writer took, its median
 * over the writers beside it. segment_kib is the bytes a segment node 0 sent
 * carried, on average over the window: how full the agent's batches were.
 *
 * usage: turnover-herd BYTES SECONDS, under osoe-launch -m 0,...,0,1
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <shmem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "invariants.h"

enum { QUIET_EVERY = 8 << 20, MOST_PES = 1024 };

/* Each writer's figures, put to PE 0's copy of this after the window. */
struct writer_figures {
    uint64_t bytes;
    uint64_t sleeps;
    uint64_t longest_put_ns;
};
static struct writer_figures figures[MOST_PES];

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* This thread's voluntary context switches: its sleeps. */
static uint64_t my_sleeps(void)
{
    FILE *f = fopen("/proc/thread-self/status", "r");
    char line[256];
    uint64_t n = 0;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "voluntary_ctxt_switches: %llu", (unsigned long long *)&n) == 1)
            break;
    if (f)
        fclose(f);
    return n;
}

/* Nanoseconds on a CPU of this process's osoe-agent and osoe-helper-N threads. */
static uint64_t progress_cpu_ns(void)
{
    uint64_t total = 0;
    DIR *d = opendir("/proc/self/task");
    for (struct dirent *e; d && (e = readdir(d)) != NULL;) {
        if (e->d_name[0] == '.')
            continue;
        char path[128], name[64] = "";
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(name, sizeof name, f))
            name[strcspn(name, "\n")] = 0;
        fclose(f);
        if (strcmp(name, "osoe-agent") != 0 && strncmp(name, "osoe-helper", 11) != 0)
            continue;
        unsigned long long ns = 0;
        snprintf(path, sizeof path, "/proc/self/task/%s/schedstat", e->d_name);
        if ((f = fopen(path, "r")) != NULL) {
            if (fscanf(f, "%llu", &ns) == 1)
                total += ns;
            fclose(f);
        }
    }
    if (d)
        closedir(d);
    return total;
}

/* A thread-scope counter summed over this node's counter blocks. */
static uint64_t thread_total(const char *metric)
{
    char name[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(name, sizeof name, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, name, err, sizeof err) != 0) {
        fprintf(stderr, "turnover-herd: %s: %s\n", metric, err);
        return 0;
    }
    const struct osoe_metric_descriptor *m = s.table_usable ? osoe_segment_metric(&s, metric) : NULL;
    uint64_t total = 0;
    for (uint32_t b = 0; m && m->scope == OSOE_MSCOPE_THREAD && b < s.dir->block_count; b++) {
        uint64_t v;
        if (osoe_segment_read(&s, m, b, &v))
            total += v;
    }
    if (!m)
        fprintf(stderr, "turnover-herd: no metric %s in %s\n", metric, s.name);
    osoe_segment_close(&s);
    return total;
}

static int ascending(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: turnover-herd BYTES SECONDS\n");
        return 2;
    }
    size_t bytes = (size_t)strtoull(argv[1], NULL, 0);
    double seconds = atof(argv[2]);
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes(), target = n - 1, writers = n - 1;
    if (n < 2 || n > MOST_PES || bytes == 0) {
        if (me == 0)
            fprintf(stderr, "turnover-herd: 2 to %d PEs, and a block of a byte or more\n", MOST_PES);
        shmem_global_exit(2);
    }
    unsigned char *window = shmem_malloc(bytes);
    unsigned char *source = malloc(bytes);
    if (!window || !source) {
        fprintf(stderr, "turnover-herd: PE %d: out of memory for %zu bytes\n", me, bytes);
        shmem_global_exit(2);
    }
    memset(source, me & 0xff, bytes);
    const char *setting = getenv("OSOE_TURNOVER_WAKE");

    uint64_t waits0 = 0, hand_ons0 = 0, passes0 = 0, cpu0 = 0, segments0 = 0;
    shmem_barrier_all();
    if (me == 0) {
        osoe_test_node_metric("transport.am.segments_sent", &segments0);
        waits0 = thread_total("am.reserve_stalls");
        hand_ons0 = thread_total("am.turnover_hand_ons");
        passes0 = thread_total("am.turnover_passes");
        cpu0 = progress_cpu_ns();
    }
    shmem_barrier_all();
    struct writer_figures mine = { 0, 0, 0 };
    if (me != target) {
        uint64_t sleeps0 = my_sleeps(), since_quiet = 0;
        uint64_t end = now_ns() + (uint64_t)(seconds * 1e9);
        for (uint64_t t = now_ns(); t < end;) {
            shmem_putmem(window, source, bytes, target);
            uint64_t after = now_ns();
            if (after - t > mine.longest_put_ns)
                mine.longest_put_ns = after - t;
            mine.bytes += bytes;
            if ((since_quiet += bytes) >= QUIET_EVERY) {
                shmem_quiet();
                since_quiet = 0;
                after = now_ns();
            }
            t = after;
        }
        shmem_quiet();
        mine.sleeps = my_sleeps() - sleeps0;
    }
    shmem_barrier_all();
    uint64_t cpu = me == 0 ? progress_cpu_ns() - cpu0 : 0;
    if (me != target)
        shmem_putmem(&figures[me], &mine, sizeof mine, 0);
    shmem_barrier_all();

    if (me == 0) {
        uint64_t waits = thread_total("am.reserve_stalls") - waits0;
        uint64_t hand_ons = thread_total("am.turnover_hand_ons") - hand_ons0;
        uint64_t passes = thread_total("am.turnover_passes") - passes0;
        uint64_t segments = 0;
        osoe_test_node_metric("transport.am.segments_sent", &segments);
        segments -= segments0;
        uint64_t total = 0, sleeps = 0, least = UINT64_MAX, most = 0, longest = 0;
        uint64_t *longests = malloc(sizeof *longests * (size_t)writers);
        for (int w = 0; w < writers; w++) {
            total += figures[w].bytes;
            sleeps += figures[w].sleeps;
            least = figures[w].bytes < least ? figures[w].bytes : least;
            most = figures[w].bytes > most ? figures[w].bytes : most;
            longest = figures[w].longest_put_ns > longest ? figures[w].longest_put_ns : longest;
            if (longests)
                longests[w] = figures[w].longest_put_ns;
        }
        if (longests)
            qsort(longests, (size_t)writers, sizeof *longests, ascending);
        double mean = (double)total / writers, pieces = (double)total / 65536.0;
        printf("herd wake=%s writers=%d bytes=%zu seconds=%.1f mib_per_s=%.1f "
               "agent_cpu_s_per_gib=%.4f sleeps_per_64kib=%.3f waits_per_64kib=%.3f "
               "hand_ons_per_64kib=%.3f passes_per_64kib=%.3f slowest_share=%.3f "
               "fastest_share=%.3f longest_put_ms=%.2f median_longest_put_ms=%.2f "
               "segment_kib=%.1f\n",
               setting ? setting : "unset", writers, bytes, seconds,
               (double)total / seconds / 1048576.0,
               (double)cpu * 1e-9 / ((double)total / 1073741824.0), sleeps / pieces,
               waits / pieces, hand_ons / pieces, passes / pieces, (double)least / mean,
               (double)most / mean, (double)longest * 1e-6,
               longests ? (double)longests[writers / 2] * 1e-6 : 0.0,
               segments ? (double)total / (double)segments / 1024.0 : 0.0);
        free(longests);
    }
    shmem_barrier_all();
    free(source);
    shmem_free(window);
    shmem_finalize();
    return 0;
}
