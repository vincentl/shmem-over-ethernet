/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A node that is merely stopped, with a transfer in flight toward it, is waited
 * for and not declared gone (§3.15, "As built: keepalive"; the phase 1 release
 * review, correctness K5).
 *
 * The lead finds a node that vanished with data in flight by asking the kernel
 * how long its peer has acknowledged nothing sent to it - TCP_INFO's
 * last_ack_recv - and asks only while something sent is still unacknowledged
 * (wire.c, osoe_wire_silent_ms). That condition is what protects a stopped
 * node. Its kernel goes on acknowledging until its receive buffer is full;
 * then the window shuts, the sender probes it at intervals that double, and a
 * probe leaves nothing outstanding, so the node reads as not silent. Ask
 * without the condition and the same node reads as silent for as long as the
 * gap between two probes; the gaps double up to two minutes, and a short budget
 * is passed in the first that is longer: the job ends, naming a node that was
 * never gone. A process under a debugger, or stopped with ^Z, is that node.
 *
 * shmem.stopped-node.keepalive cannot see the condition. It puts 8 bytes,
 * which a stopped node's kernel acknowledges; the connection then idles, and
 * keepalive's own probes keep last_ack_recv young. With the condition removed,
 * that test and every other related one (36 in all) still passed. This test
 * fills the buffers.
 *
 * Map 0,0,1: PE 2 is node 1's only PE. It stops itself (SIGSTOP). PE 0 puts
 * OSOE_TEST_BULK_BYTES to it - 64 MiB by default, more than six times the 10 MiB
 * the two kernels' buffers can hold between them at Linux's defaults (tcp_rmem
 * and tcp_wmem allow 6 MiB and 4 MiB) - and quiets. PE 1 continues PE 2 after
 * OSOE_TEST_STOP_MS. The job must complete with every byte in place, and PE 0's
 * put must have been held up until then: if it returned while node 1 was still
 * stopped, the buffers took the lot, the window never shut, and the test would
 * say nothing about the check.
 *
 * The stop must be long. The test is registered with keepalive at its tightest,
 * 1,1,3, a budget of 4 s (IDLE + INTERVAL x COUNT). The probes of a shut window
 * come at intervals that double from the retransmission timeout, and each is
 * answered, which restarts the silence. Measured on Linux 6.12, the answers came
 * at about 0.4, 0.9, 2.2, 3.9, 7.4 and 14.3 s after the barrier, so the silence
 * between the fifth and the sixth passes 4 s at 11.4 s, and the check, which
 * looks once a second, ends the job at about 12.2 s (28 runs, all four presets,
 * idle and under eight busy loops: 12.1 s to 12.4 s). A stop of 12 s let the
 * check's removal through, and 13 s did not. The 15 s given here is 2.8 s past
 * the removal's time, and is not to be trimmed.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* Word i of the transfer. Distinct for every i (an odd multiplier is a
 * bijection on 64 bits), so a piece delivered twice, in the wrong place or not
 * at all is a mismatch. */
static uint64_t word_at(size_t i)
{
    return ((uint64_t)i + 1) * 0x9E3779B97F4A7C15ull;
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe();
    const char *stop = getenv("OSOE_TEST_STOP_MS");
    const char *bulk = getenv("OSOE_TEST_BULK_BYTES");
    long stop_ms = stop ? atol(stop) : 15000;
    size_t bytes = bulk ? (size_t)strtoull(bulk, NULL, 0) : (size_t)64 << 20;
    size_t words = bytes / sizeof(uint64_t);
    CHECK_FATAL(words > 0 && bytes % sizeof(uint64_t) == 0);

    int *stopped_pid = shmem_calloc(1, sizeof *stopped_pid);
    long *wrong_words = shmem_calloc(1, sizeof *wrong_words);
    uint64_t *target = shmem_malloc(bytes);
    uint64_t *source = me == 0 ? malloc(bytes) : NULL;
    CHECK_FATAL(stopped_pid && wrong_words && target && (me != 0 || source));
    if (me == 0)
        for (size_t i = 0; i < words; i++)
            source[i] = word_at(i);
    shmem_barrier_all();
    double start = now();

    if (me == 2) {
        int pid = (int)getpid();
        shmem_putmem(stopped_pid, &pid, sizeof pid, 1);
        shmem_quiet();
        raise(SIGSTOP);                 /* continued by PE 1 */
    } else if (me == 0) {
        struct timespec settle = { 0, 300 * 1000000L };
        nanosleep(&settle, NULL);       /* PE 2 has stopped itself by now */
        fprintf(stderr, "PE 0: putting %zu bytes toward node 1, which is stopped, at %.1f s\n",
                bytes, now() - start);
        shmem_putmem(target, source, bytes, 2);
        double put_returned = now() - start;
        shmem_quiet();
        double quiet_completed = now() - start;
        fprintf(stderr, "PE 0: the put returned at %.1f s, the quiet completed at %.1f s\n",
                put_returned, quiet_completed);
        /* PE 1 continues node 1 once stop_ms is over, and the put cannot return
         * until the buffers between the nodes have room for the rest of it: an
         * earlier return means they took the lot. A second's slack covers the
         * barrier's skew. */
        double earliest_return = (double)stop_ms / 1000.0 - 1.0;
        if (put_returned < earliest_return)
            fprintf(stderr, "PE 0: the put returned before node 1 was continued: the buffers "
                            "took all %zu bytes, the window never shut, and this test says "
                            "nothing about the silent-peer check. Raise OSOE_TEST_BULK_BYTES\n",
                    bytes);
        CHECK(put_returned >= earliest_return);
    } else {
        shmem_int_wait_until(stopped_pid, SHMEM_CMP_NE, 0);
        struct timespec t = { stop_ms / 1000, (stop_ms % 1000) * 1000000L };
        nanosleep(&t, NULL);
        fprintf(stderr, "PE 1: continuing PE 2 (pid %d) at %.1f s\n", *stopped_pid,
                now() - start);
        CHECK_FATAL(kill((pid_t)*stopped_pid, SIGCONT) == 0);
    }

    shmem_barrier_all();
    if (me == 2) {
        long wrong = 0;
        size_t first = 0;
        for (size_t i = 0; i < words; i++) {
            if (target[i] != word_at(i)) {
                if (wrong == 0)
                    first = i;
                wrong++;
            }
        }
        if (wrong != 0)
            fprintf(stderr, "PE 2: %ld of %zu words differ, the first at byte %zu\n", wrong,
                    words, first * sizeof(uint64_t));
        shmem_long_p(wrong_words, wrong, 0);
    }
    shmem_barrier_all();
    if (me == 0) {
        CHECK(*wrong_words == 0);
        if (!CHECK_DONE()) {                    /* nothing has failed */
            printf("STOPPED BULK OK: %zu bytes arrived intact after node 1 was stopped for "
                   "%ld ms, at %.1f s\n", bytes, stop_ms, now() - start);
            fflush(stdout);
        }
    }

    CHECK_INVARIANTS();
    free(source);
    shmem_free(target);
    shmem_free(wrong_words);
    shmem_free(stopped_pid);
    shmem_finalize();
    return CHECK_DONE();
}
