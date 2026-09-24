/*
 * The broadcasts (spec §9.10.7): shmem_TYPENAME_broadcast for every standard
 * RMA type of Table 5, and shmem_broadcastmem, over the accumulator
 * (collectives.c) with the root's source in place of a combine. "The dest
 * object is updated on all PEs" - the root's included, where the deprecated
 * active-set forms, shmem_broadcast32 and shmem_broadcast64, leave the root's
 * alone: "the dest object is updated on all PEs other than the root PE". Those
 * run over the whole job only (collectives.h), and count their elements in four
 * and eight bytes; pSync is neither read nor written, which leaves it as the
 * spec requires it to be found afterwards.
 *
 * Complexity: O(n) copies per member; the internode phase as collectives.c
 * says, down a tree rooted at the root's node.
 */
#include <stdbool.h>
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
                              sizeof(T), PE_root, false);                             \
    }

OSOE_RMA_STANDARD_TYPES(OSOE_BROADCAST_DEFINE)

OSOE_API int pshmem_broadcastmem(shmem_team_t team, void *dest, const void *source,
                                 size_t nelems, int PE_root)
{
    return osoe_broadcast("shmem_broadcastmem", team, dest, source, nelems, 1, PE_root, false);
}

/* The deprecated active-set forms (Annex F): PE_root counts within the active
 * set, which is the whole job, so it is a world rank. */
#define OSOE_BROADCAST_ACTIVE_SET_DEFINE(BITS)                                        \
    OSOE_API void pshmem_broadcast##BITS(void *dest, const void *source, size_t nelems,  \
                                         int PE_root, int PE_start, int logPE_stride,  \
                                         int PE_size, long *pSync)                     \
    {                                                                                 \
        (void)pSync;                                                                  \
        osoe_active_set_whole_job("shmem_broadcast" #BITS, PE_start, logPE_stride,    \
                                  PE_size);                                           \
        osoe_broadcast("shmem_broadcast" #BITS, SHMEM_TEAM_WORLD, dest, source, nelems, \
                       BITS / 8, PE_root, true);                                      \
    }
OSOE_BROADCAST_ACTIVE_SET_DEFINE(32)
OSOE_BROADCAST_ACTIVE_SET_DEFINE(64)

/* The weak `shmem_` aliases (export.h). */
#define OSOE_BROADCAST_ALIAS(T, NAME) OSOE_ALIAS(NAME##_broadcast)
OSOE_RMA_STANDARD_TYPES(OSOE_BROADCAST_ALIAS)
OSOE_ALIAS(broadcastmem)
OSOE_ALIAS(broadcast32)
OSOE_ALIAS(broadcast64)
