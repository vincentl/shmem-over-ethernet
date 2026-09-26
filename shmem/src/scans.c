/*
 * The prefix scans (spec §9.10.10): shmem_TYPENAME_sum_inscan and
 * shmem_TYPENAME_sum_exscan for every integer, floating and complex type Table
 * 10 allows SUM on, over osoe_scan (collectives.c), generated from shmem.h's
 * lists as the reductions are (reductions.c), so a type appears once.
 *
 * Complexity: O(1) here; the scan's own in collectives.c and internode.h.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pshmem.h>
#include <shmem.h>

#include "collectives.h"
#include "export.h"

#define OSOE_SCAN_INTEGER_ELEMENT(T)                                                  \
    ((struct osoe_element){ (T)0 < (T)-1 ? OSOE_ELEMENT_UNSIGNED_INTEGER               \
                                         : OSOE_ELEMENT_SIGNED_INTEGER, sizeof(T) })
#define OSOE_SCAN_FLOATING_ELEMENT(T) ((struct osoe_element){ OSOE_ELEMENT_FLOATING, sizeof(T) })
#define OSOE_SCAN_COMPLEX_ELEMENT(T) ((struct osoe_element){ OSOE_ELEMENT_COMPLEX, sizeof(T) })

#define OSOE_SCAN_DEFINE(T, NAME, ELEMENT)                                            \
    OSOE_API int OSOE_FN(pshmem_, NAME, _sum_inscan)(shmem_team_t team, T *dest,      \
                                                     const T *source, size_t nelems)  \
    {                                                                                 \
        return osoe_scan("shmem_" #NAME "_sum_inscan", team, dest, source, nelems,    \
                         ELEMENT(T), true);                                           \
    }                                                                                 \
    OSOE_API int OSOE_FN(pshmem_, NAME, _sum_exscan)(shmem_team_t team, T *dest,      \
                                                     const T *source, size_t nelems)  \
    {                                                                                 \
        return osoe_scan("shmem_" #NAME "_sum_exscan", team, dest, source, nelems,    \
                         ELEMENT(T), false);                                          \
    }
#define OSOE_SCAN_DEFINE_INTEGER(T, NAME) OSOE_SCAN_DEFINE(T, NAME, OSOE_SCAN_INTEGER_ELEMENT)
#define OSOE_SCAN_DEFINE_FLOATING(T, NAME) OSOE_SCAN_DEFINE(T, NAME, OSOE_SCAN_FLOATING_ELEMENT)
#define OSOE_SCAN_DEFINE_COMPLEX(T, NAME) OSOE_SCAN_DEFINE(T, NAME, OSOE_SCAN_COMPLEX_ELEMENT)

OSOE_REDUCE_INTEGER_TYPES(OSOE_SCAN_DEFINE_INTEGER)
OSOE_REDUCE_FLOATING_TYPES(OSOE_SCAN_DEFINE_FLOATING)
OSOE_REDUCE_COMPLEX_TYPES(OSOE_SCAN_DEFINE_COMPLEX)

/* The weak `shmem_` aliases (export.h), over the same lists. */
#define OSOE_SCAN_ALIAS(T, NAME) OSOE_ALIAS(NAME##_sum_inscan) OSOE_ALIAS(NAME##_sum_exscan)

OSOE_REDUCE_INTEGER_TYPES(OSOE_SCAN_ALIAS)
OSOE_REDUCE_FLOATING_TYPES(OSOE_SCAN_ALIAS)
OSOE_REDUCE_COMPLEX_TYPES(OSOE_SCAN_ALIAS)
