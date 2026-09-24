/*
 * Typed and sized RMA (spec §9.6): every standard RMA type's put, get, p, g,
 * iput, iget and their nonblocking forms; the sized put8..128/get8..128 and
 * their strided and nonblocking forms; and putmem_nbi/getmem_nbi. The typed
 * put-with-signal family (§9.8.3-§9.8.4) is defined alongside putmem_signal in
 * signal.c, where the signal machinery already lives.
 *
 * Every routine here is a call to osoe_rma_put or osoe_rma_get (rma.h), the
 * bodies of shmem_putmem and shmem_getmem with the element width and the
 * routine's own name as parameters: a transfer of nelems elements is a byte
 * transfer of nelems * width bytes, and those two already carry the PE check,
 * the SHMEM_CTX_INVALID check (through osoe_ctx_of, §7.3), the symmetric-range
 * check and the zero-length no-op. Reusing them, rather than re-deriving that
 * logic once per type, is what keeps several hundred generated functions a
 * mechanical certainty instead of several hundred places the same bug could be
 * made differently; passing the name keeps a diagnostic about the routine the
 * program called.
 *
 * Complexity.
 *
 *   - put/get/p/g/put_nbi/get_nbi: O(1) - one transfer.
 *   - iput/iget: O(nelems) - a loop of nelems single-element transfers, each
 *     with its own checks and its own transport call, rather than one strided
 *     or batched transport primitive. Correct - every element lands at the
 *     offset the stride computes - but nelems network operations where a wider
 *     primitive would need one: across nodes, an iget of n elements is n round
 *     trips. §4.10 already names "the strided ... RMA loops" as
 *     OSOE_TRANSPORT_MORE's next driver; this slice does not reach for it:
 *     mechanical first, measured later (design notes §7.5).
 *
 * nbi. Spec §9.6.2: the nonblocking routines "return after initiating the
 * operation", and the operation is complete after a subsequent shmem_quiet;
 * completing sooner is permitted. The blocking forms already give put_nbi all
 * it promises and more: osoe_transport_put returns at local completion, with
 * remote completion left to quiet, on both backends (transport.h). A blocking
 * get completes before it returns, which get_nbi permits. So the nonblocking
 * forms call exactly the blocking code. The overlap a nonblocking get could
 * buy - several requests in flight before one wait - is the nonblocking flag
 * transport.h reserves on osoe_transport_get, not taken yet; when it is, only
 * the calls below change, never the declarations above them.
 */
#include <stddef.h>
#include <stdint.h>

#include <pshmem.h>
#include <shmem.h>

#include "export.h"
#include "rma.h"

/*
 * int8_t/uint8_t and int16_t/uint16_t, which Table 5 adds and no AMO table
 * has, are typedefs of signed/unsigned char and short on this implementation's
 * targets (LP64 Linux) - the identity shmem.h's C11 selection assumes so that
 * shmem_schar_put and shmem_int8_put name the same type without becoming the
 * same `_Generic` association. amo.c's static assertions already check the
 * four wider identities (int32_t/int64_t/uint32_t/uint64_t/size_t/ptrdiff_t)
 * Table 5 shares with Tables 6-8; these two are Table 5's own.
 */
_Static_assert(_Generic((int8_t)0, signed char: 1, default: 0),
               "int8_t must be signed char");
_Static_assert(_Generic((uint8_t)0, unsigned char: 1, default: 0),
               "uint8_t must be unsigned char");
_Static_assert(_Generic((int16_t)0, short: 1, default: 0), "int16_t must be short");
_Static_assert(_Generic((uint16_t)0, unsigned short: 1, default: 0),
               "uint16_t must be unsigned short");

/*
 * The strided forms (spec §9.6.1.3, §9.6.1.7), typed and sized alike: element
 * i goes from source + i*sst elements to dest + i*dst elements, the strides
 * "scaled by the element size". nelems 0 makes the loop a no-op, matching
 * putmem's own zero-length rule.
 */
