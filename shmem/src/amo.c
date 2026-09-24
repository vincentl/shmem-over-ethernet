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

#include <pshmem.h>
#include <shmem.h>

#include "export.h"
#include "metrics.h"
#include "symmetric.h"
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

/* An AMO's target must be a symmetric word, aligned for its width. */
static struct osoe_symmetric_location amo_location(const char *routine, const void *addr,
                                                   unsigned width, int pe)
{
    osoe_check_pe(routine, pe);
    return osoe_symmetric_word_locate(routine, addr, width);
}

static uint64_t amo_rmw(const char *routine, shmem_ctx_t ctx, const void *dest,
                        unsigned width, enum osoe_rmw_op op, uint64_t value,
                        uint64_t compare, unsigned flags, int pe)
{
    struct osoe_symmetric_location at = amo_location(routine, dest, width, pe);
    return osoe_transport_rmw(osoe_ctx_of(routine, ctx), pe, at.region, at.offset, op, width,
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
    OSOE_API T pshmem_ctx_##NAME##_atomic_compare_swap(shmem_ctx_t ctx, T *dest, T cond, \
                                                      T value, int pe)                  \
    {                                                                                   \
        return OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_compare_swap", ctx,      \
                                       dest, (unsigned)sizeof(T), OSOE_RMW_CSWAP,       \
                                       OSOE_AMO_IN(value), OSOE_AMO_IN(cond),           \
                                       OSOE_TRANSPORT_FETCH, pe));                             \
    }                                                                                   \
    OSOE_API T pshmem_##NAME##_atomic_compare_swap(T *dest, T cond, T value, int pe)     \
    {                                                                                   \
        return pshmem_ctx_##NAME##_atomic_compare_swap(SHMEM_CTX_DEFAULT, dest, cond,    \
                                                      value, pe);                       \
    }                                                                                   \
    OSOE_API void pshmem_ctx_##NAME##_atomic_inc(shmem_ctx_t ctx, T *dest, int pe)       \
    {                                                                                   \
        amo_rmw("shmem_" #NAME "_atomic_inc", ctx, dest, (unsigned)sizeof(T),           \
                OSOE_RMW_ADD, 1, 0, 0, pe);                                             \
    }                                                                                   \
    OSOE_API void pshmem_##NAME##_atomic_inc(T *dest, int pe)                            \
    {                                                                                   \
        pshmem_ctx_##NAME##_atomic_inc(SHMEM_CTX_DEFAULT, dest, pe);                     \
    }                                                                                   \
    OSOE_API T pshmem_ctx_##NAME##_atomic_fetch_inc(shmem_ctx_t ctx, T *dest, int pe)    \
    {                                                                                   \
        return OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_fetch_inc", ctx, dest,   \
                                       (unsigned)sizeof(T), OSOE_RMW_ADD, 1, 0,         \
                                       OSOE_TRANSPORT_FETCH, pe));                             \
    }                                                                                   \
    OSOE_API T pshmem_##NAME##_atomic_fetch_inc(T *dest, int pe)                         \
    {                                                                                   \
        return pshmem_ctx_##NAME##_atomic_fetch_inc(SHMEM_CTX_DEFAULT, dest, pe);        \
    }                                                                                   \
    OSOE_API void pshmem_ctx_##NAME##_atomic_add(shmem_ctx_t ctx, T *dest, T value,      \
                                                int pe)                                 \
    {                                                                                   \
        amo_rmw("shmem_" #NAME "_atomic_add", ctx, dest, (unsigned)sizeof(T),           \
                OSOE_RMW_ADD, OSOE_AMO_IN(value), 0, 0, pe);                            \
    }                                                                                   \
    OSOE_API void pshmem_##NAME##_atomic_add(T *dest, T value, int pe)                   \
    {                                                                                   \
        pshmem_ctx_##NAME##_atomic_add(SHMEM_CTX_DEFAULT, dest, value, pe);              \
    }                                                                                   \
    OSOE_API T pshmem_ctx_##NAME##_atomic_fetch_add(shmem_ctx_t ctx, T *dest, T value,   \
                                                   int pe)                              \
    {                                                                                   \
        return OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_fetch_add", ctx, dest,   \
                                       (unsigned)sizeof(T), OSOE_RMW_ADD,               \
                                       OSOE_AMO_IN(value), 0, OSOE_TRANSPORT_FETCH, pe));      \
    }                                                                                   \
    OSOE_API T pshmem_##NAME##_atomic_fetch_add(T *dest, T value, int pe)                \
    {                                                                                   \
        return pshmem_ctx_##NAME##_atomic_fetch_add(SHMEM_CTX_DEFAULT, dest, value, pe); \
    }

/* ---- Table 7: the extended AMO types, integers ------------------------ */

#define OSOE_AMO_EXTENDED(T, NAME)                                                      \
    OSOE_API T pshmem_ctx_##NAME##_atomic_fetch(shmem_ctx_t ctx, const T *source, int pe) \
    {                                                                                    \
        return OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_fetch", ctx, source,      \
                                       (unsigned)sizeof(T), OSOE_RMW_READ, 0, 0,         \
                                       OSOE_TRANSPORT_FETCH, pe));                              \
    }                                                                                    \
    OSOE_API T pshmem_##NAME##_atomic_fetch(const T *source, int pe)                      \
    {                                                                                    \
        return pshmem_ctx_##NAME##_atomic_fetch(SHMEM_CTX_DEFAULT, source, pe);           \
    }                                                                                    \
    OSOE_API void pshmem_ctx_##NAME##_atomic_set(shmem_ctx_t ctx, T *dest, T value,       \
                                                int pe)                                  \
    {                                                                                    \
        amo_rmw("shmem_" #NAME "_atomic_set", ctx, dest, (unsigned)sizeof(T),            \
                OSOE_RMW_WRITE, OSOE_AMO_IN(value), 0, 0, pe);                           \
    }                                                                                    \
    OSOE_API void pshmem_##NAME##_atomic_set(T *dest, T value, int pe)                    \
    {                                                                                    \
        pshmem_ctx_##NAME##_atomic_set(SHMEM_CTX_DEFAULT, dest, value, pe);               \
    }                                                                                    \
    OSOE_API T pshmem_ctx_##NAME##_atomic_swap(shmem_ctx_t ctx, T *dest, T value, int pe) \
    {                                                                                    \
        return OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_swap", ctx, dest,         \
                                       (unsigned)sizeof(T), OSOE_RMW_WRITE,              \
                                       OSOE_AMO_IN(value), 0, OSOE_TRANSPORT_FETCH, pe));       \
    }                                                                                    \
    OSOE_API T pshmem_##NAME##_atomic_swap(T *dest, T value, int pe)                      \
    {                                                                                    \
        return pshmem_ctx_##NAME##_atomic_swap(SHMEM_CTX_DEFAULT, dest, value, pe);       \
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
    OSOE_API T pshmem_ctx_##NAME##_atomic_fetch(shmem_ctx_t ctx, const T *source, int pe)  \
    {                                                                                    \
        return NAME##_value(amo_rmw("shmem_" #NAME "_atomic_fetch", ctx, source,         \
                                    (unsigned)sizeof(T), OSOE_RMW_READ, 0, 0,            \
                                    OSOE_TRANSPORT_FETCH, pe));                                 \
    }                                                                                    \
    OSOE_API T pshmem_##NAME##_atomic_fetch(const T *source, int pe)                       \
    {                                                                                    \
        return pshmem_ctx_##NAME##_atomic_fetch(SHMEM_CTX_DEFAULT, source, pe);           \
    }                                                                                    \
    OSOE_API void pshmem_ctx_##NAME##_atomic_set(shmem_ctx_t ctx, T *dest, T value,        \
                                                int pe)                                  \
    {                                                                                    \
        amo_rmw("shmem_" #NAME "_atomic_set", ctx, dest, (unsigned)sizeof(T),            \
                OSOE_RMW_WRITE, NAME##_bits(value), 0, 0, pe);                           \
    }                                                                                    \
    OSOE_API void pshmem_##NAME##_atomic_set(T *dest, T value, int pe)                     \
    {                                                                                    \
        pshmem_ctx_##NAME##_atomic_set(SHMEM_CTX_DEFAULT, dest, value, pe);               \
    }                                                                                    \
    OSOE_API T pshmem_ctx_##NAME##_atomic_swap(shmem_ctx_t ctx, T *dest, T value, int pe)  \
    {                                                                                    \
        return NAME##_value(amo_rmw("shmem_" #NAME "_atomic_swap", ctx, dest,            \
                                    (unsigned)sizeof(T), OSOE_RMW_WRITE,                 \
                                    NAME##_bits(value), 0, OSOE_TRANSPORT_FETCH, pe));          \
    }                                                                                    \
    OSOE_API T pshmem_##NAME##_atomic_swap(T *dest, T value, int pe)                       \
    {                                                                                    \
        return pshmem_ctx_##NAME##_atomic_swap(SHMEM_CTX_DEFAULT, dest, value, pe);       \
    }

/* ---- Table 8: the bitwise AMO types ----------------------------------- */

#define OSOE_AMO_BITWISE_OP(T, NAME, OP, RMW)                                            \
    OSOE_API void pshmem_ctx_##NAME##_atomic_##OP(shmem_ctx_t ctx, T *dest, T value,       \
                                                 int pe)                                 \
    {                                                                                    \
        amo_rmw("shmem_" #NAME "_atomic_" #OP, ctx, dest, (unsigned)sizeof(T), RMW,      \
                OSOE_AMO_IN(value), 0, 0, pe);                                           \
    }                                                                                    \
    OSOE_API void pshmem_##NAME##_atomic_##OP(T *dest, T value, int pe)                    \
    {                                                                                    \
        pshmem_ctx_##NAME##_atomic_##OP(SHMEM_CTX_DEFAULT, dest, value, pe);              \
    }                                                                                    \
    OSOE_API T pshmem_ctx_##NAME##_atomic_fetch_##OP(shmem_ctx_t ctx, T *dest, T value,    \
                                                    int pe)                              \
    {                                                                                    \
        return OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_fetch_" #OP, ctx, dest,   \
                                       (unsigned)sizeof(T), RMW, OSOE_AMO_IN(value), 0,  \
                                       OSOE_TRANSPORT_FETCH, pe));                              \
    }                                                                                    \
    OSOE_API T pshmem_##NAME##_atomic_fetch_##OP(T *dest, T value, int pe)                 \
    {                                                                                    \
        return pshmem_ctx_##NAME##_atomic_fetch_##OP(SHMEM_CTX_DEFAULT, dest, value, pe); \
    }

#define OSOE_AMO_BITWISE(T, NAME)                                                        \
    OSOE_AMO_BITWISE_OP(T, NAME, and, OSOE_RMW_AND)                                      \
    OSOE_AMO_BITWISE_OP(T, NAME, or, OSOE_RMW_OR)                                        \
    OSOE_AMO_BITWISE_OP(T, NAME, xor, OSOE_RMW_XOR)

/*
 * ---- The nonblocking fetching AMOs (§9.7.2) -----------------------------
 *
 * The spec requires a _nbi routine to return after initiating the operation,
 * and promises only that *fetch holds the answer "at the completion of
 * [the next] shmem_quiet" - a deadline, not a floor. osoe takes the earliest
 * conforming choice: each definition below issues the very same blocking
 * fetching rmw as its blocking counterpart above (amo_rmw(..., dest or
 * source, ..., OSOE_TRANSPORT_FETCH, pe)) and stores the result to *fetch
 * before returning, so the value is already there at the call site rather
 * than only after the caller's next quiet.
 *
 * The upgrade the design notes leave open (§4.6, as built) is the transport's
 * rmw taking OSOE_TRANSPORT_NONBLOCKING instead, with a result destination
 * copied in at the PE's next wait - the shape osoe's nonblocking gets already
 * have. Not built here: every routine below is the immediate form only.
 *
 * Each amo_rmw call below is given the _nbi routine's own name
 * ("shmem_long_atomic_fetch_add_nbi", not "...fetch_add"), so a diagnostic a
 * caller sees names the call that was actually made.
 */

/* ---- Table 6's nonblocking fetching AMOs -------------------------------- */

#define OSOE_AMO_STANDARD_NBI(T, NAME)                                                \
    OSOE_API void pshmem_ctx_##NAME##_atomic_compare_swap_nbi(shmem_ctx_t ctx,        \
                                                              T *fetch, T *dest,       \
                                                              T cond, T value, int pe) \
    {                                                                                 \
        *fetch = OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_compare_swap_nbi",   \
                                         ctx, dest, (unsigned)sizeof(T),              \
                                         OSOE_RMW_CSWAP, OSOE_AMO_IN(value),          \
                                         OSOE_AMO_IN(cond), OSOE_TRANSPORT_FETCH, pe)); \
    }                                                                                 \
    OSOE_API void pshmem_##NAME##_atomic_compare_swap_nbi(T *fetch, T *dest, T cond,   \
                                                          T value, int pe)             \
    {                                                                                 \
        pshmem_ctx_##NAME##_atomic_compare_swap_nbi(SHMEM_CTX_DEFAULT, fetch, dest,   \
                                                     cond, value, pe);                 \
    }                                                                                 \
    OSOE_API void pshmem_ctx_##NAME##_atomic_fetch_inc_nbi(shmem_ctx_t ctx, T *fetch,  \
                                                           T *dest, int pe)            \
    {                                                                                 \
        *fetch = OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_fetch_inc_nbi", ctx, \
                                         dest, (unsigned)sizeof(T), OSOE_RMW_ADD, 1,  \
                                         0, OSOE_TRANSPORT_FETCH, pe));               \
    }                                                                                 \
    OSOE_API void pshmem_##NAME##_atomic_fetch_inc_nbi(T *fetch, T *dest, int pe)      \
    {                                                                                 \
        pshmem_ctx_##NAME##_atomic_fetch_inc_nbi(SHMEM_CTX_DEFAULT, fetch, dest, pe);  \
    }                                                                                 \
    OSOE_API void pshmem_ctx_##NAME##_atomic_fetch_add_nbi(shmem_ctx_t ctx, T *fetch,  \
                                                           T *dest, T value, int pe)   \
    {                                                                                 \
        *fetch = OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_fetch_add_nbi", ctx, \
                                         dest, (unsigned)sizeof(T), OSOE_RMW_ADD,     \
                                         OSOE_AMO_IN(value), 0, OSOE_TRANSPORT_FETCH, \
                                         pe));                                        \
    }                                                                                 \
    OSOE_API void pshmem_##NAME##_atomic_fetch_add_nbi(T *fetch, T *dest, T value,     \
                                                       int pe)                         \
    {                                                                                 \
        pshmem_ctx_##NAME##_atomic_fetch_add_nbi(SHMEM_CTX_DEFAULT, fetch, dest,      \
                                                 value, pe);                           \
    }

/* ---- Table 7's nonblocking fetching AMOs, integers ---------------------- */

#define OSOE_AMO_EXTENDED_NBI(T, NAME)                                                 \
    OSOE_API void pshmem_ctx_##NAME##_atomic_fetch_nbi(shmem_ctx_t ctx, T *fetch,      \
                                                       const T *source, int pe)         \
    {                                                                                  \
        *fetch = OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_fetch_nbi", ctx,      \
                                         source, (unsigned)sizeof(T), OSOE_RMW_READ,   \
                                         0, 0, OSOE_TRANSPORT_FETCH, pe));             \
    }                                                                                  \
    OSOE_API void pshmem_##NAME##_atomic_fetch_nbi(T *fetch, const T *source, int pe)   \
    {                                                                                  \
        pshmem_ctx_##NAME##_atomic_fetch_nbi(SHMEM_CTX_DEFAULT, fetch, source, pe);     \
    }                                                                                  \
    OSOE_API void pshmem_ctx_##NAME##_atomic_swap_nbi(shmem_ctx_t ctx, T *fetch,       \
                                                      T *dest, T value, int pe)         \
    {                                                                                  \
        *fetch = OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_swap_nbi", ctx, dest, \
                                         (unsigned)sizeof(T), OSOE_RMW_WRITE,          \
                                         OSOE_AMO_IN(value), 0, OSOE_TRANSPORT_FETCH,  \
                                         pe));                                         \
    }                                                                                  \
    OSOE_API void pshmem_##NAME##_atomic_swap_nbi(T *fetch, T *dest, T value, int pe)   \
    {                                                                                  \
        pshmem_ctx_##NAME##_atomic_swap_nbi(SHMEM_CTX_DEFAULT, fetch, dest, value, pe); \
    }

