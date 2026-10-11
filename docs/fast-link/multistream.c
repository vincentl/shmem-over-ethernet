/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Part 4's stream (docs/fast-link.md, §5): every PE puts, or gets, BYTES-byte
 * blocks to the PEs of every other node in turn for SECONDS, a quiet each time
 * 8 MiB has gone, so that every peer of every node carries traffic at once - a
 * node's progress helpers divide its peers, not its streams (owner-map.h), and
 * one stream between two nodes keeps one helper busy whatever their count.
 *
 * **Consecutive blocks go to different nodes, and a node's PEs start on
 * different ones.** For the PE of local rank r on node n of N, block i goes to
 * node (n + 1 + (r + i) mod (N - 1)) mod N, to its PE of local rank
 * (r + i div (N - 1)) mod its PEs. In the order (me + k) mod npes every PE of a
 * node had the same list, starting on the same peer, and under the block
 * layout a node's PEs moved together as a convoy, one peer node at a time: in a
 * model, 2 to 2.8 peers of 7 busy at once at 8 PEs a node, and 1.5 at 32 (the
 * adversarial read of the kit, finding 1). So the spread is measured, not
 * assumed: each node's bytes to each peer node over the window, which come out
 * even whatever the order, and how many peer nodes its PEs start blocks toward
 * at once - each PE ORs a bit for the block's peer node into its node's word
 * for the millisecond the block starts in, a word a millisecond on the node's
 * first PE, and that PE averages the bits set over the milliseconds with any
 * (the second adversarial read of the kit, finding 7). For jobs of at most 64
 * nodes and windows of at most 16 s, longer windows taking longer slices.
 *
 * PE 0 prints a line for each node, one for each node's peers, and one for the
 * job:
 *
 *   multistream node=N pes=P mib_per_s=R progress_cpu_s=C program_cpu_s=Q
 *               progress_cpu_s_per_gib=G peers_at_once=A
 *   multistream node=N peer_mib=M1,M2,... (to nodes in order, itself skipped)
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
 * of progress and of program CPU, and its PEs; and, for jobs of at most
 * PEER_NODES nodes, each node's bytes to each peer node, by node index. */
enum { PEER_NODES = 64, SLICES = 16384 };
static uint64_t node_bytes[4096], node_progress[4096], node_program[4096], node_pes[4096];
static uint64_t peer_bytes[PEER_NODES][PEER_NODES];
/* On each node's first PE: a bit for each peer node its PEs started a block
 * toward in each slice of the window; and at PE 0, each node's mean of them, in
 * thousandths. */
