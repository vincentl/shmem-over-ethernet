/*
 * Atomic memory operations (spec §9.7) over the boundary's rmw (§4.10).
 *
 * The spec's twenty-odd routine names per type are one parameterized
 * read-modify-write: set and swap are a write, fetch is a read, inc and add are
 * an add, and the bitwise routines pass their operator. What varies across
 * Tables 6, 7 and 8 is only which *types* get which subset, so the definitions
 * are three macros over the same three type lists shmem.h declares from. A type
 * appears in one place.
 *
 * Every type in the three tables is four or eight bytes here, which is what the
 * boundary's rmw takes; the static assertions below say so rather than leaving
 * it to be discovered by a link error on some other platform. They also check
 * that each exact-width typedef is the canonical type shmem.h's C11 generic
 * selection lists, because that selection must name one association per
 * *distinct type* and would silently omit a table entry if, say, int64_t were
 * long long here instead of long.
 *
 * Table 7 adds float and double. They are a bit-pattern move through the same
 * rmw - fetch, set and swap are the only operations the spec gives them, and
 * none of the three interprets the value - so they are carried through the
 * width-matched unsigned type with memcpy, which is the conversion the C
 * standard defines. No arithmetic is ever done on a float here, so the fact
 * that the transport's add is integer never arises.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <shmem.h>

#include "export.h"
#include "metrics.h"
#include "sym.h"
#include "team.h"
#include "transport.h"

#define OSOE_AMO_ASSERT_WIDTH(T, NAME)                                                \
    _Static_assert(sizeof(T) == 4 || sizeof(T) == 8,                                  \
                   "every AMO type must be 4 or 8 bytes for the boundary's rmw");
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_ASSERT_WIDTH)
OSOE_AMO_FLOAT_TYPES(OSOE_AMO_ASSERT_WIDTH)

/* The assumption shmem.h's C11 selection rests on, checked where it is cheap to
 * check: the library fails to build rather than quietly omitting a case. */
#if !defined(__cplusplus) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(_Generic((int32_t)0, int: 1, default: 0), "int32_t must be int");
_Static_assert(_Generic((uint32_t)0, unsigned int: 1, default: 0),
               "uint32_t must be unsigned int");
_Static_assert(_Generic((int64_t)0, long: 1, default: 0), "int64_t must be long");
_Static_assert(_Generic((uint64_t)0, unsigned long: 1, default: 0),
               "uint64_t must be unsigned long");
_Static_assert(_Generic((size_t)0, unsigned long: 1, default: 0),
               "size_t must be unsigned long");
_Static_assert(_Generic((ptrdiff_t)0, long: 1, default: 0), "ptrdiff_t must be long");
#endif

/* An AMO's target must be a word in the heap, aligned for its width. */
static uint64_t amo_offset(const char *routine, const void *addr, unsigned width, int pe)
{
    osoe_check_pe(routine, pe);
    return osoe_sym_word_offset(routine, addr, width);
}

static uint64_t amo_rmw(const char *routine, shmem_ctx_t ctx, const void *dest,
                        unsigned width, enum osoe_rmw_op op, uint64_t value,
                        uint64_t compare, unsigned flags, int pe)
{
    uint64_t offset = amo_offset(routine, dest, width, pe);
    return osoe_tp_rmw(osoe_ctx_of(routine, ctx), pe, OSOE_REGION_HEAP, offset, op, width,
                       value, compare, flags);
}

/*
 * Carrying a value in and out of a uint64_t. Going in, a narrower or signed
 * type converts to uint64_t by the standard's modular rule and the rmw uses
 * only the low `width` bytes. Coming out, the zero-extended result converts
 * back; for a signed type whose value does not fit, that conversion is
 * implementation-defined, and every compiler this project builds with defines
 * it as the two's-complement pattern - which is the only reading under which a
 * fetching AMO on a signed type means anything at all.
 */
#define OSOE_AMO_IN(v)  ((uint64_t)(v))
#define OSOE_AMO_OUT(T, r) ((T)(r))

/*
 * Both spellings of every routine, from one body (§7.3). The context form is
 * the definition and the plain form calls it with SHMEM_CTX_DEFAULT, so the
 * parameter is carried on the real path from the start rather than being a
 * wrapper that discards it - which is the shape that breaks when a second
 * context exists.
 */

/* ---- Table 6: the standard AMO types ---------------------------------- */

