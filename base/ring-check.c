/* SPDX-License-Identifier: LGPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <liburing.h>

#include "base/ring-check.h"

int osoe_ring_check(const char **facility)
{
    struct io_uring ring;
    struct io_uring_params p = {
        .flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_R_DISABLED | IORING_SETUP_CQSIZE,
        .cq_entries = 8,
    };
    *facility = "an io_uring created SINGLE_ISSUER, R_DISABLED and CQSIZE";
    int e = io_uring_queue_init_params(4, &ring, &p);
    if (e < 0)
        return -e;
    int result = 0;
    *facility = "the RECV, SEND and FUTEX_WAIT io_uring opcodes";
    struct io_uring_probe *probe = io_uring_get_probe_ring(&ring);
    if (!probe || !io_uring_opcode_supported(probe, IORING_OP_RECV) ||
        !io_uring_opcode_supported(probe, IORING_OP_SEND) ||
        !io_uring_opcode_supported(probe, IORING_OP_FUTEX_WAIT))
        result = ENOSYS;
    if (probe)
        io_uring_free_probe(probe);
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
