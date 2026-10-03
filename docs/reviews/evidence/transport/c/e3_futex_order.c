/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* E3: (a) futex wake(1) order for SCHED_OTHER waiters queued in sequence, plus one
 * SCHED_FIFO waiter queued LAST (needs root). (b) one futex word shared by two resource
 * classes with wake-one: a descriptor release wakes a buffer-space waiter; the
 * descriptor waiter sleeps until its safety timeout. */
#define _GNU_SOURCE
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#define __NR_futex_wake 454
#define __NR_futex_wait 455
static _Atomic uint32_t word;
static _Atomic int order[16]; static _Atomic int norder;
static long fwait(uint32_t *a, uint32_t v, int ms) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); ts.tv_sec += ms / 1000; ts.tv_nsec += (ms % 1000) * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return syscall(__NR_futex_wait, a, v, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, &ts, CLOCK_MONOTONIC);
}
static long fwake(uint32_t *a, int n) { return syscall(__NR_futex_wake, a, FUTEX_BITSET_MATCH_ANY, n, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE); }
static void *waiter(void *p) {
    int id = (int)(long)p;
    if (id == 99) { struct sched_param sp = { .sched_priority = 10 }; if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp)) id = 98; }
    fwait((uint32_t *)&word, 0, 5000);
    order[atomic_fetch_add(&norder, 1)] = id; return NULL;
}
/* (b) */
static _Atomic uint32_t cword; static _Atomic int descr_free, buf_free;
static void *descr_waiter(void *p) {
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) { uint32_t v = atomic_load(&cword); int d = atomic_load(&descr_free);
        if (d > 0 && atomic_compare_exchange_strong(&descr_free, &d, d - 1)) break;
        long r = fwait((uint32_t *)&cword, v, 1000); if (r == -1 && errno == ETIMEDOUT) { clock_gettime(CLOCK_MONOTONIC, &t1); printf("(b) descriptor waiter: TIMED OUT after %.2fs although a descriptor was freed and a wake issued\n", (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9); } }
    printf("(b) descriptor waiter finally acquired (via safety timeout)\n"); return NULL;
}
static void *buf_waiter(void *p) {
    int wakes = 0;
    for (;;) { uint32_t v = atomic_load(&cword); if (atomic_load(&buf_free)) break;
        long r = fwait((uint32_t *)&cword, v, 3000); if (r == 0) { wakes++; printf("(b) buffer-space waiter woken (wake #%d), no buffer, re-sleeps\n", wakes); } else if (errno == ETIMEDOUT) break; }
    return NULL;
}
int main(void) {
    pthread_t t[8]; int n = 0;
    for (int i = 0; i < 5; i++) { pthread_create(&t[n++], NULL, waiter, (void *)(long)i); usleep(50000); }
    pthread_create(&t[n++], NULL, waiter, (void *)99L); usleep(50000);
    for (int i = 0; i < n; i++) { fwake((uint32_t *)&word, 1); usleep(30000); }
    for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
    printf("(a) queue order: 0 1 2 3 4 then 99(SCHED_FIFO; 98 = setschedparam failed). wake(1) order:");
    for (int i = 0; i < norder; i++) printf(" %d", order[i]); printf("\n");
    pthread_t bw, dw;
    pthread_create(&bw, NULL, buf_waiter, NULL); usleep(50000);   /* buffer waiter queued first */
    pthread_create(&dw, NULL, descr_waiter, NULL); usleep(50000);
    atomic_fetch_add(&descr_free, 1); atomic_fetch_add(&cword, 1);
    printf("(b) released one descriptor; futex_wake(1) woke %ld\n", fwake((uint32_t *)&cword, 1));
    pthread_join(dw, NULL); atomic_store(&buf_free, 1); atomic_fetch_add(&cword, 1); fwake((uint32_t *)&cword, 8); pthread_join(bw, NULL);
    return 0;
}
