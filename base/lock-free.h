/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The atomic words processes share are lock-free (§3.16; the step D review,
 * portability Q4).
 *
 * Every atomic word osoe's processes share - the futex words, the protocol
 * words of a node's segments, of the connection area and of the job's tables,
 * and an AMO's target in a node-mate's heap - is an _Atomic uint32_t or an
 * _Atomic uint64_t, in memory each process maps at an address of its own. C11
 * expects two mappings of one atomic object to communicate atomically only
 * where it is lock-free (C11 §7.17.5, whose note asks that lock-free operations
 * be address-free): one that is not is guarded by a lock in one process's
 * memory, which another cannot see. <stdatomic.h> says of each integer type
 * whether its atomics are always lock-free (2), never (0) or sometimes (1),
 * which only the running machine answers; so never is refused here, when
 * compiling, and sometimes is asked of the objects at start-up
 * (osoe_lock_free, which shmem_init calls before anything is shared). On LP64
 * Linux, the only targets the library builds for, uint32_t is unsigned int and
 * uint64_t unsigned long; long long is asked too, which a C library could make
 * uint64_t. GCC and Clang say always of all three on x86-64 and aarch64, so
 * nothing is asked there.
 */
#ifndef OSOE_BASE_LOCK_FREE_H
#define OSOE_BASE_LOCK_FREE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

_Static_assert(ATOMIC_INT_LOCK_FREE != 0 && ATOMIC_LONG_LOCK_FREE != 0 &&
               ATOMIC_LLONG_LOCK_FREE != 0,
               "the atomics osoe shares between processes must be lock-free, and "
               "this compiler says its 32- or 64-bit atomics never are");

/* Whether the shared atomics are lock-free here: true wherever compiling has
 * said always, and otherwise what the objects say. */
static inline bool osoe_lock_free(void)
{
#if ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2
    return true;
#else
    static _Atomic uint32_t word32;
    static _Atomic uint64_t word64;
    static _Atomic unsigned long long wide;
    return atomic_is_lock_free(&word32) && atomic_is_lock_free(&word64) &&
           atomic_is_lock_free(&wide);
#endif
}

#endif /* OSOE_BASE_LOCK_FREE_H */