/* ---- Table 7's nonblocking fetching AMOs, float and double ------------- */

/* Reuses NAME##_bits/NAME##_value, defined just above by this NAME's
 * OSOE_AMO_FLOAT(T, NAME, U) - so this must be invoked after that, for the
 * same NAME, which the invocations below do. */
#define OSOE_AMO_FLOAT_NBI(T, NAME)                                                    \
    OSOE_API void pshmem_ctx_##NAME##_atomic_fetch_nbi(shmem_ctx_t ctx, T *fetch,      \
                                                       const T *source, int pe)         \
    {                                                                                  \
        *fetch = NAME##_value(amo_rmw("shmem_" #NAME "_atomic_fetch_nbi", ctx, source, \
                                      (unsigned)sizeof(T), OSOE_RMW_READ, 0, 0,        \
                                      OSOE_TRANSPORT_FETCH, pe));                      \
    }                                                                                  \
    OSOE_API void pshmem_##NAME##_atomic_fetch_nbi(T *fetch, const T *source, int pe)   \
    {                                                                                  \
        pshmem_ctx_##NAME##_atomic_fetch_nbi(SHMEM_CTX_DEFAULT, fetch, source, pe);     \
    }                                                                                  \
    OSOE_API void pshmem_ctx_##NAME##_atomic_swap_nbi(shmem_ctx_t ctx, T *fetch,       \
                                                      T *dest, T value, int pe)         \
    {                                                                                  \
        *fetch = NAME##_value(amo_rmw("shmem_" #NAME "_atomic_swap_nbi", ctx, dest,    \
                                      (unsigned)sizeof(T), OSOE_RMW_WRITE,             \
                                      NAME##_bits(value), 0, OSOE_TRANSPORT_FETCH,     \
                                      pe));                                            \
    }                                                                                  \
    OSOE_API void pshmem_##NAME##_atomic_swap_nbi(T *fetch, T *dest, T value, int pe)   \
    {                                                                                  \
        pshmem_ctx_##NAME##_atomic_swap_nbi(SHMEM_CTX_DEFAULT, fetch, dest, value, pe); \
    }