static uint64_t slice_peers[SLICES], node_at_once[4096];
/* Every PE's node, as its first PE, and its local rank there (fcollect). */
static int my_first, my_rank, firsts[4096], ranks[4096];

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
    int first = shmem_team_translate_pe(SHMEM_TEAM_SHARED, 0, SHMEM_TEAM_WORLD);
    int pes_per_node = shmem_team_n_pes(SHMEM_TEAM_SHARED);
    /* Every PE's node and local rank; the nodes in order of their first PE. */
    my_first = first;
    my_rank = shmem_team_my_pe(SHMEM_TEAM_SHARED);
    shmem_int_fcollect(SHMEM_TEAM_WORLD, firsts, &my_first, 1);
    shmem_int_fcollect(SHMEM_TEAM_WORLD, ranks, &my_rank, 1);
    int *node_first = malloc(sizeof *node_first * (size_t)npes);
    int *node_size = calloc((size_t)npes, sizeof *node_size);
    int *node_of = malloc(sizeof *node_of * (size_t)npes);
    if (!node_first || !node_size || !node_of) {
        fprintf(stderr, "multistream: PE %d: out of memory for the node table\n", me);
        shmem_global_exit(2);
    }
    int nodes_count = 0;
    for (int p = 0; p < npes; p++)
        if (firsts[p] == p)
            node_first[nodes_count++] = p;
    for (int p = 0; p < npes; p++)
        for (int n = 0; n < nodes_count; n++)
            if (node_first[n] == firsts[p]) {
                node_of[p] = n;
                node_size[n]++;
            }
    /* Block i's target (the comment at the top): consecutive blocks to
     * different nodes, a node's PEs starting on different ones, and every PE
     * of every other node in (N - 1) times the largest node's PEs. */
    int count = 0, largest = 0, mine = node_of[me];
    for (int n = 0; n < nodes_count; n++)
        largest = node_size[n] > largest ? node_size[n] : largest;
    if (nodes_count > 1) {
        free(targets);
        targets = malloc(sizeof *targets * (size_t)(nodes_count - 1) * (size_t)largest);
        if (!targets) {
            fprintf(stderr, "multistream: PE %d: out of memory for its targets\n", me);
            shmem_global_exit(2);
        }
        for (int i = 0; i < (nodes_count - 1) * largest; i++) {
            int n = (mine + 1 + (my_rank + i) % (nodes_count - 1)) % nodes_count;
            int rank = (my_rank + i / (nodes_count - 1)) % node_size[n];
            for (int p = 0; p < npes; p++)
                if (node_of[p] == n && ranks[p] == rank) {
                    targets[count++] = p;
                    break;
                }
        }
    }
    uint64_t *to_node = calloc((size_t)nodes_count, sizeof *to_node);
    if (!to_node) {
        fprintf(stderr, "multistream: PE %d: out of memory\n", me);
        shmem_global_exit(2);
    }
    size_t window_bytes = (size_t)8 << 20;
    size_t per_quiet = bytes >= window_bytes ? 1 : window_bytes / bytes;
    /* PE 0's counters before any PE starts, so that its window and the
     * others' begin together (the read, note 14). */
    if (me == 0)
        dump("start");
    shmem_barrier_all();

    uint64_t progress0, program0;
    thread_cpu(&progress0, &program0);
    double slice = seconds / SLICES > 1e-3 ? seconds / SLICES : 1e-3;
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
            to_node[node_of[targets[t]]] += bytes;
            if (nodes_count <= PEER_NODES) {
                size_t s = (size_t)((now() - start) / slice);
                if (s < SLICES)
                    shmem_uint64_atomic_or(&slice_peers[s], UINT64_C(1) << node_of[targets[t]],
                                           first);
            }
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
    if (nodes_count <= PEER_NODES)
        for (int n = 0; n < nodes_count; n++)
            if (to_node[n])
                shmem_uint64_atomic_add(&peer_bytes[mine][n], to_node[n], 0);
    /* Every OR is done, at every node's first PE, before it counts them. */
    shmem_barrier_all();
    if (me == first && nodes_count <= PEER_NODES) {
        uint64_t bits = 0, busy = 0;
        for (size_t s = 0; s < SLICES; s++)
            if (slice_peers[s]) {
                bits += (uint64_t)__builtin_popcountll(slice_peers[s]);
                busy++;
            }
        if (busy)
            shmem_uint64_atomic_add(&node_at_once[first], bits * 1000 / busy, 0);
    }
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
            uint64_t a = shmem_uint64_atomic_fetch(&node_at_once[n], 0);
            double gib = (double)b / (double)(1u << 30);
            printf("multistream node=%d pes=%llu mib_per_s=%.1f progress_cpu_s=%.3f "
                   "program_cpu_s=%.3f progress_cpu_s_per_gib=%.4f peers_at_once=%.2f\n",
                   nodes - 1, (unsigned long long)pes, (double)b / (1 << 20) / slowest,
                   (double)c * 1e-9, (double)q * 1e-9, gib > 0 ? (double)c * 1e-9 / gib : 0.0,
                   (double)a / 1000);
            total += b;
            progress += c;
            if (nodes_count <= PEER_NODES && nodes_count > 1) {
                printf("multistream node=%d peer_mib=", nodes - 1);
                for (int m = 0, comma = 0; m < nodes_count; m++)
                    if (m != nodes - 1) {
                        uint64_t to = shmem_uint64_atomic_fetch(&peer_bytes[nodes - 1][m], 0);
                        printf("%s%.1f", comma++ ? "," : "", (double)to / (1 << 20));
                    }
                printf("\n");
            }
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
    free(to_node);
    free(node_of);
    free(node_size);
    free(node_first);
    shmem_finalize();
    return 0;
}
