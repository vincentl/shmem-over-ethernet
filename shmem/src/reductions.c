/*
 * The team-based reductions (spec §9.10.9): shmem_TYPENAME_OP_reduce for every
 * operator and type of Table 10, over the accumulator (collectives.c).
 *
 * Each is a call to osoe_reduce naming its operation and its element - the
 * kind and width of its C type (internode.h) - generated here per operator and
 * type from shmem.h's lists, the same lists the declarations and pshmem.h come
 * from, so a type appears once. The arithmetic is combine.c's, per element, and
 * the internode phase's is the backend's (internode.h).
 *
 * The deprecated active-set forms, shmem_TYPENAME_OP_to_all over Table 11,
 * are the same reductions over the whole job (collectives.h), the team form's
 * engine on SHMEM_TEAM_WORLD with pSync untouched, and active-set.c's over any
 * other set. pWrk is never used, which makes no demand on its size.
 *
 * Complexity: O(1) here; the reduction's own in collectives.c.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pshmem.h>
#include <shmem.h>

#include "collectives.h"
#include "export.h"
#include "fatal.h"
#include "internode.h"

/* The element of a C type in each of Table 10's families. An integer's kind is
 * its signedness, so char is whichever char this platform has. */
#define OSOE_INTEGER_ELEMENT(T)                                                       \
    ((struct osoe_element){ (T)0 < (T)-1 ? OSOE_ELEMENT_UNSIGNED_INTEGER               \
                                         : OSOE_ELEMENT_SIGNED_INTEGER, sizeof(T) })
#define OSOE_FLOATING_ELEMENT(T) ((struct osoe_element){ OSOE_ELEMENT_FLOATING, sizeof(T) })
#define OSOE_COMPLEX_ELEMENT(T) ((struct osoe_element){ OSOE_ELEMENT_COMPLEX, sizeof(T) })

/* The routines, the context-free team form (§9.10.9 has no context form). */
#define OSOE_REDUCE_DEFINE(T, NAME, OPERATOR, OPERATION, ELEMENT)                     \
    OSOE_API int OSOE_FN(pshmem_, NAME, _##OPERATOR##_reduce)(shmem_team_t team,      \
                                         T *dest, const T *source, size_t nreduce)    \
    {                                                                                 \
        return osoe_reduce("shmem_" #NAME "_" #OPERATOR "_reduce", team, dest,        \
                           source, nreduce, OPERATION, ELEMENT);                      \
    }

#define OSOE_REDUCE_DEFINE_BITWISE(T, NAME)                                           \
    OSOE_REDUCE_DEFINE(T, NAME, and, OSOE_REDUCTION_AND, OSOE_INTEGER_ELEMENT(T))     \
    OSOE_REDUCE_DEFINE(T, NAME, or, OSOE_REDUCTION_OR, OSOE_INTEGER_ELEMENT(T))       \
    OSOE_REDUCE_DEFINE(T, NAME, xor, OSOE_REDUCTION_XOR, OSOE_INTEGER_ELEMENT(T))
#define OSOE_REDUCE_DEFINE_ORDERED(T, NAME, ELEMENT)                                  \
    OSOE_REDUCE_DEFINE(T, NAME, max, OSOE_REDUCTION_MAXIMUM, ELEMENT(T))              \
    OSOE_REDUCE_DEFINE(T, NAME, min, OSOE_REDUCTION_MINIMUM, ELEMENT(T))
#define OSOE_REDUCE_DEFINE_ARITHMETIC(T, NAME, ELEMENT)                               \
    OSOE_REDUCE_DEFINE(T, NAME, sum, OSOE_REDUCTION_SUM, ELEMENT(T))                  \
    OSOE_REDUCE_DEFINE(T, NAME, prod, OSOE_REDUCTION_PRODUCT, ELEMENT(T))

#define OSOE_REDUCE_DEFINE_INTEGER(T, NAME)                                           \
    OSOE_REDUCE_DEFINE_ORDERED(T, NAME, OSOE_INTEGER_ELEMENT)                         \
    OSOE_REDUCE_DEFINE_ARITHMETIC(T, NAME, OSOE_INTEGER_ELEMENT)
