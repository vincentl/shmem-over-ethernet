/* The symmetric-address checks every API entry point makes (§4.1, §4.2). */
#include <stdint.h>

#include "fatal.h"
#include "state.h"
#include "symmetric.h"

uint64_t osoe_symmetric_offset(const char *routine, const void *addr, uint64_t len)
{
    uint64_t offset;
    if (!osoe_symmetric_heap_offset(addr, len, &offset))
        osoe_fatal("%s: [%p, +%llu) is not within the symmetric heap (symmetric statics "
                   "are not yet supported)", routine, addr, (unsigned long long)len);
    return offset;
}

uint64_t osoe_symmetric_word_offset(const char *routine, const void *addr, unsigned width)
{
    uint64_t offset = osoe_symmetric_offset(routine, addr, width);
    if (offset % width)
        osoe_fatal("%s: %p is not %u-byte aligned", routine, addr, width);
    return offset;
}

void osoe_check_pe(const char *routine, int pe)
{
    if (pe < 0 || (uint32_t)pe >= osoe.bootstrap.world_size)
        osoe_fatal("%s: PE %d is not in 0..%u", routine, pe, osoe.bootstrap.world_size - 1);
}