/* ---- Table 8's nonblocking fetching AMOs -------------------------------- */

#define OSOE_AMO_BITWISE_NBI_OP(T, NAME, OP, RMW)                                      \
    OSOE_API void pshmem_ctx_##NAME##_atomic_fetch_##OP##_nbi(shmem_ctx_t ctx,         \
                                                              T *fetch, T *dest,        \
                                                              T value, int pe)          \
    {                                                                                  \
        *fetch = OSOE_AMO_OUT(T, amo_rmw("shmem_" #NAME "_atomic_fetch_" #OP "_nbi",   \
                                         ctx, dest, (unsigned)sizeof(T), RMW,          \
                                         OSOE_AMO_IN(value), 0, OSOE_TRANSPORT_FETCH,  \
                                         pe));                                         \
    }                                                                                  \
    OSOE_API void pshmem_##NAME##_atomic_fetch_##OP##_nbi(T *fetch, T *dest, T value,   \
                                                          int pe)                       \
    {                                                                                  \
        pshmem_ctx_##NAME##_atomic_fetch_##OP##_nbi(SHMEM_CTX_DEFAULT, fetch, dest,    \
                                                     value, pe);                        \
    }

#define OSOE_AMO_BITWISE_NBI(T, NAME)                                                  \
    OSOE_AMO_BITWISE_NBI_OP(T, NAME, and, OSOE_RMW_AND)                                \
    OSOE_AMO_BITWISE_NBI_OP(T, NAME, or, OSOE_RMW_OR)                                  \
    OSOE_AMO_BITWISE_NBI_OP(T, NAME, xor, OSOE_RMW_XOR)

