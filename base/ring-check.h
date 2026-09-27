/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Does the running kernel have the io_uring the AM backend's ring needs (§1,
 * §3.3)? Checked at start-up, beside the futex2 check, before anything is
 * created in /dev/shm: an old kernel under new headers is refused by name
 * rather than failing mid-rendezvous (§1's second check).
 */
#ifndef OSOE_BASE_RING_CHECK_H
#define OSOE_BASE_RING_CHECK_H

/*
 * Returns 0, or the errno of what failed, with `*facility` naming it: a ring
 * created SINGLE_ISSUER, R_DISABLED and with an explicit completion queue size
 * (§3.3), a registered provided-buffer ring, and the RECV, SEND and FUTEX_WAIT
 * opcodes. FUTEX_WAIT is Linux 6.7, which also brings everything older in the
 * list (multishot receive is 6.0). EPERM usually means io_uring is disabled by
 * the kernel.io_uring_disabled sysctl.
 */
int osoe_ring_check(const char **facility);

#endif /* OSOE_BASE_RING_CHECK_H */
