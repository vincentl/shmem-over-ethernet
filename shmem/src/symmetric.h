/* Symmetric addresses in the calling PE (§4.1, §4.2, §4.3). */
#ifndef OSOE_SYMMETRIC_H
#define OSOE_SYMMETRIC_H

#include <stdbool.h>
#include <stdint.h>

#include "state.h"
#include "transport.h"

/*
 * Where a symmetric object is: its region, and its offset from the region's
 * start, which is the same on every PE (§4.2) - what the transport addresses
 * by (§4.10), never the address itself.
 */
struct osoe_symmetric_location {
    enum osoe_region region;
    uint64_t offset;
};

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

/* The same for the program's statics (§4.3), which a program not linked with
 * oshcc's script does not have. */
bool osoe_symmetric_statics_find(const void *addr, uint64_t len,
                                 struct osoe_symmetric_location *where);

/*
 * If [addr, addr + len) lies within one of this PE's symmetric regions - the
 * heap, or the program's global and static variables - say where, and return
 * true. The heap first: it is where nearly every symmetric object lives, and
 * its test is a subtraction and a compare.
 */
static inline bool osoe_symmetric_find(const void *addr, uint64_t len,
                                       struct osoe_symmetric_location *where)
{
    uint64_t offset;
    if (osoe_symmetric_heap_offset(addr, len, &offset)) {
        where->region = OSOE_REGION_HEAP;
        where->offset = offset;
        return true;
    }
    return osoe_symmetric_statics_find(addr, len, where);
}

/*
 * The same, fatal on failure, for the API entry points. `routine` names the
 * caller. An address in neither region is an error naming the routine, not a
 * silent miss, and says so differently for a program that has no symmetric
 * statics because it was not linked with oshcc.
 */
struct osoe_symmetric_location osoe_symmetric_locate(const char *routine, const void *addr,
                                                     uint64_t len);

/* As osoe_symmetric_locate, for a word of `width` bytes, which must also be
 * aligned for its width. The spec's AMO and wait types are aligned by C, so a
 * failure here means the address is not a symmetric object at all. */
struct osoe_symmetric_location osoe_symmetric_word_locate(const char *routine,
                                                          const void *addr, unsigned width);

/* Fatal unless pe is a world rank of this job. */
void osoe_check_pe(const char *routine, int pe);

#endif /* OSOE_SYMMETRIC_H */
