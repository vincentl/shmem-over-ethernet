/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The zero-copy send at the locked-memory limit a site has, among many peers
 * (T39; the scale review of v0.2 before Part 4, S8).
 *
 * With OSOE_WIRE_ZERO_COPY=on io_uring charges each send by reference the
 * length it offers, and two pages, until its notification comes, to one count
 * for every process of the user on the host, and holds that count to the
 * charging process's own limit: 8 MiB, where a site has not raised it. A
 * connection lends a buffer of up to 256 KiB at a time, so about thirty
 * connections of the whole host use the count up, and a node with a connection
 * to each of many peers, all of them being put to, meets refusals on most of
 * its own. The wire copies from then on, on each connection that met one
 * (ring.c). The other tests of that fallback refuse every send, at a limit
 * lowered to nothing, among a handful of nodes (shmem.wire.zero-copy-refused,
 * shmem.internode.zero-copy-refused.0-1-2); this one leaves the limit as a site
 * has it, and has the connections of sixty-four nodes on one host: 4,032.
 *
 * The job is sixty-four logical nodes of one PE each. The script
 * (zero-copy-refusals.sh) gives every process a limit of 8 MiB, soft and hard,
 * so that shmem_init, which raises a soft limit to the hard one, cannot raise
 * it. Every PE puts a block of bytes to every other, in a number of rounds. A
 * block is more than a chunk (OSOE_AM_CHUNK), so that a put is several messages
 * and a connection's buffer goes out with several in it. The bytes of a round
 * differ at every position from every other round's, so that a put that did not
 * land, or that the fallback lost, leaves another round's bytes and fails like
 * a wrong one. In an even round the PEs put in rank order, so that all of them
 * put to PE 0 first and sixty-three connections lend buffers toward one
 * receiver; in an odd round each PE starts after itself, so that every receiver
 * has one sender at a time, over connections most of which copy by then.
 *
 * What this program checks is the job's: that it completes, that the limit it
 * was given is the one in force after shmem_init, that every byte is right,
 * that every node sent by reference (the count of its sends is not zero, a
 * refused one among them: a node that never tried tested nothing), and that
 * the invariant counters end at zero. That the refusal happened, and on how
 * many nodes, is the script's, from the notice each process says once.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"
#include "locked-memory.h"

/* What PE `from` writes at byte `i` of its put to PE `to` in round `round`: it
 * varies with the origin, the target and the position, and at every position it
 * differs from every other round's, since 53 is odd. The period is prime, so
 * that a block landing a whole number of chunks from its place does not repeat
 * the pattern there. */
static unsigned char pattern(int from, int to, unsigned round, size_t i)
{
    return (unsigned char)(from * 31 + to * 7 + (int)round * 53 + (int)(i % 251));
}

