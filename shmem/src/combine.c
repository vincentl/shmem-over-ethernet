/*
 * The reductions' combine functions, one per operation and element, and the
 * table that finds them (combine.h).
 */
#include <stddef.h>
#include <stdint.h>

#include "combine.h"

#define OSOE_COMBINE(T, NAME, OPERATION, EXPRESSION)                                  \
    static void combine_##NAME##_##OPERATION(void *accumulator, const void *in,       \
                                              size_t count)                            \
    {                                                                                 \
        T *a = accumulator;                                                           \
        const T *b = in;                                                              \
        for (size_t i = 0; i < count; i++) {                                          \
            T x = a[i], y = b[i];                                                     \
            a[i] = (EXPRESSION);                                                      \
        }                                                                             \
    }

/* The integers: every operation, the arithmetic wrapping (combine.h). */
#define OSOE_COMBINE_INTEGER(T, NAME)                                                 \
    OSOE_COMBINE(T, NAME, and, (T)(x & y))                                            \
    OSOE_COMBINE(T, NAME, or, (T)(x | y))                                             \
    OSOE_COMBINE(T, NAME, xor, (T)(x ^ y))                                            \
    OSOE_COMBINE(T, NAME, maximum, x > y ? x : y)                                     \
    OSOE_COMBINE(T, NAME, minimum, x < y ? x : y)                                     \
    OSOE_COMBINE(T, NAME, sum, (T)((uintmax_t)x + (uintmax_t)y))                      \
    OSOE_COMBINE(T, NAME, product, (T)((uintmax_t)x * (uintmax_t)y))

/* The floating types: the orderings and the arithmetic. */
#define OSOE_COMBINE_FLOATING(T, NAME)                                                \
    OSOE_COMBINE(T, NAME, maximum, x > y ? x : y)                                     \
    OSOE_COMBINE(T, NAME, minimum, x < y ? x : y)                                     \
    OSOE_COMBINE(T, NAME, sum, x + y)                                                 \
    OSOE_COMBINE(T, NAME, product, x * y)

/* The complex types: the arithmetic only. */
#define OSOE_COMBINE_COMPLEX(T, NAME)                                                 \
    OSOE_COMBINE(T, NAME, sum, x + y)                                                 \
    OSOE_COMBINE(T, NAME, product, x * y)

OSOE_COMBINE_INTEGER(int8_t, int8)
OSOE_COMBINE_INTEGER(int16_t, int16)
OSOE_COMBINE_INTEGER(int32_t, int32)
OSOE_COMBINE_INTEGER(int64_t, int64)
OSOE_COMBINE_INTEGER(uint8_t, uint8)
OSOE_COMBINE_INTEGER(uint16_t, uint16)
OSOE_COMBINE_INTEGER(uint32_t, uint32)
OSOE_COMBINE_INTEGER(uint64_t, uint64)
OSOE_COMBINE_FLOATING(float, float)
OSOE_COMBINE_FLOATING(double, double)
OSOE_COMBINE_FLOATING(long double, long_double)
#ifndef __STDC_NO_COMPLEX__
OSOE_COMBINE_COMPLEX(float _Complex, complex_float)
OSOE_COMBINE_COMPLEX(double _Complex, complex_double)
#endif

enum row {
    ROW_INT8, ROW_INT16, ROW_INT32, ROW_INT64, ROW_UINT8, ROW_UINT16, ROW_UINT32, ROW_UINT64,
    ROW_FLOAT, ROW_DOUBLE, ROW_LONG_DOUBLE, ROW_COMPLEX_FLOAT, ROW_COMPLEX_DOUBLE,
    ROW_ELEMENTS
};

#define OSOE_ROW_INTEGER(NAME)                                                        \
    [OSOE_REDUCTION_AND] = combine_##NAME##_and,                                      \
    [OSOE_REDUCTION_OR] = combine_##NAME##_or,                                        \
    [OSOE_REDUCTION_XOR] = combine_##NAME##_xor,                                      \
    [OSOE_REDUCTION_MAXIMUM] = combine_##NAME##_maximum,                              \
    [OSOE_REDUCTION_MINIMUM] = combine_##NAME##_minimum,                              \
    [OSOE_REDUCTION_SUM] = combine_##NAME##_sum,                                      \
    [OSOE_REDUCTION_PRODUCT] = combine_##NAME##_product
