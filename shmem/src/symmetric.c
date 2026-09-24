/* The symmetric-address checks every API entry point makes (§4.1, §4.2, §4.3). */
#include <stdint.h>

#include "fatal.h"
#include "state.h"
#include "symmetric.h"

/*
 * The statics object holds the data range at offset 0 and the bss range after
 * it (node.h), so an address in the bss range is data_bytes past its offset in
 * that range. An object lies in one range - no C object spans two sections -
 * but where the linker placed the two ranges end to end, as it does unless
 * another output section falls between them, a transfer may run from one into
 * the other as it would in the object.
 */
bool osoe_symmetric_statics_find(const void *addr, uint64_t len,
                                 struct osoe_symmetric_location *where)
{
    const struct osoe_node *n = &osoe.node;
    uintptr_t a = (uintptr_t)addr;
    uintptr_t data = (uintptr_t)n->statics_data, bss = (uintptr_t)n->statics_bss;
    bool adjacent = data + n->statics_data_bytes == bss;
    uint64_t offset, limit;
    if (n->statics_data_bytes && a >= data && a - data < n->statics_data_bytes) {
        offset = a - data;
        limit = adjacent ? n->statics_bytes : n->statics_data_bytes;
    } else if (n->statics_bss_bytes && a >= bss && a - bss < n->statics_bss_bytes) {
        offset = n->statics_data_bytes + (a - bss);
        limit = n->statics_bytes;
    } else {
        return false;
    }
    if (len > limit - offset)
        return false;
    where->region = OSOE_REGION_STATICS;
    where->offset = offset;
    return true;
}

struct osoe_symmetric_location osoe_symmetric_locate(const char *routine, const void *addr,
                                                     uint64_t len)
{
    osoe_require_initialized(routine);
    struct osoe_symmetric_location where;
    if (osoe_symmetric_find(addr, len, &where))
        return where;
    if (!osoe.node.statics_bytes)
        osoe_fatal("%s: [%p, +%llu) is not within the symmetric heap, and this program "
                   "was not linked with oshcc, so its global and static variables are not "
                   "symmetric (design §4.3)", routine, addr, (unsigned long long)len);
    osoe_fatal("%s: [%p, +%llu) is neither within the symmetric heap nor among the "
               "program's global and static variables", routine, addr,
               (unsigned long long)len);
}

struct osoe_symmetric_location osoe_symmetric_word_locate(const char *routine,
                                                          const void *addr, unsigned width)
{
    struct osoe_symmetric_location where = osoe_symmetric_locate(routine, addr, width);
    if (where.offset % width)
        osoe_fatal("%s: %p is not %u-byte aligned", routine, addr, width);
    return where;
}

void osoe_check_pe(const char *routine, int pe)
{
    osoe_require_initialized(routine);
    if (pe < 0 || (uint32_t)pe >= osoe.bootstrap.world_size)
        osoe_fatal("%s: PE %d is not in 0..%u", routine, pe, osoe.bootstrap.world_size - 1);
}