static void strided_put(const char *routine, shmem_ctx_t ctx, void *dest, const void *source,
                        ptrdiff_t dst, ptrdiff_t sst, size_t nelems, size_t width, int pe)
{
    char *d = dest;
    const char *s = source;
    ptrdiff_t w = (ptrdiff_t)width;
    for (size_t i = 0; i < nelems; i++)
        osoe_rma_put(routine, ctx, d + (ptrdiff_t)i * dst * w, s + (ptrdiff_t)i * sst * w, 1,
                     width, pe);
}

static void strided_get(const char *routine, shmem_ctx_t ctx, void *dest, const void *source,
                        ptrdiff_t dst, ptrdiff_t sst, size_t nelems, size_t width, int pe)
{
    char *d = dest;
    const char *s = source;
    ptrdiff_t w = (ptrdiff_t)width;
    for (size_t i = 0; i < nelems; i++)
        osoe_rma_get(routine, ctx, d + (ptrdiff_t)i * dst * w, s + (ptrdiff_t)i * sst * w, 1,
                     width, pe);
}

/* ---- Typed put/get, block (spec §9.6.1.1, §9.6.1.5) -------------------- */

#define OSOE_RMA_PUT_GET(T, NAME)                                                        \
    OSOE_API void OSOE_FN(pshmem_, ctx_##NAME, _put)(shmem_ctx_t ctx, T *dest,           \
                                    const T *source, size_t nelems, int pe)               \
    {                                                                                     \
        osoe_rma_put("shmem_ctx_" #NAME "_put", ctx, dest, source, nelems, sizeof(T), pe); \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, NAME, _put)(T *dest, const T *source, size_t nelems,  \
                                              int pe)                                     \
    {                                                                                     \
        osoe_rma_put("shmem_" #NAME "_put", SHMEM_CTX_DEFAULT, dest, source, nelems,      \
                     sizeof(T), pe);                                                      \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, ctx_##NAME, _get)(shmem_ctx_t ctx, T *dest,           \
                                    const T *source, size_t nelems, int pe)               \
    {                                                                                     \
        osoe_rma_get("shmem_ctx_" #NAME "_get", ctx, dest, source, nelems, sizeof(T), pe); \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, NAME, _get)(T *dest, const T *source, size_t nelems,  \
                                              int pe)                                     \
    {                                                                                     \
        osoe_rma_get("shmem_" #NAME "_get", SHMEM_CTX_DEFAULT, dest, source, nelems,      \
                     sizeof(T), pe);                                                      \
    }

/* ---- Typed p/g, single element (spec §9.6.1.2, §9.6.1.6) --------------- */

#define OSOE_RMA_P_G(T, NAME)                                                             \
    OSOE_API void OSOE_FN(pshmem_, ctx_##NAME, _p)(shmem_ctx_t ctx, T *dest, T value,     \
                                                  int pe)                                  \
    {                                                                                     \
        osoe_rma_put("shmem_ctx_" #NAME "_p", ctx, dest, &value, 1, sizeof(T), pe);       \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, NAME, _p)(T *dest, T value, int pe)                    \
    {                                                                                     \
        osoe_rma_put("shmem_" #NAME "_p", SHMEM_CTX_DEFAULT, dest, &value, 1, sizeof(T),  \
                     pe);                                                                 \
    }                                                                                     \
    OSOE_API T OSOE_FN(pshmem_, ctx_##NAME, _g)(shmem_ctx_t ctx, const T *source, int pe)  \
    {                                                                                     \
        T value;                                                                          \
        osoe_rma_get("shmem_ctx_" #NAME "_g", ctx, &value, source, 1, sizeof(T), pe);     \
        return value;                                                                     \
    }                                                                                     \
    OSOE_API T OSOE_FN(pshmem_, NAME, _g)(const T *source, int pe)                        \
    {                                                                                     \
        T value;                                                                          \
        osoe_rma_get("shmem_" #NAME "_g", SHMEM_CTX_DEFAULT, &value, source, 1,           \
                     sizeof(T), pe);                                                      \
        return value;                                                                     \
    }

/* ---- Typed iput/iget, strided (spec §9.6.1.3, §9.6.1.7) ---------------- */

#define OSOE_RMA_IPUT_IGET(T, NAME)                                                       \
    OSOE_API void OSOE_FN(pshmem_, ctx_##NAME, _iput)(shmem_ctx_t ctx, T *dest,           \
                            const T *source, ptrdiff_t dst, ptrdiff_t sst,                \
                            size_t nelems, int pe)                                        \
    {                                                                                     \
        strided_put("shmem_ctx_" #NAME "_iput", ctx, dest, source, dst, sst, nelems,      \
                    sizeof(T), pe);                                                       \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, NAME, _iput)(T *dest, const T *source, ptrdiff_t dst,  \
                            ptrdiff_t sst, size_t nelems, int pe)                         \
    {                                                                                     \
        strided_put("shmem_" #NAME "_iput", SHMEM_CTX_DEFAULT, dest, source, dst, sst,    \
                    nelems, sizeof(T), pe);                                               \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, ctx_##NAME, _iget)(shmem_ctx_t ctx, T *dest,           \
                            const T *source, ptrdiff_t dst, ptrdiff_t sst,                \
                            size_t nelems, int pe)                                        \
    {                                                                                     \
        strided_get("shmem_ctx_" #NAME "_iget", ctx, dest, source, dst, sst, nelems,      \
                    sizeof(T), pe);                                                       \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, NAME, _iget)(T *dest, const T *source, ptrdiff_t dst,  \
                            ptrdiff_t sst, size_t nelems, int pe)                         \
    {                                                                                     \
        strided_get("shmem_" #NAME "_iget", SHMEM_CTX_DEFAULT, dest, source, dst, sst,    \
                    nelems, sizeof(T), pe);                                               \
    }

/* ---- Typed put_nbi/get_nbi (spec §9.6.2.1-§9.6.2.2) --------------------- */

#define OSOE_RMA_NBI(T, NAME)                                                             \
    OSOE_API void OSOE_FN(pshmem_, ctx_##NAME, _put_nbi)(shmem_ctx_t ctx, T *dest,        \
                            const T *source, size_t nelems, int pe)                       \
    {                                                                                     \
        osoe_rma_put("shmem_ctx_" #NAME "_put_nbi", ctx, dest, source, nelems, sizeof(T), \
                     pe);                                                                 \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, NAME, _put_nbi)(T *dest, const T *source,              \
                                                  size_t nelems, int pe)                  \
    {                                                                                     \
        osoe_rma_put("shmem_" #NAME "_put_nbi", SHMEM_CTX_DEFAULT, dest, source, nelems,  \
                     sizeof(T), pe);                                                      \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, ctx_##NAME, _get_nbi)(shmem_ctx_t ctx, T *dest,        \
                            const T *source, size_t nelems, int pe)                       \
    {                                                                                     \
        osoe_rma_get("shmem_ctx_" #NAME "_get_nbi", ctx, dest, source, nelems, sizeof(T), \
                     pe);                                                                 \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, NAME, _get_nbi)(T *dest, const T *source,              \
                                                  size_t nelems, int pe)                  \
    {                                                                                     \
        osoe_rma_get("shmem_" #NAME "_get_nbi", SHMEM_CTX_DEFAULT, dest, source, nelems,  \
                     sizeof(T), pe);                                                      \
    }

OSOE_RMA_STANDARD_TYPES(OSOE_RMA_PUT_GET)
OSOE_RMA_STANDARD_TYPES(OSOE_RMA_P_G)
OSOE_RMA_STANDARD_TYPES(OSOE_RMA_IPUT_IGET)
OSOE_RMA_STANDARD_TYPES(OSOE_RMA_NBI)

/* ---- Sized put/get, strided and nonblocking (spec §9.6.1, §9.6.2) ------
 *
 * SIZE is in bits (8, 16, 32, 64, 128), so an element is SIZE / 8 bytes. */

#define OSOE_RMA_SIZED(SIZE)                                                              \
    OSOE_API void pshmem_ctx_put##SIZE(shmem_ctx_t ctx, void *dest, const void *source,   \
                                       size_t nelems, int pe)                             \
    {                                                                                     \
        osoe_rma_put("shmem_ctx_put" #SIZE, ctx, dest, source, nelems, SIZE / 8, pe);     \
    }                                                                                     \
    OSOE_API void pshmem_put##SIZE(void *dest, const void *source, size_t nelems, int pe) \
    {                                                                                     \
        osoe_rma_put("shmem_put" #SIZE, SHMEM_CTX_DEFAULT, dest, source, nelems,          \
                     SIZE / 8, pe);                                                       \
    }                                                                                     \
    OSOE_API void pshmem_ctx_get##SIZE(shmem_ctx_t ctx, void *dest, const void *source,   \
                                       size_t nelems, int pe)                             \
    {                                                                                     \
        osoe_rma_get("shmem_ctx_get" #SIZE, ctx, dest, source, nelems, SIZE / 8, pe);     \
    }                                                                                     \
    OSOE_API void pshmem_get##SIZE(void *dest, const void *source, size_t nelems, int pe) \
    {                                                                                     \
        osoe_rma_get("shmem_get" #SIZE, SHMEM_CTX_DEFAULT, dest, source, nelems,          \
                     SIZE / 8, pe);                                                       \
    }                                                                                     \
    OSOE_API void pshmem_ctx_iput##SIZE(shmem_ctx_t ctx, void *dest, const void *source,  \
                                        ptrdiff_t dst, ptrdiff_t sst, size_t nelems,      \
                                        int pe)                                           \
    {                                                                                     \
        strided_put("shmem_ctx_iput" #SIZE, ctx, dest, source, dst, sst, nelems,          \
                    SIZE / 8, pe);                                                        \
    }                                                                                     \
    OSOE_API void pshmem_iput##SIZE(void *dest, const void *source, ptrdiff_t dst,        \
                                    ptrdiff_t sst, size_t nelems, int pe)                 \
    {                                                                                     \
        strided_put("shmem_iput" #SIZE, SHMEM_CTX_DEFAULT, dest, source, dst, sst,        \
                    nelems, SIZE / 8, pe);                                                \
    }                                                                                     \
    OSOE_API void pshmem_ctx_iget##SIZE(shmem_ctx_t ctx, void *dest, const void *source,  \
                                        ptrdiff_t dst, ptrdiff_t sst, size_t nelems,      \
                                        int pe)                                           \
    {                                                                                     \
        strided_get("shmem_ctx_iget" #SIZE, ctx, dest, source, dst, sst, nelems,          \
                    SIZE / 8, pe);                                                        \
    }                                                                                     \
    OSOE_API void pshmem_iget##SIZE(void *dest, const void *source, ptrdiff_t dst,        \
                                    ptrdiff_t sst, size_t nelems, int pe)                 \
    {                                                                                     \
        strided_get("shmem_iget" #SIZE, SHMEM_CTX_DEFAULT, dest, source, dst, sst,        \
                    nelems, SIZE / 8, pe);                                                \
    }                                                                                     \
    OSOE_API void pshmem_ctx_put##SIZE##_nbi(shmem_ctx_t ctx, void *dest,                 \
                                             const void *source, size_t nelems, int pe)   \
    {                                                                                     \
        osoe_rma_put("shmem_ctx_put" #SIZE "_nbi", ctx, dest, source, nelems, SIZE / 8,   \
                     pe);                                                                 \
    }                                                                                     \
    OSOE_API void pshmem_put##SIZE##_nbi(void *dest, const void *source, size_t nelems,   \
                                         int pe)                                          \
    {                                                                                     \
        osoe_rma_put("shmem_put" #SIZE "_nbi", SHMEM_CTX_DEFAULT, dest, source, nelems,   \
                     SIZE / 8, pe);                                                       \
    }                                                                                     \
    OSOE_API void pshmem_ctx_get##SIZE##_nbi(shmem_ctx_t ctx, void *dest,                 \
                                             const void *source, size_t nelems, int pe)   \
    {                                                                                     \
        osoe_rma_get("shmem_ctx_get" #SIZE "_nbi", ctx, dest, source, nelems, SIZE / 8,   \
                     pe);                                                                 \
    }                                                                                     \
    OSOE_API void pshmem_get##SIZE##_nbi(void *dest, const void *source, size_t nelems,   \
                                         int pe)                                          \
    {                                                                                     \
        osoe_rma_get("shmem_get" #SIZE "_nbi", SHMEM_CTX_DEFAULT, dest, source, nelems,   \
                     SIZE / 8, pe);                                                       \
    }

