#include <stdatomic.h>

#include "base/order.h"

void osoe_release_fence(void)
{
    atomic_thread_fence(memory_order_release);
}

void osoe_serializing_fence(void)
{
    atomic_thread_fence(memory_order_seq_cst);
}
