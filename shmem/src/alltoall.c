/*
 * alltoall and alltoalls (spec §9.10.5, §9.10.6): shmem_TYPENAME_alltoall and
 * shmem_TYPENAME_alltoalls for every standard RMA type of Table 5, and
 * shmem_alltoallmem and shmem_alltoallsmem, over the exchange in
 * collectives.c - member i's block j to member j's dest block i, direct, then
 * the team's barrier. The deprecated active-set forms, alltoall32, alltoall64,
 * alltoalls32 and alltoalls64, run on any active set (collectives.h): the
 * world team's form for the whole job, pSync untouched; active-set.c's for any
 * other. Their elements are four and eight bytes.
 *
 * Complexity: P puts a member - one a block, or one an element when strided -
 * and a barrier.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pshmem.h>
#include <shmem.h>

#include "collectives.h"
#include "export.h"

#define OSOE_ALLTOALL_DEFINE(T, NAME)                                                 \
    OSOE_API int OSOE_FN(pshmem_, NAME, _alltoall)(shmem_team_t team, T *dest,        \
                                                   const T *source, size_t nelems)   \
    {                                                                                 \
        return osoe_alltoall("shmem_" #NAME "_alltoall", team, dest, source, 1, 1,    \
                             nelems, sizeof(T));                                      \
    }                                                                                 \
    OSOE_API int OSOE_FN(pshmem_, NAME, _alltoalls)(shmem_team_t team, T *dest,       \
                                                    const T *source, ptrdiff_t dst,   \
                                                    ptrdiff_t sst, size_t nelems)     \
    {                                                                                 \
        return osoe_alltoall("shmem_" #NAME "_alltoalls", team, dest, source, dst,    \
                             sst, nelems, sizeof(T));                                 \
    }

OSOE_RMA_STANDARD_TYPES(OSOE_ALLTOALL_DEFINE)

OSOE_API int pshmem_alltoallmem(shmem_team_t team, void *dest, const void *source,
                                size_t nelems)
{
    return osoe_alltoall("shmem_alltoallmem", team, dest, source, 1, 1, nelems, 1);
}

OSOE_API int pshmem_alltoallsmem(shmem_team_t team, void *dest, const void *source,
                                 ptrdiff_t dst, ptrdiff_t sst, size_t nelems)
{
    return osoe_alltoall("shmem_alltoallsmem", team, dest, source, dst, sst, nelems, 1);
}

/* The deprecated active-set forms (Annex F), on any active set. */
#define OSOE_ALLTOALL_ACTIVE_SET_DEFINE(BITS)                                         \
    OSOE_API void pshmem_alltoall##BITS(void *dest, const void *source, size_t nelems, \
                                        int PE_start, int logPE_stride, int PE_size,   \
                                        long *pSync)                                   \
    {                                                                                 \
        const char *routine = "shmem_alltoall" #BITS;                                 \
        struct osoe_active_set s;                                                     \
        if (osoe_active_set_is_whole_job(routine, PE_start, logPE_stride, PE_size, &s)) \
            osoe_alltoall(routine, SHMEM_TEAM_WORLD, dest, source, 1, 1, nelems, BITS / 8); \
        else                                                                          \
            osoe_active_set_alltoall(routine, &s, dest, source, 1, 1, nelems, BITS / 8, \
                                     pSync);                                          \
    }                                                                                 \
    OSOE_API void pshmem_alltoalls##BITS(void *dest, const void *source, ptrdiff_t dst, \
                                         ptrdiff_t sst, size_t nelems, int PE_start,  \
                                         int logPE_stride, int PE_size, long *pSync)  \
    {                                                                                 \
        const char *routine = "shmem_alltoalls" #BITS;                                \
        struct osoe_active_set s;                                                     \
        if (osoe_active_set_is_whole_job(routine, PE_start, logPE_stride, PE_size, &s)) \
            osoe_alltoall(routine, SHMEM_TEAM_WORLD, dest, source, dst, sst, nelems,   \
                          BITS / 8);                                                  \
        else                                                                          \
            osoe_active_set_alltoall(routine, &s, dest, source, dst, sst, nelems,     \
                                     BITS / 8, pSync);                                \
    }
OSOE_ALLTOALL_ACTIVE_SET_DEFINE(32)
OSOE_ALLTOALL_ACTIVE_SET_DEFINE(64)

/* The weak `shmem_` aliases (export.h). */
#define OSOE_ALLTOALL_ALIAS(T, NAME) OSOE_ALIAS(NAME##_alltoall) OSOE_ALIAS(NAME##_alltoalls)
OSOE_RMA_STANDARD_TYPES(OSOE_ALLTOALL_ALIAS)
OSOE_ALIAS(alltoallmem)
OSOE_ALIAS(alltoallsmem)
OSOE_ALIAS(alltoall32)
OSOE_ALIAS(alltoall64)
OSOE_ALIAS(alltoalls32)
OSOE_ALIAS(alltoalls64)