OSOE_RMA_SIZES(OSOE_RMA_SIZED)

/* ---- The byte forms' nonblocking spellings (spec §9.6.2.1-§9.6.2.2) ---- */

OSOE_API void pshmem_ctx_putmem_nbi(shmem_ctx_t ctx, void *dest, const void *source,
                                    size_t nelems, int pe)
{
    osoe_rma_put("shmem_ctx_putmem_nbi", ctx, dest, source, nelems, 1, pe);
}

OSOE_API void pshmem_putmem_nbi(void *dest, const void *source, size_t nelems, int pe)
{
    osoe_rma_put("shmem_putmem_nbi", SHMEM_CTX_DEFAULT, dest, source, nelems, 1, pe);
}

OSOE_API void pshmem_ctx_getmem_nbi(shmem_ctx_t ctx, void *dest, const void *source,
                                    size_t nelems, int pe)
{
    osoe_rma_get("shmem_ctx_getmem_nbi", ctx, dest, source, nelems, 1, pe);
}

OSOE_API void pshmem_getmem_nbi(void *dest, const void *source, size_t nelems, int pe)
{
    osoe_rma_get("shmem_getmem_nbi", SHMEM_CTX_DEFAULT, dest, source, nelems, 1, pe);
}

/*
 * The weak `shmem_` aliases (export.h), over the same lists: a separate pass,
 * as amo.c's, so the definitions above read as definitions.
 */
