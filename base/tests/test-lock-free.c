/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The shared atomics' lock-free check (base/lock-free.h; §3.16).
 *
 * Every machine the library builds for says always, so a build meets neither
 * of the header's other two answers; this test makes <stdatomic.h> give each,
 * for long's atomics. Built with OSOE_TEST_LOCK_FREE_NEVER they are never
 * lock-free, and the compile must fail with the header's assertion
 * (base.lock-free.never, by its message). Built without it they are
 * sometimes, and the header asks the objects, as shmem_init does at start-up:
 * this machine's answer must be yes, as its compiler's is (base.lock-free).
 */
#include <stdatomic.h>

#undef ATOMIC_LONG_LOCK_FREE
#ifdef OSOE_TEST_LOCK_FREE_NEVER
#define ATOMIC_LONG_LOCK_FREE 0
#else
#define ATOMIC_LONG_LOCK_FREE 1
#endif

#include "base/lock-free.h"
#include "check.h"

int main(void)
{
    CHECK(osoe_lock_free());
    if (!CHECK_DONE())
        printf("the shared atomics are lock-free, asked of the objects\n");
    return CHECK_DONE();
}
