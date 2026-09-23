/*
 * A value one thread moves and others wait on, with no syscall per move when
 * nobody waits (design §3.11, §4.6; tools/modelcheck/wait_word_model.py).
 *
 * §4.6's shape: the value has a companion 32-bit futex word; the mover moves
 * the value, then bumps the word, then wakes; a waiter loads the word, then
 * checks the value, then sleeps on the word at what it loaded, so a move that
 * lands between its check and its sleep changes the word and the sleep returns
 * at once. What the shape leaves open is the cost the second review named
 * (atomics F3): an acknowledgement lands on every quiet's path, and a
 * futex_wake for each, waiter or none, is a syscall per event on the agent's
 * thread. So the word carries a count of waiters, and the mover wakes only
 * when the count is not zero.
 *
 * **That makes it a two-sided protocol (§3.16)**: a waiter stores (registers)
 * and then loads the value; the mover stores the value and then loads the
 * count. C11 forbids both missing each other only among seq_cst operations, so
 * the registration is a seq_cst RMW, the value's store and the waiter's check
 * of it are seq_cst, and so is the mover's load of the count. The model's
 * breakages - the count read before the value's store, the registration after
 * the check, the value published by a release store - each lose a wake.
 *
 * **Every waiter is woken** (§3.11: the wake count tracks divisibility). The
 * values this carries are divisible: each quiet waits for its own sequence, and
 * each writer has its own reservation to retry. A credit, which only one
 * waiter can take, is not one of them.
 *
 * The value is the caller's, and so are its stores and loads: this holds only
 * the word and the count, and says nothing about what the value is.
 */
#ifndef OSOE_BASE_WAIT_WORD_H
#define OSOE_BASE_WAIT_WORD_H

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>

#include "base/futex.h"

struct osoe_wait_word {
    _Atomic uint32_t word;          /* the futex word: bumped after the value moves */
    _Atomic uint32_t waiters;       /* registered waiters: the mover wakes only if any */
};

/* The waiter's first step, before it looks at the value at all. */
static inline void osoe_wait_word_enter(struct osoe_wait_word *w)
{
    atomic_fetch_add_explicit(&w->waiters, 1, memory_order_seq_cst);
}

/* The word, loaded before each check of the value: sleep on this. An acquire
 * load keeps the check that follows it after it. */
static inline uint32_t osoe_wait_word_load(const struct osoe_wait_word *w)
{
    return atomic_load_explicit(&w->word, memory_order_acquire);
}

static inline void osoe_wait_word_leave(struct osoe_wait_word *w)
{
    atomic_fetch_sub_explicit(&w->waiters, 1, memory_order_seq_cst);
}

/*
 * The mover's step, **after** its seq_cst store of the value: if anyone is
 * registered, bump the word and wake them all. A waiter that registered after
 * this load of the count checks the value after the store, and so does not
 * sleep on the old one.
 */
static inline void osoe_wait_word_wake_all(struct osoe_wait_word *w)
{
    if (atomic_load_explicit(&w->waiters, memory_order_seq_cst) == 0)
        return;
    atomic_fetch_add_explicit(&w->word, 1, memory_order_seq_cst);
    osoe_futex_wake(&w->word, INT_MAX, OSOE_FUTEX_SHARED);
}

#endif /* OSOE_BASE_WAIT_WORD_H */