#define OSOE_RMA_ALIAS_TYPED(T, NAME)                                                     \
    OSOE_ALIAS(ctx_##NAME##_put) OSOE_ALIAS(NAME##_put)                                   \
    OSOE_ALIAS(ctx_##NAME##_get) OSOE_ALIAS(NAME##_get)                                   \
    OSOE_ALIAS(ctx_##NAME##_p) OSOE_ALIAS(NAME##_p)                                       \
    OSOE_ALIAS(ctx_##NAME##_g) OSOE_ALIAS(NAME##_g)                                       \
    OSOE_ALIAS(ctx_##NAME##_iput) OSOE_ALIAS(NAME##_iput)                                 \
    OSOE_ALIAS(ctx_##NAME##_iget) OSOE_ALIAS(NAME##_iget)                                 \
    OSOE_ALIAS(ctx_##NAME##_put_nbi) OSOE_ALIAS(NAME##_put_nbi)                           \
    OSOE_ALIAS(ctx_##NAME##_get_nbi) OSOE_ALIAS(NAME##_get_nbi)

#define OSOE_RMA_ALIAS_SIZED(SIZE)                                                        \
    OSOE_ALIAS(ctx_put##SIZE) OSOE_ALIAS(put##SIZE)                                       \
    OSOE_ALIAS(ctx_get##SIZE) OSOE_ALIAS(get##SIZE)                                       \
    OSOE_ALIAS(ctx_iput##SIZE) OSOE_ALIAS(iput##SIZE)                                     \
    OSOE_ALIAS(ctx_iget##SIZE) OSOE_ALIAS(iget##SIZE)                                     \
    OSOE_ALIAS(ctx_put##SIZE##_nbi) OSOE_ALIAS(put##SIZE##_nbi)                           \
    OSOE_ALIAS(ctx_get##SIZE##_nbi) OSOE_ALIAS(get##SIZE##_nbi)

OSOE_RMA_STANDARD_TYPES(OSOE_RMA_ALIAS_TYPED)
OSOE_RMA_SIZES(OSOE_RMA_ALIAS_SIZED)

OSOE_ALIAS(ctx_putmem_nbi)
OSOE_ALIAS(putmem_nbi)
OSOE_ALIAS(ctx_getmem_nbi)
OSOE_ALIAS(getmem_nbi)
