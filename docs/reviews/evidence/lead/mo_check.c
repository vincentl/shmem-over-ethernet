/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include <stdatomic.h>
#include <stdint.h>
_Atomic uint32_t a;
uint32_t load_acq(void){ return atomic_load_explicit(&a, memory_order_acquire); }
uint32_t load_rel(void){ return atomic_load_explicit(&a, memory_order_relaxed); }
void store_rel(uint32_t v){ atomic_store_explicit(&a, v, memory_order_release); }
void store_relaxed(uint32_t v){ atomic_store_explicit(&a, v, memory_order_relaxed); }
uint32_t fa_rel(void){ return atomic_fetch_add_explicit(&a, 1, memory_order_release); }
_Bool cas(uint32_t h){ return atomic_compare_exchange_weak_explicit(&a,&h,h+1,memory_order_acquire,memory_order_acquire); }
