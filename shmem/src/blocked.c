/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The "blocked in" record's protocol (§3.14): the PE publishing where it
 * waits, and a reader in another process taking one publication whole - the
 * deadlock detector's lead here, osoe-stat by its own route over the segment's
 * metric table (tools/stat/segment.c), the name's words shared through
 * base/atomic-name.h.
 *
 * The writer bumps the generation odd, stores the fields, and bumps it even;
 * the reader loads the generation, loads the fields, and loads it again,
 * accepting only an unchanged even value. Every field is an atomic word, stored
 * with release and loaded with acquire, the routine's name included as four of
 * them, and both bumps are seq_cst read-modify-writes. Why a copy accepted is
 * one publication's, in C11's terms rather than any machine's:
 *
 *  - A field load reading a *later* publication's store: that store is a
 *    release sequenced after the later publication's odd bump, and the load is
 *    an acquire, so the odd bump happens before the reader's second load of the
 *    generation, which then reads it or something after it - not the value
 *    the first load read. Refused.
 *  - A field load reading an *earlier* publication's store, when the first
 *    load of the generation read publication k's even bump: that bump is a
 *    release sequenced after k's field stores, and the load synchronizes with
 *    it, so k's stores happen before the field loads, which cannot then read
 *    anything older.
 *
 * The first argument is why the name is not a character array. Plain loads
 * carry no acquire, and on aarch64 they may be satisfied after the reader's
 * second load of the generation: the round-three review (K1) measured 28,000
 * to 218,000 torn copies in 60 to 90 million reads with the name as plain
 * characters, and none as words. The record's model passes either way, since
 * it runs under sequential consistency and x86-TSO, which keep loads in order
 * (blocked_record_model.py says so); shmem/tests/test-blocked-record.c is the
 * check on the target.
 *
 * §3.16 rules out the textbook seqlock's relaxed stores and release fence;
 * nothing here needs a rule the project does not already have.
 */
#include "blocked.h"

#include <stdatomic.h>

#include "base/atomic-name.h"

enum { ROUTINE_WORDS = OSOE_BLOCKED_ROUTINE_MAX / 8 };
_Static_assert(OSOE_BLOCKED_ROUTINE_MAX % 8 == 0, "the name is a whole number of words");

void osoe_blocked_publish(struct osoe_pe_slot *slot, const char *routine,
                          enum osoe_blocked_detail kind, uint64_t detail, uint64_t since_ns)
{
    atomic_fetch_add_explicit(&slot->blocked_gen, 1, memory_order_seq_cst);
    osoe_atomic_name_store(slot->blocked_routine, ROUTINE_WORDS, routine);
    atomic_store_explicit(&slot->blocked_detail, detail, memory_order_release);
    atomic_store_explicit(&slot->blocked_detail_kind, (uint32_t)kind, memory_order_release);
    atomic_store_explicit(&slot->blocked_since_ns, since_ns, memory_order_release);
    atomic_fetch_add_explicit(&slot->blocked_gen, 1, memory_order_seq_cst);
}

bool osoe_blocked_read(const struct osoe_pe_slot *slot, struct osoe_blocked_copy *copy)
{
    for (int attempt = 0; attempt < 100; attempt++) {
        uint64_t before = atomic_load_explicit(&slot->blocked_gen, memory_order_seq_cst);
        if (before & 1)
            continue;
        osoe_atomic_name_load(slot->blocked_routine, ROUTINE_WORDS, copy->routine,
                              sizeof copy->routine);
        copy->kind = atomic_load_explicit(&slot->blocked_detail_kind, memory_order_acquire);
        copy->detail = atomic_load_explicit(&slot->blocked_detail, memory_order_acquire);
        copy->since_ns = atomic_load_explicit(&slot->blocked_since_ns, memory_order_acquire);
        if (atomic_load_explicit(&slot->blocked_gen, memory_order_seq_cst) == before)
            return true;
    }
    return false;
}
