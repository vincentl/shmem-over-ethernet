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
 * (§3.3); enabled, and entered to wait for completions, as the agent's first
 * pass does with its own (ring.c) - a kernel or a filter that refused only
 * those ended a job after its PEs had left shmem_init (Part 6's adversarial
 * reads); its list of opcodes, with RECV, SEND and FUTEX_WAIT in it; and a
 * registered provided-buffer ring. FUTEX_WAIT is Linux 6.7, which also brings
 * everything older in the list (multishot receive is 6.0).
 */
int osoe_ring_check(const char **facility);

/* The facility named when creating the ring failed: the one step the
 * kernel.io_uring_disabled sysctl refuses, with EPERM. */
extern const char osoe_ring_check_creating[];

/*
 * The locked-memory limit this process has (RLIMIT_MEMLOCK, `ulimit -l`), in
 * KiB, or -1 where it has none, which the messages call "unlimited", as
 * `ulimit -l` does. io_uring charges against it each send by reference - the
 * length it offers, and two pages - until its notification, and on Linux 6.14
 * and later its rings too, counting every process of the user on the host
 * together against each process's own limit, and where one meets it says only
 * -ENOMEM; so the messages that report one say this number (ring.c,
 * progress.c, the probe).
 */
long long osoe_locked_memory_kib(void);

#endif /* OSOE_BASE_RING_CHECK_H */
