/*
 * The team-based reductions (spec §9.10.9): shmem_TYPENAME_OP_reduce for every
 * operator and type of Table 10, over the accumulator (collectives.c).
 *
 * Each is a call to osoe_reduce with its element width and one combine
 * function, generated here per operator and type from shmem.h's lists - the
 * same lists the declarations and pshmem.h come from, so a type appears once.
 *
 * **Integer sums and products are computed modulo 2^N** through uintmax_t and
 * converted back: a signed overflow is undefined behaviour in C, where the
 * unsigned arithmetic and the conversion are defined (the conversion
 * implementation-defined, and modular on every target osoe builds for), so a
 * program that overflows a reduction gets the wrapped value on every PE rather
 * than a sanitizer's report or an optimiser's surprise. Floating and complex
 * arithmetic is the type's own. Maximum and minimum are the plain comparisons.
 *
 * Complexity: O(n) per combine, one combine per local member at the lead and
 * one per child at each node of the tree (collectives.c).
 */
#include <stddef.h>
#include <stdint.h>

#include <pshmem.h>
#include <shmem.h>

#include "collectives.h"
#include "export.h"

#define OSOE_COMBINE(T, NAME, OPERATOR, EXPRESSION)                                   \
    static void combine_##NAME##_##OPERATOR(void *acc, const void *in, size_t n)      \
    {                                                                                 \
        T *a = acc;                                                                   \
        const T *b = in;                                                              \
        for (size_t i = 0; i < n; i++) {                                              \
            T x = a[i], y = b[i];                                                     \
            a[i] = (EXPRESSION);                                                      \
        }                                                                             \
    }

#define OSOE_COMBINE_BITWISE(T, NAME)                                                 \
    OSOE_COMBINE(T, NAME, and, (T)(x & y))                                            \
    OSOE_COMBINE(T, NAME, or, (T)(x | y))                                             \
    OSOE_COMBINE(T, NAME, xor, (T)(x ^ y))
#define OSOE_COMBINE_ORDERED(T, NAME)                                                 \
    OSOE_COMBINE(T, NAME, max, x > y ? x : y)                                         \
    OSOE_COMBINE(T, NAME, min, x < y ? x : y)
#define OSOE_COMBINE_INTEGER_ARITHMETIC(T, NAME)                                      \
    OSOE_COMBINE(T, NAME, sum, (T)((uintmax_t)x + (uintmax_t)y))                      \
    OSOE_COMBINE(T, NAME, prod, (T)((uintmax_t)x * (uintmax_t)y))
#define OSOE_COMBINE_ARITHMETIC(T, NAME)                                              \
    OSOE_COMBINE(T, NAME, sum, x + y)                                                 \
    OSOE_COMBINE(T, NAME, prod, x * y)

OSOE_REDUCE_BITWISE_TYPES(OSOE_COMBINE_BITWISE)
OSOE_REDUCE_INTEGER_TYPES(OSOE_COMBINE_ORDERED)
OSOE_REDUCE_FLOATING_TYPES(OSOE_COMBINE_ORDERED)
OSOE_REDUCE_INTEGER_TYPES(OSOE_COMBINE_INTEGER_ARITHMETIC)
OSOE_REDUCE_FLOATING_TYPES(OSOE_COMBINE_ARITHMETIC)
OSOE_REDUCE_COMPLEX_TYPES(OSOE_COMBINE_ARITHMETIC)

/* The routines, the context-free team form (§9.10.9 has no context form). */
#define OSOE_REDUCE_DEFINE(T, NAME, OPERATOR)                                         \
    OSOE_API int OSOE_FN(pshmem_, NAME, _##OPERATOR##_reduce)(shmem_team_t team,      \
                                         T *dest, const T *source, size_t nreduce)    \
    {                                                                                 \
        return osoe_reduce("shmem_" #NAME "_" #OPERATOR "_reduce", team, dest,        \
                           source, nreduce, sizeof(T), combine_##NAME##_##OPERATOR);  \
    }

#define OSOE_REDUCE_DEFINE_BITWISE(T, NAME)                                           \
    OSOE_REDUCE_DEFINE(T, NAME, and) OSOE_REDUCE_DEFINE(T, NAME, or)                  \
    OSOE_REDUCE_DEFINE(T, NAME, xor)
#define OSOE_REDUCE_DEFINE_ORDERED(T, NAME)                                           \
    OSOE_REDUCE_DEFINE(T, NAME, max) OSOE_REDUCE_DEFINE(T, NAME, min)
#define OSOE_REDUCE_DEFINE_ARITHMETIC(T, NAME)                                        \
    OSOE_REDUCE_DEFINE(T, NAME, sum) OSOE_REDUCE_DEFINE(T, NAME, prod)

OSOE_REDUCE_BITWISE_TYPES(OSOE_REDUCE_DEFINE_BITWISE)
OSOE_REDUCE_INTEGER_TYPES(OSOE_REDUCE_DEFINE_ORDERED)
OSOE_REDUCE_FLOATING_TYPES(OSOE_REDUCE_DEFINE_ORDERED)
OSOE_REDUCE_INTEGER_TYPES(OSOE_REDUCE_DEFINE_ARITHMETIC)
OSOE_REDUCE_FLOATING_TYPES(OSOE_REDUCE_DEFINE_ARITHMETIC)
OSOE_REDUCE_COMPLEX_TYPES(OSOE_REDUCE_DEFINE_ARITHMETIC)

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
