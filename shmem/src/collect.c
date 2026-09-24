/*
 * collect and fcollect (spec §9.10.8): shmem_TYPENAME_collect and
 * shmem_TYPENAME_fcollect for every standard RMA type of Table 5, and
 * shmem_collectmem and shmem_fcollectmem, over the gather in collectives.c -
 * every member's block, in team rank order, into every member's dest. fcollect's
 * blocks are all one length; collect's may each differ, and are gathered after
 * an fcollect of their lengths. The deprecated active-set forms, collect32,
 * collect64, fcollect32 and fcollect64, run over the whole job only
 * (collectives.h), their elements four and eight bytes; pSync is neither read
 * nor written.
 *
 * Complexity: O(P n) copied by each member and O(L n) by the lead; the
 * internode phase as internode.h says.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pshmem.h>
#include <shmem.h>

#include "collectives.h"
#include "export.h"

#define OSOE_COLLECT_DEFINE(T, NAME)                                                  \
    OSOE_API int OSOE_FN(pshmem_, NAME, _collect)(shmem_team_t team, T *dest,         \
                                                  const T *source, size_t nelems)    \
    {                                                                                 \
        return osoe_collect("shmem_" #NAME "_collect", team, dest, source, nelems,    \
                            sizeof(T), false);                                        \
    }                                                                                 \
    OSOE_API int OSOE_FN(pshmem_, NAME, _fcollect)(shmem_team_t team, T *dest,        \
                                                   const T *source, size_t nelems)   \
    {                                                                                 \
        return osoe_collect("shmem_" #NAME "_fcollect", team, dest, source, nelems,   \
                            sizeof(T), true);                                         \
    }

OSOE_RMA_STANDARD_TYPES(OSOE_COLLECT_DEFINE)

OSOE_API int pshmem_collectmem(shmem_team_t team, void *dest, const void *source, size_t nelems)
{
    return osoe_collect("shmem_collectmem", team, dest, source, nelems, 1, false);
}

OSOE_API int pshmem_fcollectmem(shmem_team_t team, void *dest, const void *source, size_t nelems)
{
    return osoe_collect("shmem_fcollectmem", team, dest, source, nelems, 1, true);
}

/* The deprecated active-set forms (Annex F), over the whole job only. */
#define OSOE_COLLECT_ACTIVE_SET_DEFINE(KIND, BITS, FIXED)                             \
    OSOE_API void pshmem_##KIND##BITS(void *dest, const void *source, size_t nelems,   \
                                      int PE_start, int logPE_stride, int PE_size,     \
                                      long *pSync)                                     \
    {                                                                                 \
        (void)pSync;                                                                  \
        osoe_active_set_whole_job("shmem_" #KIND #BITS, PE_start, logPE_stride,       \
                                  PE_size);                                           \
        osoe_collect("shmem_" #KIND #BITS, SHMEM_TEAM_WORLD, dest, source, nelems,    \
                     BITS / 8, FIXED);                                                \
    }
OSOE_COLLECT_ACTIVE_SET_DEFINE(collect, 32, false)
OSOE_COLLECT_ACTIVE_SET_DEFINE(collect, 64, false)
OSOE_COLLECT_ACTIVE_SET_DEFINE(fcollect, 32, true)
OSOE_COLLECT_ACTIVE_SET_DEFINE(fcollect, 64, true)

/* The weak `shmem_` aliases (export.h). */
#define OSOE_COLLECT_ALIAS(T, NAME) OSOE_ALIAS(NAME##_collect) OSOE_ALIAS(NAME##_fcollect)
OSOE_RMA_STANDARD_TYPES(OSOE_COLLECT_ALIAS)
OSOE_ALIAS(collectmem)
OSOE_ALIAS(fcollectmem)
OSOE_ALIAS(collect32)
OSOE_ALIAS(collect64)
OSOE_ALIAS(fcollect32)
OSOE_ALIAS(fcollect64)
