/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* v0.2 step 4 part 0: what a fetch-add waits behind a put stream on the same
 * connection (head-of-line blocking, §3.2), against the connection buffer's
 * size. Four PEs, two a node, in blocks: PE 0 streams 1 MiB puts to PE 2 while
 * PE 1 times fetch-adds to PE 3, whose requests share node 0's connection to
 * node 1 with the stream. First with no stream, then under it.
 *
 * And the same of a barrier, a reduction or a broadcast in place of the
 * fetch-add, on any number of nodes, two PEs each (docs/fast-link.md §13, H15
 * to H17): each node's first PE streams to the next node's, all but the last
 * node's - on two nodes PE 0's stream alone, as above - and the nodes' second
 * PEs, PE 1 first, make the team the operation runs on: a barrier's messages on
 * the control wires, which the lead's agent keeps at every helper count, a
 * collective's payloads on the data wires, which the helpers hold. PE 1 times
 * each operation; the team runs the count PE 1 sets from the operation's time
 * alone, and an operation counts toward the stream's line only if PE 0's stream
 * was still running when it ended. The fetch-add stays PE 1's to PE 3, behind
 * PE 0's stream, on any number of nodes.
 *
 * usage: hol [SECONDS [OPERATION [BYTES]]]
 *   SECONDS    the stream's length (default 10)
 *   OPERATION  fetch-add (the default), barrier, reduce or broadcast
 *   BYTES      a reduction's or broadcast's size (default 8; a reduction's a
 *              whole number of longs)
 * PE 1 prints each phase as "hol phase=alone|stream op=OPERATION bytes=BYTES
 * operations=N median_us=... p90_us=... p99_us=... max_us=...
 * stream_mib_per_s=...", the stream's rate PE 0's. */
#define _GNU_SOURCE
#include <shmem.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void report(const char *phase, const char *operation, size_t bytes, double *microseconds,
                   size_t n, double extra)
{
    if (n == 0) {
        printf("hol phase=%s op=%s bytes=%zu operations=0\n", phase, operation, bytes);
        fflush(stdout);
        return;
    }
    qsort(microseconds, n, sizeof *microseconds, compare);
    printf("hol phase=%s op=%s bytes=%zu operations=%zu median_us=%.0f p90_us=%.0f p99_us=%.0f "
           "max_us=%.0f stream_mib_per_s=%.0f\n", phase, operation, bytes, n, microseconds[n / 2],
           microseconds[n * 9 / 10], microseconds[n * 99 / 100], microseconds[n - 1], extra);
    fflush(stdout);
}

enum { BYTES = 1 << 20, MOST = 200000 };
enum operation { FETCH_ADD, BARRIER, REDUCE, BROADCAST };

static long cell;
static long done;
static long count;                      /* the operations PE 1 sets for the team */
static double stream_rate;
static shmem_team_t timed = SHMEM_TEAM_INVALID;     /* each node's second PE */
static long *reduce_source, *reduce_dest;
static unsigned char *broadcast_source, *broadcast_dest;
static size_t elements;

/* One operation: PE 1's fetch-add to PE 3, or the timed team's. */
static void operate(enum operation op)
{
    switch (op) {
    case FETCH_ADD:
        (void)shmem_long_atomic_fetch_add(&cell, 1, 3);
        break;
    case BARRIER:
        shmem_team_sync(timed);
        break;
    case REDUCE:
        shmem_long_sum_reduce(timed, reduce_dest, reduce_source, elements);
        break;
    case BROADCAST:
        shmem_broadcastmem(timed, broadcast_dest, broadcast_source, elements, 0);
        break;
    }
}

/* PE 1 gives the team the count it set. */
static void share_count(int npes)
{
    for (int pe = 3; pe < npes; pe += 2)
        shmem_long_p(&count, count, pe);
    shmem_quiet();
}

