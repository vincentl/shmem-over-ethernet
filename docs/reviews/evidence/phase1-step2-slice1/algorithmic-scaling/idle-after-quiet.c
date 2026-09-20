/*
 * Does a connection go idle again after one put + quiet?
 *
 * Review evidence (phase1-step2-slice1, algorithmic-scaling). Two logical
 * nodes, one PE each, so each PE is its node's lead and hosts the progress
 * engine. Phase A: init, barrier, sleep 2 s, report the CPU time this process
 * burned while asleep (the agent's idle polling: a 250 us sleep per pass).
 * Phase B: PE 0 puts 8 bytes to PE 1 and quiets, then both sleep 2 s again.
 * If the acknowledgement machinery settles, phase B's idle CPU equals phase
 * A's. If segments carrying ACK_REQUESTED answer each other, it does not.
 *
 * CPU time turned out to be the wrong detector: in step 2 the exchange is paced
 * by the agents' 250 us poll sleep, so it costs little CPU. The instrumented
 * run (a fprintf of osoe_progress's private counters at stop, in the private
 * copy only) is the evidence: see idle-after-quiet-instrumented.log. Run with
 * any argument for the control, which issues no put.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>
#include <shmem.h>

static double cpu_ms(void)
{
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1e3 +
           (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e3;
}

static void nap(void)
{
    struct timespec t = { 2, 0 };
    nanosleep(&t, NULL);
}

int main(int argc, char **argv)
{
    int do_put = argc < 2;      /* any argument: the control run, no put at all */
    (void)argv;
    shmem_init();
    int me = shmem_my_pe();
    long *x = shmem_malloc(sizeof *x);
    shmem_barrier_all();

    double a0 = cpu_ms();
    nap();
    double a1 = cpu_ms();

    if (me == 0 && do_put) {
        long v = 42;
        shmem_putmem(x, &v, sizeof v, 1);
        shmem_quiet();
    }
    double b0 = cpu_ms();
    nap();
    double b1 = cpu_ms();

    printf("pe %d: idle cpu before any put %.1f ms / 2 s; after put+quiet %.1f ms / 2 s\n",
           me, a1 - a0, b1 - b0);
    shmem_barrier_all();
    shmem_free(x);
    shmem_finalize();
    return 0;
}