#define OSOE_AMO_STANDARD(T, NAME)                                                     \
    OSOE_API T shmem_ctx_##NAME##_atomic_compare_swap(shmem_ctx_t ctx, T *dest, T cond, \
                                                      T value, int pe)                  \
    {                                                                                   \
        return OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_compare_swap", ctx,      \
                                       dest, (unsigned)sizeof(T), OSOE_RMW_CSWAP,       \
                                       OSOE_AMO_IN(value), OSOE_AMO_IN(cond),           \
                                       OSOE_TP_FETCH, pe));                             \
    }                                                                                   \
    OSOE_API T shmem_##NAME##_atomic_compare_swap(T *dest, T cond, T value, int pe)     \
    {                                                                                   \
        return shmem_ctx_##NAME##_atomic_compare_swap(SHMEM_CTX_DEFAULT, dest, cond,    \
                                                      value, pe);                       \
    }                                                                                   \
    OSOE_API void shmem_ctx_##NAME##_atomic_inc(shmem_ctx_t ctx, T *dest, int pe)       \
    {                                                                                   \
        amo_rmw("shmem_" #NAME "_atomic_inc", ctx, dest, (unsigned)sizeof(T),           \
                OSOE_RMW_ADD, 1, 0, 0, pe);                                             \
    }                                                                                   \
    OSOE_API void shmem_##NAME##_atomic_inc(T *dest, int pe)                            \
    {                                                                                   \
        shmem_ctx_##NAME##_atomic_inc(SHMEM_CTX_DEFAULT, dest, pe);                     \
    }                                                                                   \
    OSOE_API T shmem_ctx_##NAME##_atomic_fetch_inc(shmem_ctx_t ctx, T *dest, int pe)    \
    {                                                                                   \
        return OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_fetch_inc", ctx, dest,   \
                                       (unsigned)sizeof(T), OSOE_RMW_ADD, 1, 0,         \
                                       OSOE_TP_FETCH, pe));                             \
    }                                                                                   \
    OSOE_API T shmem_##NAME##_atomic_fetch_inc(T *dest, int pe)                         \
    {                                                                                   \
        return shmem_ctx_##NAME##_atomic_fetch_inc(SHMEM_CTX_DEFAULT, dest, pe);        \
    }                                                                                   \
    OSOE_API void shmem_ctx_##NAME##_atomic_add(shmem_ctx_t ctx, T *dest, T value,      \
                                                int pe)                                 \
    {                                                                                   \
        amo_rmw("shmem_" #NAME "_atomic_add", ctx, dest, (unsigned)sizeof(T),           \
                OSOE_RMW_ADD, OSOE_AMO_IN(value), 0, 0, pe);                            \
    }                                                                                   \
    OSOE_API void shmem_##NAME##_atomic_add(T *dest, T value, int pe)                   \
    {                                                                                   \
        shmem_ctx_##NAME##_atomic_add(SHMEM_CTX_DEFAULT, dest, value, pe);              \
    }                                                                                   \
    OSOE_API T shmem_ctx_##NAME##_atomic_fetch_add(shmem_ctx_t ctx, T *dest, T value,   \
                                                   int pe)                              \
    {                                                                                   \
        return OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_fetch_add", ctx, dest,   \
                                       (unsigned)sizeof(T), OSOE_RMW_ADD,               \
                                       OSOE_AMO_IN(value), 0, OSOE_TP_FETCH, pe));      \
    }                                                                                   \
    OSOE_API T shmem_##NAME##_atomic_fetch_add(T *dest, T value, int pe)                \
    {                                                                                   \
        return shmem_ctx_##NAME##_atomic_fetch_add(SHMEM_CTX_DEFAULT, dest, value, pe); \
    }

/* ---- Table 7: the extended AMO types, integers ------------------------ */

#define OSOE_AMO_EXTENDED(T, NAME)                                                      \
    OSOE_API T shmem_ctx_##NAME##_atomic_fetch(shmem_ctx_t ctx, const T *source, int pe) \
    {                                                                                    \
        return OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_fetch", ctx, source,      \
                                       (unsigned)sizeof(T), OSOE_RMW_READ, 0, 0,         \
                                       OSOE_TP_FETCH, pe));                              \
    }                                                                                    \
    OSOE_API T shmem_##NAME##_atomic_fetch(const T *source, int pe)                      \
    {                                                                                    \
        return shmem_ctx_##NAME##_atomic_fetch(SHMEM_CTX_DEFAULT, source, pe);           \
    }                                                                                    \
    OSOE_API void shmem_ctx_##NAME##_atomic_set(shmem_ctx_t ctx, T *dest, T value,       \
                                                int pe)                                  \
    {                                                                                    \
        amo_rmw("shmem_" #NAME "_atomic_set", ctx, dest, (unsigned)sizeof(T),            \
                OSOE_RMW_WRITE, OSOE_AMO_IN(value), 0, 0, pe);                           \
    }                                                                                    \
    OSOE_API void shmem_##NAME##_atomic_set(T *dest, T value, int pe)                    \
    {                                                                                    \
        shmem_ctx_##NAME##_atomic_set(SHMEM_CTX_DEFAULT, dest, value, pe);               \
    }                                                                                    \
    OSOE_API T shmem_ctx_##NAME##_atomic_swap(shmem_ctx_t ctx, T *dest, T value, int pe) \
    {                                                                                    \
        return OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_swap", ctx, dest,         \
                                       (unsigned)sizeof(T), OSOE_RMW_WRITE,              \
                                       OSOE_AMO_IN(value), 0, OSOE_TP_FETCH, pe));       \
    }                                                                                    \
    OSOE_API T shmem_##NAME##_atomic_swap(T *dest, T value, int pe)                      \
    {                                                                                    \
        return shmem_ctx_##NAME##_atomic_swap(SHMEM_CTX_DEFAULT, dest, value, pe);       \
    }

