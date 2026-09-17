#include <stdatomic.h>
#include <stdint.h>
_Atomic uint32_t w;
uint32_t load_acq(void){ return atomic_load_explicit(&w, memory_order_acquire); }
uint32_t load_seq(void){ return atomic_load_explicit(&w, memory_order_seq_cst); }
void store_rel(uint32_t v){ atomic_store_explicit(&w, v, memory_order_release); }
void store_seq(uint32_t v){ atomic_store_explicit(&w, v, memory_order_seq_cst); }
uint32_t fadd_rel(void){ return atomic_fetch_add_explicit(&w, 1, memory_order_release); }
uint32_t fadd_seq(void){ return atomic_fetch_add_explicit(&w, 1, memory_order_seq_cst); }
