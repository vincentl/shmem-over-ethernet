#include <limits.h>

#include "base/epoch.h"
#include "base/futex.h"

void osoe_epoch_init(struct osoe_epoch *e)
{
    atomic_store_explicit(&e->count_compare_only_by_equality, 0, memory_order_release);
}

bool osoe_epoch_bump_eq(struct osoe_epoch *e, uint32_t target)
{
    uint32_t prev = atomic_fetch_add_explicit(&e->count_compare_only_by_equality, 1,
                                              memory_order_seq_cst);
    return (uint32_t)(prev + 1) == target;
}

void osoe_epoch_wake_all(struct osoe_epoch *e)
{
    osoe_futex_wake(&e->count_compare_only_by_equality, INT_MAX, OSOE_FUTEX_SHARED);
}

void osoe_epoch_wait_eq(struct osoe_epoch *e, uint32_t target)
{
    for (;;) {
        uint32_t v = atomic_load_explicit(&e->count_compare_only_by_equality,
                                          memory_order_acquire);
        if (v == target)
            return;
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        osoe_futex_wait(&e->count_compare_only_by_equality, v, &sweep, OSOE_FUTEX_SHARED);
    }
}
