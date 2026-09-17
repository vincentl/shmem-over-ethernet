/*
 * The serializing store (§3.16, §4.6 rule 2).
 *
 * One seq_cst store in the calling thread. On x86-64 it is an xchg, which
 * drains the store buffer, so every plain store this thread made earlier is
 * visible to other cores once it returns. fence, wait, and the barriers begin
 * with it.
 */
#ifndef OSOE_BASE_ORDER_H
#define OSOE_BASE_ORDER_H

void osoe_serializing_store(void);

#endif /* OSOE_BASE_ORDER_H */
