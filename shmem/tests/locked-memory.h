/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A process with no locked memory, for the tests of the zero-copy send's
 * refusal (ring.c; the review of v0.2 step 4's merge point, portability P1, and
 * the adversarial read of its fix).
 *
 * io_uring charges every send by reference to the locked-memory limit, and on
 * Linux 6.14 and later its rings too. A limit set before a job starts must so
 * leave room for the rings and none for the sends, which no one number does on
 * every kernel, page size and helper count - and the count is the user's whole,
 * so tests running beside each other spend it too. So a test lowers its own
 * process's limit to nothing once its rings are made: from then on every send
 * by reference is refused, on any kernel, whatever else the user holds.
 *
 * Except where the process may lock memory whatever its limit: one with
 * CAP_IPC_LOCK, root's, which io_uring does not charge at all. A page that locks
 * with the limit at nothing says so, and the test skips. The page is locked by
 * the system call itself: AddressSanitizer's and ThreadSanitizer's runtimes
 * take mlock() and return 0 without locking anything, which made every process
 * of theirs look like one with the capability, and both tests skip in both
 * presets (the second adversarial read of the change, finding 1). The kernel
 * asks the capability of the initial user namespace for both, as io_uring
 * does; a process's own CapEff, which a rootless container's root has, would
 * not say what io_uring will charge.
 *
 * A test at a limit the site has, not at nothing, leaves the limit as it was
 * set for the job and asks the same question of memory a page past it
 * (osoe_test_locked_memory_is_limited; T39, test-zero-copy-refusals.c).
 */
#ifndef OSOE_TEST_LOCKED_MEMORY_H
#define OSOE_TEST_LOCKED_MEMORY_H

#include <stdbool.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "check.h"

/* Whether `bytes` of fresh memory lock under this process's present limit,
 * by the system call itself (above); they are unlocked again. */
static inline bool osoe_test_memory_locks(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK_FATAL(p != MAP_FAILED);
    bool locked = syscall(SYS_mlock, p, bytes) == 0;
    if (locked)
        syscall(SYS_munlock, p, bytes);
    munmap(p, bytes);
    return locked;
}

/* Lower this process's locked-memory limit to nothing; true if its sends by
 * reference will now be refused, false if it may lock memory anyway. */
static inline bool osoe_test_no_locked_memory(void)
{
    struct rlimit limit;
    CHECK_FATAL(getrlimit(RLIMIT_MEMLOCK, &limit) == 0);
    limit.rlim_cur = 0;
    CHECK_FATAL(setrlimit(RLIMIT_MEMLOCK, &limit) == 0);
    return !osoe_test_memory_locks((size_t)sysconf(_SC_PAGESIZE));
}

/* True if this process's locked-memory limit, as it is, is held to: memory a
 * page past it does not lock. False for a process that may lock memory whatever
 * its limit, or whose limit is none, where no send by reference is refused. */
static inline bool osoe_test_locked_memory_is_limited(void)
{
    struct rlimit limit;
    CHECK_FATAL(getrlimit(RLIMIT_MEMLOCK, &limit) == 0);
    if (limit.rlim_cur == RLIM_INFINITY)
        return false;
    return !osoe_test_memory_locks((size_t)limit.rlim_cur + (size_t)sysconf(_SC_PAGESIZE));
}

#endif /* OSOE_TEST_LOCKED_MEMORY_H */
