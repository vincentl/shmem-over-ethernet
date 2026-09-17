#include <stdatomic.h>
#include <stdint.h>

#include "base/order.h"

/* Thread-local, so concurrent callers never share a cache line. */
static _Thread_local _Atomic uint32_t serial;

void osoe_serializing_store(void)
{
    atomic_store_explicit(&serial, 1, memory_order_seq_cst);
}