static void fill(unsigned char *bytes, size_t len, int from, int to, unsigned round)
{
    for (size_t i = 0; i < len; i++)
        bytes[i] = pattern(from, to, round, i);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s LIMIT_KIB PUT_KIB ROUNDS (through zero-copy-refusals.sh)\n",
                argv[0]);
        return 2;
    }
    const unsigned long limit_kib = strtoul(argv[1], NULL, 10);
    const size_t put_bytes = (size_t)strtoul(argv[2], NULL, 10) << 10;
    const unsigned rounds = (unsigned)strtoul(argv[3], NULL, 10);
    CHECK_FATAL(limit_kib > 0 && put_bytes > 0 && rounds > 0);

    shmem_init();
    const int me = shmem_my_pe(), npes = shmem_n_pes();

    /* The limit the script set is the one in force: it is the hard limit too,
     * so shmem_init could not raise it, and it is not unlimited. A program run
     * by hand, or a script whose limit did not take, would test nothing. */
    struct rlimit limit;
    CHECK_FATAL(getrlimit(RLIMIT_MEMLOCK, &limit) == 0);
    if (limit.rlim_cur != ((rlim_t)limit_kib << 10) || limit.rlim_max != limit.rlim_cur)
        fprintf(stderr, "PE %d: the locked-memory limit is %llu bytes soft and %llu hard, not "
                "%lu KiB: run this through zero-copy-refusals.sh\n", me,
                (unsigned long long)limit.rlim_cur, (unsigned long long)limit.rlim_max,
                limit_kib);
    CHECK_FATAL(limit.rlim_cur == ((rlim_t)limit_kib << 10) && limit.rlim_max == limit.rlim_cur);

    /* A PE that may lock memory whatever its limit (CAP_IPC_LOCK) is charged
     * nothing by io_uring, and none of its sends is refused: every PE skips
     * alike (locked-memory.h). On the heap: this program's statics are not
     * symmetric (not oshcc's). */
    int *flags = shmem_malloc(2 * sizeof *flags);
    CHECK_FATAL(flags != NULL);
    flags[0] = osoe_test_locked_memory_is_limited() ? 0 : 1;
    shmem_int_max_reduce(SHMEM_TEAM_WORLD, &flags[1], &flags[0], 1);
    const bool skip = flags[1] != 0;
    shmem_free(flags);
    if (skip) {
        if (me == 0)
            printf("SKIPPED: a PE may lock memory whatever its limit (CAP_IPC_LOCK), so no send "
                   "by reference is refused\n");
        shmem_finalize();
        return 0;
    }

    /* One inbox slot per origin, so that every PE may write at once without
     * two origins sharing bytes. */
    unsigned char *inbox = shmem_malloc(put_bytes * (size_t)npes);
    unsigned char *source = malloc(put_bytes);
    unsigned char *expected = malloc(put_bytes);
    CHECK_FATAL(inbox != NULL);
    CHECK_FATAL(source != NULL && expected != NULL);
    memset(inbox, 0, put_bytes * (size_t)npes);
    shmem_barrier_all();

    for (unsigned round = 0; round < rounds; round++) {
        for (int step = 0; step < npes; step++) {
            int to = round % 2 == 0 ? step : (me + 1 + step) % npes;
            if (to == me)
                continue;
            fill(source, put_bytes, me, to, round);
            shmem_putmem(inbox + put_bytes * (size_t)me, source, put_bytes, to);
        }
        /* Complete at every target, then every PE has put, before any reads. */
        shmem_quiet();
        shmem_barrier_all();

        for (int from = 0; from < npes; from++) {
            if (from == me)
                continue;
            const unsigned char *got = inbox + put_bytes * (size_t)from;
            fill(expected, put_bytes, from, me, round);
            long bad = -1;
            if (memcmp(got, expected, put_bytes) != 0) {
                bad = 0;
                while (got[bad] == expected[bad])
                    bad++;
                fprintf(stderr, "PE %d: round %u: byte %ld of %zu from PE %d is %u, expected %u\n",
                        me, round, bad, put_bytes, from, got[bad], expected[bad]);
            }
            CHECK(bad < 0);
        }
        /* Every PE has read, before any writes the next round over it. */
        shmem_barrier_all();
    }

    /* This node tried: a send by reference was made, a refused one counted.
     * One PE a node, so the node's count is this PE's. */
    uint64_t by_reference = 0;
    CHECK(osoe_test_node_metric("transport.am.zero_copy_sends", &by_reference));
    CHECK(by_reference > 0);

    CHECK_INVARIANTS();

    /* One line for the job, from PE 0, once every PE has passed: the script
     * looks for it. */
    int *tally = shmem_malloc(2 * sizeof *tally);
    CHECK_FATAL(tally != NULL);
    tally[0] = check_fails;
    shmem_int_sum_reduce(SHMEM_TEAM_WORLD, &tally[1], &tally[0], 1);
    if (me == 0 && tally[1] == 0)
        printf("ZERO COPY PUTS OK: %d PEs, %u rounds of %zu KiB to every other PE, under "
               "%lu KiB of locked memory\n", npes, rounds, put_bytes >> 10, limit_kib);
    shmem_free(tally);

    free(expected);
    free(source);
    shmem_free(inbox);
    shmem_finalize();
    return CHECK_DONE();
}
