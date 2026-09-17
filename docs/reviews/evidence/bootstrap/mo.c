#include <stdatomic.h>
#include <stdint.h>
_Atomic uint32_t head, filled;
uint32_t load_relaxed(void){ return atomic_load_explicit(&head, memory_order_relaxed); }
uint32_t load_acquire(void){ return atomic_load_explicit(&head, memory_order_acquire); }
void add_relaxed(uint32_t n){ atomic_fetch_add_explicit(&filled, n, memory_order_relaxed); }
void add_release(uint32_t n){ atomic_fetch_add_explicit(&filled, n, memory_order_release); }
int cas_acqrel(uint32_t *h, uint32_t n){ return atomic_compare_exchange_weak_explicit(&head, h, *h + n, memory_order_acq_rel, memory_order_acquire); }
