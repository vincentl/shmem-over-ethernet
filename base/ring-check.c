/* SPDX-License-Identifier: LGPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <liburing.h>
#include <stdlib.h>
#include <sys/resource.h>

#include "base/ring-check.h"

const char osoe_ring_check_creating[] = "an io_uring created SINGLE_ISSUER, R_DISABLED and CQSIZE";

int osoe_ring_check(const char **facility)
{
    struct io_uring ring;
    struct io_uring_params p = {
        .flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_R_DISABLED | IORING_SETUP_CQSIZE,
        .cq_entries = 8,
    };
    *facility = osoe_ring_check_creating;
    int e = io_uring_queue_init_params(4, &ring, &p);
    if (e < 0)
        return -e;
    int result = 0;
    /* What the agent does with its own ring on its first pass (ring.c): enable
     * it, then enter the kernel. The enter is a wait for completions with a
     * zero timeout and nothing submitted - the agent's own call - so this
     * thread takes on no io_uring task state. */
    *facility = "an io_uring enabled after it was created R_DISABLED";
    e = io_uring_enable_rings(&ring);
    if (e < 0)
        result = -e;
    if (result == 0) {
        *facility = "an io_uring that can be entered to wait for completions";
        struct io_uring_cqe *cqe;
        struct __kernel_timespec zero = { 0, 0 };
        e = io_uring_wait_cqe_timeout(&ring, &cqe, &zero);
        if (e < 0 && e != -ETIME && e != -EINTR && e != -EAGAIN)
            result = -e;
    }
    if (result == 0) {
        *facility = "the io_uring's list of its opcodes (IORING_REGISTER_PROBE)";
        size_t bytes = sizeof(struct io_uring_probe) + 256 * sizeof(struct io_uring_probe_op);
        struct io_uring_probe *probe = calloc(1, bytes);
        if (!probe) {
            result = ENOMEM;
        } else {
            e = io_uring_register_probe(&ring, probe, 256);
            if (e < 0) {
                result = -e;
            } else {
                *facility = "the RECV, SEND and FUTEX_WAIT io_uring opcodes";
                if (!io_uring_opcode_supported(probe, IORING_OP_RECV) ||
                    !io_uring_opcode_supported(probe, IORING_OP_SEND) ||
                    !io_uring_opcode_supported(probe, IORING_OP_FUTEX_WAIT))
                    result = ENOSYS;
            }
            free(probe);
        }
    }
    if (result == 0) {
        *facility = "a registered provided-buffer ring";
        struct io_uring_buf_ring *buffers = io_uring_setup_buf_ring(&ring, 2, 0, 0, &e);
        if (!buffers)
            result = -e;
        else
            io_uring_free_buf_ring(&ring, buffers, 2, 0);
    }
    io_uring_queue_exit(&ring);
    return result;
}

long long osoe_locked_memory_kib(void)
{
    struct rlimit limit;
    if (getrlimit(RLIMIT_MEMLOCK, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY)
        return -1;
    return (long long)(limit.rlim_cur / 1024);
}
