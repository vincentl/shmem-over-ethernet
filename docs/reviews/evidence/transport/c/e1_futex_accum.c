/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* E1: the §3.10 idle loop, literally: each idle iteration submits a NEW
 * IORING_OP_FUTEX_WAIT. If the loop is woken by some other CQE (recv/send),
 * the previous futex wait stays armed in the kernel. Count them, then show
 * a PE-side wake from ANOTHER PROCESS (shared memfd futex) completing them. */
#define _GNU_SOURCE
#include <liburing.h>
#include <linux/futex.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#define __NR_futex_wake 454
#define ITERS 1000
int main(void) {
    int fd = memfd_create("wake", 0); ftruncate(fd, 4096);
    _Atomic uint32_t *wake = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    int p[2]; pipe(p);
    struct io_uring ring; struct io_uring_params prm = { .flags = IORING_SETUP_SINGLE_ISSUER|IORING_SETUP_CQSIZE, .cq_entries = 64 };
    if (io_uring_queue_init_params(16, &ring, &prm)) { perror("init"); return 1; }
    char buf[1];
    int armed_futex = 0, futex_cqes = 0, other_cqes = 0, overflow_seen = 0;
    for (int it = 0; it < ITERS; it++) {
        uint32_t v = atomic_load(wake);
        /* "work arrives": a recv-like event on a pipe completes this iteration */
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        io_uring_prep_read(sqe, p[0], buf, 1, 0); sqe->user_data = 2;
        sqe = io_uring_get_sqe(&ring);
        io_uring_prep_futex_wait(sqe, (uint32_t *)wake, v, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32, 0);
        sqe->user_data = 1; armed_futex++;
        write(p[1], "x", 1);
        io_uring_submit_and_wait(&ring, 1);
        struct io_uring_cqe *cqe; unsigned head, n = 0;
        io_uring_for_each_cqe(&ring, head, cqe) { n++; if (cqe->user_data == 1) futex_cqes++; else other_cqes++; }
        io_uring_cq_advance(&ring, n);
    }
    printf("after %d idle-loop iterations: futex SQEs submitted=%d, futex CQEs=%d, other CQEs=%d -> %d futex waits still armed in kernel\n",
           ITERS, armed_futex, futex_cqes, other_cqes, armed_futex - futex_cqes);
    pid_t pid = fork();
    if (pid == 0) { /* PE process */
        atomic_fetch_add(wake, 1);
        long r = syscall(__NR_futex_wake, wake, FUTEX_BITSET_MATCH_ANY, INT_MAX, FUTEX2_SIZE_U32);
        printf("PE process futex_wake(INT_MAX) woke %ld io_uring waiters\n", r);
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    usleep(100000);
    int burst = 0; struct io_uring_cqe *cqe;
    while (io_uring_peek_cqe(&ring, &cqe) == 0) { if (cqe->user_data == 1) burst++; io_uring_cqe_seen(&ring, cqe); }
    printf("CQEs from that single wake: %d (CQ size 64; kernel overflow flag=%s)\n", burst,
           (*ring.sq.kflags & IORING_SQ_CQ_OVERFLOW) ? "SET" : "clear");
    /* drain overflow */
    int more = 0; io_uring_get_events(&ring);
    while (io_uring_peek_cqe(&ring, &cqe) == 0) { more++; io_uring_cqe_seen(&ring, cqe); if (!io_uring_cq_ready(&ring)) io_uring_get_events(&ring);} 
    printf("further CQEs flushed from overflow list: %d\n", more);
    return 0;
}
