#include <stdatomic.h>
#include <stdint.h>
_Atomic uint32_t a;
void store_rel(uint32_t v){ atomic_store_explicit(&a, v, memory_order_release); }
void store_seq(uint32_t v){ atomic_store_explicit(&a, v, memory_order_seq_cst); }
uint32_t load_acq(void){ return atomic_load_explicit(&a, memory_order_acquire); }
uint32_t fa_rel(void){ return atomic_fetch_add_explicit(&a, 1, memory_order_release); }
