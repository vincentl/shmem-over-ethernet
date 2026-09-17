#include <stdatomic.h>
#include <stdint.h>
struct buf { _Atomic uint32_t head, filled; };
uint32_t load_acq(struct buf *b) { return atomic_load_explicit(&b->head, memory_order_acquire); }
uint32_t load_seq(struct buf *b) { return atomic_load_explicit(&b->head, memory_order_seq_cst); }
uint32_t load_rel(struct buf *b) { return atomic_load_explicit(&b->head, memory_order_relaxed); }
void add_rel(struct buf *b, uint32_t n) { atomic_fetch_add_explicit(&b->filled, n, memory_order_release); }
void add_seq(struct buf *b, uint32_t n) { atomic_fetch_add_explicit(&b->filled, n, memory_order_seq_cst); }
int cas_acq(struct buf *b, uint32_t h, uint32_t n) { return atomic_compare_exchange_weak_explicit(&b->head, &h, h + n, memory_order_acquire, memory_order_acquire); }
int cas_seq(struct buf *b, uint32_t h, uint32_t n) { return atomic_compare_exchange_weak_explicit(&b->head, &h, h + n, memory_order_seq_cst, memory_order_seq_cst); }
