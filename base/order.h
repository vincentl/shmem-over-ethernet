/*
 * The two fences behind §4.6 rule 2 (§3.16): what orders the calling thread's
 * plain stores - a put's memcpy, a store through shmem_ptr - under Table 14.
 *
 * **osoe_release_fence, for fence.** Fence orders delivery: a store before it
 * must not be seen after a store after it (W->W). For a program C11 calls
 * race-free this needs nothing from the library - the flag a target waits on is
 * written by an AMO (spec 1.5 change #17), a seq_cst store and so a release, and
 * that already orders every earlier store. The fence is for Table 14's plain
 * stores, which C11 calls racy and says nothing about; the weakest C11 construct
 * that orders earlier stores before later ones is a release fence. It costs no
 * instruction on x86-64, whose stores are already in order, and a dmb ish on
 * aarch64.
 *
 * **osoe_serializing_fence, for quiet, pe_quiet and the barriers.** Quiet
 * promises that everything is "complete and visible to all PEs" when it returns
 * (spec §9.12.2), so a load after it must not be satisfied before a store
 * before it is visible (W->R, the store-buffering shape). C11 forbids that
 * outcome only with a seq_cst fence between each side's store and load; a
 * release fence does not. An mfence on x86-64, a dmb ish on aarch64.
 *
 * Both were once a seq_cst store to a thread-local word. C11 relates that to no
 * other location, so it guaranteed neither; on x86-64 it is an xchg and
 * happened to give both, on aarch64 an stlr, which gave quiet nothing
 * (docs/design/evidence/fence-litmus).
 */
#ifndef OSOE_BASE_ORDER_H
#define OSOE_BASE_ORDER_H

void osoe_release_fence(void);
void osoe_serializing_fence(void);

#endif /* OSOE_BASE_ORDER_H */
