/*
 * The team-based broadcast (spec §9.10.7): shmem_TYPENAME_broadcast for every
 * standard RMA type of Table 5, and shmem_broadcastmem, over the accumulator
 * (collectives.c) with the root's source in place of a combine. "The dest
 * object is updated on all PEs" - the root's included, where the deprecated
 * active-set form leaves the root's alone.
 *
 * Complexity: O(n) copies per member; the internode phase as collectives.c
 * says, down a tree rooted at the root's node.
 */
#include <stddef.h>
#include <stdint.h>

#include <pshmem.h>
#include <shmem.h>

#include "collectives.h"
#include "export.h"

#define OSOE_BROADCAST_DEFINE(T, NAME)                                                \
    OSOE_API int OSOE_FN(pshmem_, NAME, _broadcast)(shmem_team_t team, T *dest,       \
                                   const T *source, size_t nelems, int PE_root)        \
    {                                                                                 \
        return osoe_broadcast("shmem_" #NAME "_broadcast", team, dest, source, nelems, \
                              sizeof(T), PE_root);                                    \
    }

OSOE_RMA_STANDARD_TYPES(OSOE_BROADCAST_DEFINE)

OSOE_API int pshmem_broadcastmem(shmem_team_t team, void *dest, const void *source,
                                 size_t nelems, int PE_root)
{
    return osoe_broadcast("shmem_broadcastmem", team, dest, source, nelems, 1, PE_root);
}

/* The weak `shmem_` aliases (export.h). */
#define OSOE_BROADCAST_ALIAS(T, NAME) OSOE_ALIAS(NAME##_broadcast)
OSOE_RMA_STANDARD_TYPES(OSOE_BROADCAST_ALIAS)
OSOE_ALIAS(broadcastmem)
