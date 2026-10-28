/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * T38's calibration, without the library: the CPU a fixed burst of work costs
 * when it runs a gap apart, as an owner's passes do under a trickle, and when
 * it runs back to back. A machine whose cores are one speed gives one figure
 * for both; a VM whose host gives a lightly loaded vCPU a slower core gives a
 * larger one for the bursts.
 *
 * usage: burst ITERATIONS BURSTS GAP_US
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t thread_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static volatile uint64_t sink;

static void work(long iterations)
{
    uint64_t x = sink;
    for (long i = 0; i < iterations; i++)
        x = x * 6364136223846793005u + 1442695040888963407u;
    sink = x;
}

int main(int argc, char **argv)
{
    long iterations = argc > 1 ? atol(argv[1]) : 20000;
    long bursts = argc > 2 ? atol(argv[2]) : 300;
    long gap_us = argc > 3 ? atol(argv[3]) : 2000;
    struct timespec gap = { gap_us / 1000000, (gap_us % 1000000) * 1000 };
    uint64_t spaced = 0, packed;
    for (long b = 0; b < bursts; b++) {
        nanosleep(&gap, NULL);
        uint64_t t = thread_ns();
        work(iterations);
        spaced += thread_ns() - t;
    }
    uint64_t t = thread_ns();
    for (long b = 0; b < bursts; b++)
        work(iterations);
    packed = thread_ns() - t;
    printf("burst of %ld iterations: %.2f us of CPU a gap apart, %.2f us back to back; "
           "%.2f times\n", iterations, (double)spaced / 1e3 / (double)bursts,
           (double)packed / 1e3 / (double)bursts, (double)spaced / (double)packed);
    return 0;
}