#define OSOE_REDUCE_DEFINE_FLOATING(T, NAME)                                          \
    OSOE_REDUCE_DEFINE_ORDERED(T, NAME, OSOE_FLOATING_ELEMENT)                        \
    OSOE_REDUCE_DEFINE_ARITHMETIC(T, NAME, OSOE_FLOATING_ELEMENT)
#define OSOE_REDUCE_DEFINE_COMPLEX(T, NAME)                                           \
    OSOE_REDUCE_DEFINE_ARITHMETIC(T, NAME, OSOE_COMPLEX_ELEMENT)

OSOE_REDUCE_BITWISE_TYPES(OSOE_REDUCE_DEFINE_BITWISE)
OSOE_REDUCE_INTEGER_TYPES(OSOE_REDUCE_DEFINE_INTEGER)
OSOE_REDUCE_FLOATING_TYPES(OSOE_REDUCE_DEFINE_FLOATING)
OSOE_REDUCE_COMPLEX_TYPES(OSOE_REDUCE_DEFINE_COMPLEX)

/* The weak `shmem_` aliases (export.h), over the same lists. */
#define OSOE_REDUCE_ALIAS_BITWISE(T, NAME)                                            \
    OSOE_ALIAS(NAME##_and_reduce) OSOE_ALIAS(NAME##_or_reduce)                        \
    OSOE_ALIAS(NAME##_xor_reduce)
#define OSOE_REDUCE_ALIAS_ORDERED(T, NAME)                                            \
    OSOE_ALIAS(NAME##_max_reduce) OSOE_ALIAS(NAME##_min_reduce)
#define OSOE_REDUCE_ALIAS_ARITHMETIC(T, NAME)                                         \
    OSOE_ALIAS(NAME##_sum_reduce) OSOE_ALIAS(NAME##_prod_reduce)

OSOE_REDUCE_BITWISE_TYPES(OSOE_REDUCE_ALIAS_BITWISE)
OSOE_REDUCE_INTEGER_TYPES(OSOE_REDUCE_ALIAS_ORDERED)
OSOE_REDUCE_FLOATING_TYPES(OSOE_REDUCE_ALIAS_ORDERED)
OSOE_REDUCE_INTEGER_TYPES(OSOE_REDUCE_ALIAS_ARITHMETIC)
OSOE_REDUCE_FLOATING_TYPES(OSOE_REDUCE_ALIAS_ARITHMETIC)
OSOE_REDUCE_COMPLEX_TYPES(OSOE_REDUCE_ALIAS_ARITHMETIC)

/* The deprecated active-set forms (Annex F; Table 11), on any active set. */
static void to_all(const char *routine, void *dest, const void *source, int nreduce,
                   int PE_start, int logPE_stride, int PE_size, long *pSync,
                   enum osoe_reduction_operation operation, struct osoe_element element)
{
    struct osoe_active_set s;
    bool whole = osoe_active_set_is_whole_job(routine, PE_start, logPE_stride, PE_size, &s);
    if (nreduce < 0)
        osoe_fatal("%s: nreduce %d is negative", routine, nreduce);
    if (whole)
        osoe_reduce(routine, SHMEM_TEAM_WORLD, dest, source, (size_t)nreduce, operation,
                    element);
    else
        osoe_active_set_reduce(routine, &s, dest, source, (size_t)nreduce, operation, element,
                               pSync);
}

#define OSOE_TO_ALL_DEFINE(T, NAME, OPERATOR, OPERATION, ELEMENT)                     \
    OSOE_API void OSOE_FN(pshmem_, NAME, _##OPERATOR##_to_all)(T *dest, const T *source, \
        int nreduce, int PE_start, int logPE_stride, int PE_size, T *pWrk, long *pSync) \
    {                                                                                 \
        (void)pWrk;                                                                   \
        to_all("shmem_" #NAME "_" #OPERATOR "_to_all", dest, source, nreduce, PE_start, \
               logPE_stride, PE_size, pSync, OPERATION, ELEMENT);                      \
    }

#define OSOE_TO_ALL_DEFINE_INTEGER(T, NAME)                                           \
    OSOE_TO_ALL_DEFINE(T, NAME, and, OSOE_REDUCTION_AND, OSOE_INTEGER_ELEMENT(T))     \
    OSOE_TO_ALL_DEFINE(T, NAME, or, OSOE_REDUCTION_OR, OSOE_INTEGER_ELEMENT(T))       \
    OSOE_TO_ALL_DEFINE(T, NAME, xor, OSOE_REDUCTION_XOR, OSOE_INTEGER_ELEMENT(T))     \
    OSOE_TO_ALL_DEFINE(T, NAME, max, OSOE_REDUCTION_MAXIMUM, OSOE_INTEGER_ELEMENT(T)) \
    OSOE_TO_ALL_DEFINE(T, NAME, min, OSOE_REDUCTION_MINIMUM, OSOE_INTEGER_ELEMENT(T)) \
    OSOE_TO_ALL_DEFINE(T, NAME, sum, OSOE_REDUCTION_SUM, OSOE_INTEGER_ELEMENT(T))     \
    OSOE_TO_ALL_DEFINE(T, NAME, prod, OSOE_REDUCTION_PRODUCT, OSOE_INTEGER_ELEMENT(T))
#define OSOE_TO_ALL_DEFINE_FLOATING(T, NAME)                                          \
    OSOE_TO_ALL_DEFINE(T, NAME, max, OSOE_REDUCTION_MAXIMUM, OSOE_FLOATING_ELEMENT(T)) \
    OSOE_TO_ALL_DEFINE(T, NAME, min, OSOE_REDUCTION_MINIMUM, OSOE_FLOATING_ELEMENT(T)) \
    OSOE_TO_ALL_DEFINE(T, NAME, sum, OSOE_REDUCTION_SUM, OSOE_FLOATING_ELEMENT(T))    \
    OSOE_TO_ALL_DEFINE(T, NAME, prod, OSOE_REDUCTION_PRODUCT, OSOE_FLOATING_ELEMENT(T))
#define OSOE_TO_ALL_DEFINE_COMPLEX(T, NAME)                                           \
    OSOE_TO_ALL_DEFINE(T, NAME, sum, OSOE_REDUCTION_SUM, OSOE_COMPLEX_ELEMENT(T))     \
    OSOE_TO_ALL_DEFINE(T, NAME, prod, OSOE_REDUCTION_PRODUCT, OSOE_COMPLEX_ELEMENT(T))

OSOE_TO_ALL_INTEGER_TYPES(OSOE_TO_ALL_DEFINE_INTEGER)
OSOE_TO_ALL_FLOATING_TYPES(OSOE_TO_ALL_DEFINE_FLOATING)
OSOE_TO_ALL_COMPLEX_TYPES(OSOE_TO_ALL_DEFINE_COMPLEX)

#define OSOE_TO_ALL_ALIAS_INTEGER(T, NAME)                                            \
    OSOE_ALIAS(NAME##_and_to_all) OSOE_ALIAS(NAME##_or_to_all)                        \
    OSOE_ALIAS(NAME##_xor_to_all) OSOE_ALIAS(NAME##_max_to_all)                       \
    OSOE_ALIAS(NAME##_min_to_all) OSOE_ALIAS(NAME##_sum_to_all)                       \
    OSOE_ALIAS(NAME##_prod_to_all)
#define OSOE_TO_ALL_ALIAS_FLOATING(T, NAME)                                           \
    OSOE_ALIAS(NAME##_max_to_all) OSOE_ALIAS(NAME##_min_to_all)                       \
    OSOE_ALIAS(NAME##_sum_to_all) OSOE_ALIAS(NAME##_prod_to_all)
#define OSOE_TO_ALL_ALIAS_COMPLEX(T, NAME)                                            \
    OSOE_ALIAS(NAME##_sum_to_all) OSOE_ALIAS(NAME##_prod_to_all)

OSOE_TO_ALL_INTEGER_TYPES(OSOE_TO_ALL_ALIAS_INTEGER)
OSOE_TO_ALL_FLOATING_TYPES(OSOE_TO_ALL_ALIAS_FLOATING)
OSOE_TO_ALL_COMPLEX_TYPES(OSOE_TO_ALL_ALIAS_COMPLEX)