int main(int argc, char **argv)
{
    double seconds = argc > 1 ? atof(argv[1]) : 10;
    const char *name = argc > 2 ? argv[2] : "fetch-add";
    size_t bytes = argc > 3 ? strtoull(argv[3], NULL, 10) : 8;
    enum operation op = strcmp(name, "barrier") == 0     ? BARRIER
                        : strcmp(name, "reduce") == 0    ? REDUCE
                        : strcmp(name, "broadcast") == 0 ? BROADCAST
                                                         : FETCH_ADD;
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    if (npes < 4 || npes % 2 != 0 || (op == FETCH_ADD && strcmp(name, "fetch-add") != 0) ||
        bytes == 0 || (op == REDUCE && bytes % sizeof(long) != 0)) {
        if (me == 0)
            fprintf(stderr, "hol: needs two PEs a node on two nodes or more, and an operation of "
                    "fetch-add, barrier, reduce or broadcast, a reduction's bytes whole longs\n");
        shmem_global_exit(2);
    }
    if (op == FETCH_ADD)
        bytes = sizeof(long);
    else if (op == BARRIER)
        bytes = 0;
    elements = op == REDUCE ? bytes / sizeof(long) : bytes;
    bool streams = me % 2 == 0 && me + 2 < npes;    /* each node's first PE but the last's */
    bool in_team = op != FETCH_ADD && me % 2 == 1;
    unsigned char *window = shmem_malloc(BYTES);
    reduce_source = shmem_calloc(elements ? elements : 1, sizeof(long));
    reduce_dest = shmem_calloc(elements ? elements : 1, sizeof(long));
    broadcast_source = shmem_calloc(bytes ? bytes : 1, 1);
    broadcast_dest = shmem_calloc(bytes ? bytes : 1, 1);
    unsigned char *source = malloc(BYTES);
    double *microseconds = malloc(MOST * sizeof *microseconds);
    memset(source, 7, BYTES);
    if (op != FETCH_ADD)
        shmem_team_split_strided(SHMEM_TEAM_WORLD, 1, 2, npes / 2, NULL, 0, &timed);
    shmem_barrier_all();

    /* Phase 1: the operation alone, for about two seconds: PE 1 times ten,
     * then sets the count the team makes from their median. */
    if (me == 1) {
        for (int i = 0; i < 10; i++) {
            double t = now();
            operate(op);
            microseconds[i] = (now() - t) * 1e6;
        }
        qsort(microseconds, 10, sizeof *microseconds, compare);
        double each = microseconds[5] > 1 ? microseconds[5] : 1;
        long n = (long)(2e6 / each);
        count = n < 10 ? 10 : n > MOST ? MOST : n;
        if (op != FETCH_ADD)
            share_count(npes);
    } else if (in_team) {
        for (int i = 0; i < 10; i++)
            operate(op);
    }
    shmem_barrier_all();
    if (me == 1) {
        for (long i = 0; i < count; i++) {
            double t = now();
            operate(op);
            microseconds[i] = (now() - t) * 1e6;
        }
        report("alone", name, bytes, microseconds, (size_t)count, 0);
    } else if (in_team) {
        for (long i = 0; i < count; i++)
            operate(op);
    }
    shmem_barrier_all();

    /* Phase 2: under the streams; PE 0 tells PE 1 when its own ends. The
     * team's count is as many as the stream's length holds at the operation's
     * time alone. */
    if (me == 1) {
        double each = microseconds[count / 2] > 1 ? microseconds[count / 2] : 1;
        long n = (long)(seconds * 1e6 / each);
        count = n < 10 ? 10 : n > MOST ? MOST : n;
        if (op != FETCH_ADD)
            share_count(npes);
    }
    shmem_barrier_all();
    if (streams) {
        double start = now();
        size_t total = 0;
        while (now() - start < seconds) {
            for (int i = 0; i < 64; i++)
                shmem_putmem(window, source, BYTES, me + 2);
            shmem_quiet();
            total += (size_t)64 * BYTES;
        }
        if (me == 0) {
            stream_rate = (double)total / (1 << 20) / (now() - start);
            shmem_double_p(&stream_rate, stream_rate, 1);
            shmem_quiet();
            shmem_long_atomic_set(&done, 1, 1);
        }
    } else if (me == 1 && op == FETCH_ADD) {
        size_t n = 0;
        double warm = now() + 0.5;      /* let the stream fill the pipe first */
        while (now() < warm)
            (void)shmem_long_atomic_fetch_add(&cell, 1, 3);
        while (shmem_long_atomic_fetch(&done, 1) == 0 && n < MOST) {
            double t = now();
            (void)shmem_long_atomic_fetch_add(&cell, 1, 3);
            microseconds[n++] = (now() - t) * 1e6;
        }
        shmem_long_wait_until(&done, SHMEM_CMP_EQ, 1);
        report("stream", name, bytes, microseconds, n, stream_rate);
    } else if (me == 1) {
        double warm = now() + 0.5;
        while (now() < warm)
            ;
        size_t n = 0;
        for (long i = 0; i < count; i++) {
            double t = now();
            operate(op);
            double us = (now() - t) * 1e6;
            if (shmem_long_atomic_fetch(&done, 1) == 0)
                microseconds[n++] = us;
        }
        shmem_long_wait_until(&done, SHMEM_CMP_EQ, 1);
        report("stream", name, bytes, microseconds, n, stream_rate);
    } else if (in_team) {
        for (long i = 0; i < count; i++)
            operate(op);
    }
    shmem_barrier_all();
    if (timed != SHMEM_TEAM_INVALID)
        shmem_team_destroy(timed);
    shmem_free(broadcast_dest);
    shmem_free(broadcast_source);
    shmem_free(reduce_dest);
    shmem_free(reduce_source);
    shmem_free(window);
    free(source);
    free(microseconds);
    shmem_finalize();
    return 0;
}
