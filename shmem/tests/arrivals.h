/*
 * Barrier timing checks that do not depend on scheduling.
 *
 * A barrier guarantees that no PE leaves before every PE has arrived. Each PE
 * records its arrival time (CLOCK_MONOTONIC, which all processes on a machine
 * share) in a small shared-memory table that is independent of the library,
 * then enters the collective; on return it checks that it left no earlier than
 * the latest recorded arrival. PE r delays r * STAGGER before arriving, so
 * arrivals are spread out and a collective that did not wait would fail.
 *
 * The table is /dev/shm/osoetest.<job>. Every PE opens it before the first
 * check and removes the name at the end; osoe-launch also removes it, quietly,
 * in case a PE failed first. It is not named osoe.<job>.*, so it never counts
 * as a library leak.
 */
#ifndef OSOE_TEST_ARRIVALS_H
#define OSOE_TEST_ARRIVALS_H

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum { ARRIVALS_MAX = 256, ARRIVALS_STAGGER_MS = 20 };

static double *arrivals;
static char arrivals_name[64];

static inline double arrivals_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static inline void arrivals_open(void)
{
    snprintf(arrivals_name, sizeof arrivals_name, "/osoetest.%s", getenv("OSOE_JOB_ID"));
    int fd = shm_open(arrivals_name, O_CREAT | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, sizeof(double) * ARRIVALS_MAX) != 0) {
        perror("arrivals_open");
        exit(2);
    }
    arrivals = mmap(NULL, sizeof(double) * ARRIVALS_MAX, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, 0);
    close(fd);
    if (arrivals == MAP_FAILED) {
        perror("arrivals_open: mmap");
        exit(2);
    }
}

static inline void arrivals_close(void)
{
    munmap(arrivals, sizeof(double) * ARRIVALS_MAX);
    shm_unlink(arrivals_name);          /* every PE tries; ENOENT is fine */
}

/* Delay by rank, then record this PE's arrival. Call immediately before the
 * collective. */
static inline void arrivals_arrive(int me)
{
    struct timespec ts = { 0, (long)me * ARRIVALS_STAGGER_MS * 1000 * 1000 };
    nanosleep(&ts, NULL);
    arrivals[me] = arrivals_now();
}

/* Call immediately after the collective: true if this PE left no earlier than
 * any PE arrived. */
static inline int arrivals_left_after_all(int n)
{
    double left = arrivals_now();
    for (int i = 0; i < n; i++)
        if (left < arrivals[i])
            return 0;
    return 1;
}

#endif /* OSOE_TEST_ARRIVALS_H */