#define OSOE_ROW_FLOATING(NAME)                                                       \
    [OSOE_REDUCTION_MAXIMUM] = combine_##NAME##_maximum,                              \
    [OSOE_REDUCTION_MINIMUM] = combine_##NAME##_minimum,                              \
    [OSOE_REDUCTION_SUM] = combine_##NAME##_sum,                                      \
    [OSOE_REDUCTION_PRODUCT] = combine_##NAME##_product
#define OSOE_ROW_COMPLEX(NAME)                                                        \
    [OSOE_REDUCTION_SUM] = combine_##NAME##_sum,                                      \
    [OSOE_REDUCTION_PRODUCT] = combine_##NAME##_product

enum { OPERATIONS = OSOE_REDUCTION_PRODUCT + 1 };

/* Every pair Table 10 has is filled; every other entry is NULL. */
static const osoe_combine_function table[ROW_ELEMENTS][OPERATIONS] = {
    [ROW_INT8] = { OSOE_ROW_INTEGER(int8) },
    [ROW_INT16] = { OSOE_ROW_INTEGER(int16) },
    [ROW_INT32] = { OSOE_ROW_INTEGER(int32) },
    [ROW_INT64] = { OSOE_ROW_INTEGER(int64) },
    [ROW_UINT8] = { OSOE_ROW_INTEGER(uint8) },
    [ROW_UINT16] = { OSOE_ROW_INTEGER(uint16) },
    [ROW_UINT32] = { OSOE_ROW_INTEGER(uint32) },
    [ROW_UINT64] = { OSOE_ROW_INTEGER(uint64) },
    [ROW_FLOAT] = { OSOE_ROW_FLOATING(float) },
    [ROW_DOUBLE] = { OSOE_ROW_FLOATING(double) },
    [ROW_LONG_DOUBLE] = { OSOE_ROW_FLOATING(long_double) },
#ifndef __STDC_NO_COMPLEX__
    [ROW_COMPLEX_FLOAT] = { OSOE_ROW_COMPLEX(complex_float) },
    [ROW_COMPLEX_DOUBLE] = { OSOE_ROW_COMPLEX(complex_double) },
#endif
};

/* The row for `element`, or ROW_ELEMENTS if this platform has no such element. The
 * floating widths are tested narrowest first, so a platform whose long double
 * is a double reduces it as one - the same arithmetic. */
static enum row row_of(struct osoe_element element)
{
    uint32_t width = element.width;
    switch (element.kind) {
    case OSOE_ELEMENT_SIGNED_INTEGER:
        return width == 1 ? ROW_INT8 : width == 2 ? ROW_INT16 : width == 4 ? ROW_INT32
             : width == 8 ? ROW_INT64 : ROW_ELEMENTS;
    case OSOE_ELEMENT_UNSIGNED_INTEGER:
        return width == 1 ? ROW_UINT8 : width == 2 ? ROW_UINT16 : width == 4 ? ROW_UINT32
             : width == 8 ? ROW_UINT64 : ROW_ELEMENTS;
    case OSOE_ELEMENT_FLOATING:
        return width == sizeof(float) ? ROW_FLOAT : width == sizeof(double) ? ROW_DOUBLE
             : width == sizeof(long double) ? ROW_LONG_DOUBLE : ROW_ELEMENTS;
    case OSOE_ELEMENT_COMPLEX:
#ifndef __STDC_NO_COMPLEX__
        return width == sizeof(float _Complex) ? ROW_COMPLEX_FLOAT
             : width == sizeof(double _Complex) ? ROW_COMPLEX_DOUBLE : ROW_ELEMENTS;
#else
        return ROW_ELEMENTS;
#endif
    }
    return ROW_ELEMENTS;
}

osoe_combine_function osoe_combine_lookup(enum osoe_reduction_operation operation,
                                          struct osoe_element element)
{
    enum row row = row_of(element);
    if (row == ROW_ELEMENTS || (unsigned)operation >= OPERATIONS)
        return NULL;
    return table[row][operation];
}
