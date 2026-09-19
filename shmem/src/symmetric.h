/* Symmetric addresses in the calling PE (§4.1, §4.2). */
#ifndef OSOE_SYMMETRIC_H
#define OSOE_SYMMETRIC_H

#include <stdbool.h>
#include <stdint.h>

#include "state.h"

/*
 * If [addr, addr + len) lies within this PE's symmetric heap, set *offset to
 * addr's offset from the heap's start and return true. The heap's extent is
 * the agreed SHMEM_SYMMETRIC_SIZE, not the page-rounded mapping.
 */
static inline bool osoe_symmetric_heap_offset(const void *addr, uint64_t len, uint64_t *offset)
{
    uintptr_t base = (uintptr_t)osoe.node.heap_of[osoe.bootstrap.local_id];
    uintptr_t a = (uintptr_t)addr;
    if (a < base || a - base > osoe.node.heap_size || len > osoe.node.heap_size - (a - base))
        return false;
    *offset = a - base;
    return true;
}

/*
 * The same check, fatal on failure, for the API entry points. `routine` names
 * the caller. Symmetric statics are not supported yet (§4.3, §7.4), so an
 * address outside the heap is an error naming the routine, not a silent miss.
 */
uint64_t osoe_symmetric_offset(const char *routine, const void *addr, uint64_t len);

/* As osoe_symmetric_offset, for a word of `width` bytes, which must also be aligned
 * for its width. The spec's AMO and wait types are aligned by C, so a failure
 * here means the address is not a symmetric object at all. */
uint64_t osoe_symmetric_word_offset(const char *routine, const void *addr, unsigned width);

/* Fatal unless pe is a world rank of this job. */
void osoe_check_pe(const char *routine, int pe);

#endif /* OSOE_SYMMETRIC_H */