/* ---- Table 7: float and double, as bit patterns ----------------------- */

#define OSOE_AMO_FLOAT(T, NAME, U)                                                       \
    static uint64_t NAME##_bits(T v)                                                     \
    {                                                                                    \
        U u;                                                                             \
        memcpy(&u, &v, sizeof u);                                                        \
        return u;                                                                        \
    }                                                                                    \
    static T NAME##_value(uint64_t bits)                                                 \
    {                                                                                    \
        U u = (U)bits;                                                                   \
        T v;                                                                             \
        memcpy(&v, &u, sizeof v);                                                        \
        return v;                                                                        \
    }                                                                                    \
    OSOE_API T shmem_ctx_##NAME##_atomic_fetch(shmem_ctx_t ctx, const T *source, int pe)  \
    {                                                                                    \
        return NAME##_value(amo_rmw("shmem_" #NAME "_atomic_fetch", ctx, source,         \
                                    (unsigned)sizeof(T), OSOE_RMW_READ, 0, 0,            \
                                    OSOE_TP_FETCH, pe));                                 \
    }                                                                                    \
    OSOE_API T shmem_##NAME##_atomic_fetch(const T *source, int pe)                       \
    {                                                                                    \
        return shmem_ctx_##NAME##_atomic_fetch(SHMEM_CTX_DEFAULT, source, pe);           \
    }                                                                                    \
    OSOE_API void shmem_ctx_##NAME##_atomic_set(shmem_ctx_t ctx, T *dest, T value,        \
                                                int pe)                                  \
    {                                                                                    \
        amo_rmw("shmem_" #NAME "_atomic_set", ctx, dest, (unsigned)sizeof(T),            \
                OSOE_RMW_WRITE, NAME##_bits(value), 0, 0, pe);                           \
    }                                                                                    \
    OSOE_API void shmem_##NAME##_atomic_set(T *dest, T value, int pe)                     \
    {                                                                                    \
        shmem_ctx_##NAME##_atomic_set(SHMEM_CTX_DEFAULT, dest, value, pe);               \
    }                                                                                    \
    OSOE_API T shmem_ctx_##NAME##_atomic_swap(shmem_ctx_t ctx, T *dest, T value, int pe)  \
    {                                                                                    \
        return NAME##_value(amo_rmw("shmem_" #NAME "_atomic_swap", ctx, dest,            \
                                    (unsigned)sizeof(T), OSOE_RMW_WRITE,                 \
                                    NAME##_bits(value), 0, OSOE_TP_FETCH, pe));          \
    }                                                                                    \
    OSOE_API T shmem_##NAME##_atomic_swap(T *dest, T value, int pe)                       \
    {                                                                                    \
        return shmem_ctx_##NAME##_atomic_swap(SHMEM_CTX_DEFAULT, dest, value, pe);       \
    }

/* ---- Table 8: the bitwise AMO types ----------------------------------- */

#define OSOE_AMO_BITWISE_OP(T, NAME, OP, RMW)                                            \
    OSOE_API void shmem_ctx_##NAME##_atomic_##OP(shmem_ctx_t ctx, T *dest, T value,       \
                                                 int pe)                                 \
    {                                                                                    \
        amo_rmw("shmem_" #NAME "_atomic_" #OP, ctx, dest, (unsigned)sizeof(T), RMW,      \
                OSOE_AMO_IN(value), 0, 0, pe);                                           \
    }                                                                                    \
    OSOE_API void shmem_##NAME##_atomic_##OP(T *dest, T value, int pe)                    \
    {                                                                                    \
        shmem_ctx_##NAME##_atomic_##OP(SHMEM_CTX_DEFAULT, dest, value, pe);              \
    }                                                                                    \
    OSOE_API T shmem_ctx_##NAME##_atomic_fetch_##OP(shmem_ctx_t ctx, T *dest, T value,    \
                                                    int pe)                              \
    {                                                                                    \
        return OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_fetch_" #OP, ctx, dest,   \
                                       (unsigned)sizeof(T), RMW, OSOE_AMO_IN(value), 0,  \
                                       OSOE_TP_FETCH, pe));                              \
    }                                                                                    \
    OSOE_API T shmem_##NAME##_atomic_fetch_##OP(T *dest, T value, int pe)                 \
    {                                                                                    \
        return shmem_ctx_##NAME##_atomic_fetch_##OP(SHMEM_CTX_DEFAULT, dest, value, pe); \
    }

#define OSOE_AMO_BITWISE(T, NAME)                                                        \
    OSOE_AMO_BITWISE_OP(T, NAME, and, OSOE_RMW_AND)                                      \
    OSOE_AMO_BITWISE_OP(T, NAME, or, OSOE_RMW_OR)                                        \
    OSOE_AMO_BITWISE_OP(T, NAME, xor, OSOE_RMW_XOR)

OSOE_AMO_STANDARD_TYPES(OSOE_AMO_STANDARD)
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_EXTENDED)
OSOE_AMO_FLOAT(float, float, uint32_t)
OSOE_AMO_FLOAT(double, double, uint64_t)
OSOE_AMO_BITWISE_TYPES(OSOE_AMO_BITWISE)