/*
 * The weak `shmem_` aliases (export.h), over the same type lists. They are a
 * separate pass rather than a line inside each body so that the definitions
 * above read as definitions; the lists are the same ones, so a type cannot get
 * a definition without an alias.
 */
#define OSOE_AMO_ALIAS_STANDARD(T, NAME)                                                \
    OSOE_ALIAS(ctx_##NAME##_atomic_compare_swap)                                        \
    OSOE_ALIAS(NAME##_atomic_compare_swap)                                              \
    OSOE_ALIAS(ctx_##NAME##_atomic_inc)                                                 \
    OSOE_ALIAS(NAME##_atomic_inc)                                                       \
    OSOE_ALIAS(ctx_##NAME##_atomic_fetch_inc)                                           \
    OSOE_ALIAS(NAME##_atomic_fetch_inc)                                                 \
    OSOE_ALIAS(ctx_##NAME##_atomic_add)                                                 \
    OSOE_ALIAS(NAME##_atomic_add)                                                       \
    OSOE_ALIAS(ctx_##NAME##_atomic_fetch_add)                                           \
    OSOE_ALIAS(NAME##_atomic_fetch_add)

#define OSOE_AMO_ALIAS_EXTENDED(T, NAME)                                                \
    OSOE_ALIAS(ctx_##NAME##_atomic_fetch)                                               \
    OSOE_ALIAS(NAME##_atomic_fetch)                                                     \
    OSOE_ALIAS(ctx_##NAME##_atomic_set)                                                 \
    OSOE_ALIAS(NAME##_atomic_set)                                                       \
    OSOE_ALIAS(ctx_##NAME##_atomic_swap)                                                \
    OSOE_ALIAS(NAME##_atomic_swap)

#define OSOE_AMO_ALIAS_BITWISE_OP(T, NAME, OP)                                          \
    OSOE_ALIAS(ctx_##NAME##_atomic_##OP)                                                \
    OSOE_ALIAS(NAME##_atomic_##OP)                                                      \
    OSOE_ALIAS(ctx_##NAME##_atomic_fetch_##OP)                                          \
    OSOE_ALIAS(NAME##_atomic_fetch_##OP)

#define OSOE_AMO_ALIAS_BITWISE(T, NAME)                                                 \
    OSOE_AMO_ALIAS_BITWISE_OP(T, NAME, and)                                             \
    OSOE_AMO_ALIAS_BITWISE_OP(T, NAME, or)                                              \
    OSOE_AMO_ALIAS_BITWISE_OP(T, NAME, xor)

/* The same, for the nonblocking fetching AMOs above. */
#define OSOE_AMO_ALIAS_STANDARD_NBI(T, NAME)                                           \
    OSOE_ALIAS(ctx_##NAME##_atomic_compare_swap_nbi)                                   \
    OSOE_ALIAS(NAME##_atomic_compare_swap_nbi)                                         \
    OSOE_ALIAS(ctx_##NAME##_atomic_fetch_inc_nbi)                                      \
    OSOE_ALIAS(NAME##_atomic_fetch_inc_nbi)                                            \
    OSOE_ALIAS(ctx_##NAME##_atomic_fetch_add_nbi)                                      \
    OSOE_ALIAS(NAME##_atomic_fetch_add_nbi)

#define OSOE_AMO_ALIAS_EXTENDED_NBI(T, NAME)                                           \
    OSOE_ALIAS(ctx_##NAME##_atomic_fetch_nbi)                                          \
    OSOE_ALIAS(NAME##_atomic_fetch_nbi)                                                \
    OSOE_ALIAS(ctx_##NAME##_atomic_swap_nbi)                                           \
    OSOE_ALIAS(NAME##_atomic_swap_nbi)

#define OSOE_AMO_ALIAS_BITWISE_NBI_OP(T, NAME, OP)                                     \
    OSOE_ALIAS(ctx_##NAME##_atomic_fetch_##OP##_nbi)                                   \
    OSOE_ALIAS(NAME##_atomic_fetch_##OP##_nbi)

#define OSOE_AMO_ALIAS_BITWISE_NBI(T, NAME)                                            \
    OSOE_AMO_ALIAS_BITWISE_NBI_OP(T, NAME, and)                                        \
    OSOE_AMO_ALIAS_BITWISE_NBI_OP(T, NAME, or)                                         \
    OSOE_AMO_ALIAS_BITWISE_NBI_OP(T, NAME, xor)

OSOE_AMO_STANDARD_TYPES(OSOE_AMO_STANDARD)
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_EXTENDED)
OSOE_AMO_FLOAT(float, float, uint32_t)
OSOE_AMO_FLOAT(double, double, uint64_t)
OSOE_AMO_BITWISE_TYPES(OSOE_AMO_BITWISE)

OSOE_AMO_STANDARD_TYPES(OSOE_AMO_STANDARD_NBI)
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_EXTENDED_NBI)
OSOE_AMO_FLOAT_NBI(float, float)
OSOE_AMO_FLOAT_NBI(double, double)
OSOE_AMO_BITWISE_TYPES(OSOE_AMO_BITWISE_NBI)

OSOE_AMO_STANDARD_TYPES(OSOE_AMO_ALIAS_STANDARD)
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_ALIAS_EXTENDED)
OSOE_AMO_FLOAT_TYPES(OSOE_AMO_ALIAS_EXTENDED)
OSOE_AMO_BITWISE_TYPES(OSOE_AMO_ALIAS_BITWISE)

OSOE_AMO_STANDARD_TYPES(OSOE_AMO_ALIAS_STANDARD_NBI)
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_ALIAS_EXTENDED_NBI)
OSOE_AMO_FLOAT_TYPES(OSOE_AMO_ALIAS_EXTENDED_NBI)
OSOE_AMO_BITWISE_TYPES(OSOE_AMO_ALIAS_BITWISE_NBI)
