/* Symmetric addresses in the calling PE (§4.1, §4.2). */
#ifndef OSOE_SYM_H
#define OSOE_SYM_H

#include <stdbool.h>
#include <stdint.h>

#include "state.h"

/*
 * If [addr, addr + len) lies within this PE's symmetric heap, set *offset to
 * addr's offset from the heap's start and return true. The heap's extent is
 * the agreed SHMEM_SYMMETRIC_SIZE, not the page-rounded mapping.
 */
static inline bool osoe_sym_heap_offset(const void *addr, uint64_t len, uint64_t *offset)
{
    uintptr_t base = (uintptr_t)osoe.node.heap_of[osoe.boot.local_id];
    uintptr_t a = (uintptr_t)addr;
    if (a < base || a - base > osoe.node.heap_size || len > osoe.node.heap_size - (a - base))
        return false;
    *offset = a - base;
    return true;
}

#endif /* OSOE_SYM_H */
