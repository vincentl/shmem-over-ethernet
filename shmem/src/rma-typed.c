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
 *     with its own checks and its own transport call. Correct - every element
 *     lands at the offset the stride computes - and batched where the
 *     transport can batch: an iput's elements but the last carry
 *     OSOE_TRANSPORT_MORE, so across nodes they cost one wake of the lead,
 *     not one each. No transport has a remote-strided operation to map a
 *     whole iput onto - RDMA's gather list is the local side's only - so this
 *     is also what an RDMA backend would do. An iget's elements are
 *     nonblocking gets, in flight together across nodes up to the landing
 *     zone's records, completed by one wait for the gets alone (design notes
 *     §7.5, §4.6).
 *   - ibput/ibget: O(nblocks) - the same loop, generalized from one element to
 *     one block of bsize elements: nblocks transport calls, each a single
 *     contiguous transfer, not nblocks * bsize. Batched the same way - every
 *     block but the last carries OSOE_TRANSPORT_MORE, and an ibget's blocks
 *     are nonblocking gets completed by one wait for the gets alone - so a
 *     block size of 1 costs exactly what iput/iget costs, which is the same
 *     loop calling osoe_rma_put/osoe_rma_get with bsize elements instead of
 *     one (spec's own Note: shmem_ibput with bsize 1 is shmem_iput). Both
 *     strides equal to bsize is one transfer of the whole: O(1).
 *
 * nbi. Spec §9.6.2: the nonblocking routines "return after initiating the
 * operation", and the operation is complete after a subsequent shmem_quiet;
 * completing sooner is permitted. A put already returns at local completion,
 * with remote completion left to quiet, on both backends (transport.h), which
 * is all put_nbi promises, so the nonblocking puts call the blocking code. The
 * nonblocking gets pass OSOE_TRANSPORT_NONBLOCKING: across nodes they return
 * once asked, and the next wait completes them (§4.6, as built); to a
 * node-mate a get is applied before it returns either way.
 */
#include <stddef.h>
#include <stdint.h>

#include <pshmem.h>
#include <shmem.h>

#include "export.h"
#include "fatal.h"
#include "rma.h"
#include "transport.h"

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
    /* Every element but the last says more follows (OSOE_TRANSPORT_MORE), so
     * a backend may submit the loop's puts together: on the AM path one wake
     * asks the lead to run, at the last, and an RDMA backend would post them
     * under one doorbell. The last carries no flag and submits whatever is
     * held. */
    for (size_t i = 0; i < nelems; i++)
        osoe_rma_put(routine, ctx, d + (ptrdiff_t)i * dst * w, s + (ptrdiff_t)i * sst * w, 1,
                     width, pe, i + 1 < nelems ? OSOE_TRANSPORT_MORE : 0);
}

static void strided_get(const char *routine, shmem_ctx_t ctx, void *dest, const void *source,
                        ptrdiff_t dst, ptrdiff_t sst, size_t nelems, size_t width, int pe)
{
    char *d = dest;
    const char *s = source;
    ptrdiff_t w = (ptrdiff_t)width;
    /* Every element's get is nonblocking and all but the last say more follows,
     * so across nodes they are in flight together, up to the landing zone's
     * records - not one round trip each - and the wait for them after is for
     * the gets alone: iget is a blocking routine, but promises nothing about
     * the writes before it (§4.6, as built). */
    if (nelems == 0)
        return;
    for (size_t i = 0; i < nelems; i++)
        osoe_rma_get(routine, ctx, d + (ptrdiff_t)i * dst * w, s + (ptrdiff_t)i * sst * w, 1,
                     width, pe,
                     OSOE_TRANSPORT_NONBLOCKING | (i + 1 < nelems ? OSOE_TRANSPORT_MORE : 0));
    osoe_rma_complete_gets(routine, ctx);
}

/*
 * The block-strided forms (spec §9.6.1.4, §9.6.1.8), typed and sized alike:
 * block b, for b from 0 to nblocks - 1, copies bsize elements from
 * source + b*sst elements to dest + b*dst elements, one contiguous transfer
 * per block - the same loop strided_put/strided_get run, generalized from one
 * element to a block of bsize. nblocks or bsize 0 transfers nothing, matching
 * the strided forms' own zero-length rule; calling with bsize 1 is iput/iget
 * (the spec's own Notes say so). Both strides are "scaled by the element
 * size" and the spec's Arguments require each at least bsize - undefined
 * below that, which the library ends by name for the price of a compare, as
 * it does for shmem_team_split_strided's stride of 0 (team.c) and
 * shmem_align's alignment (malloc.c).
 */
static void strided_block_put(const char *routine, shmem_ctx_t ctx, void *dest,
                              const void *source, ptrdiff_t dst, ptrdiff_t sst, size_t bsize,
                              size_t nblocks, size_t width, int pe)
{
    if (nblocks == 0 || bsize == 0)
        return;
    if (dst < 0 || sst < 0 || (size_t)dst < bsize || (size_t)sst < bsize)
        osoe_fatal("%s: dst %td, sst %td below bsize %zu; spec §9.6.1.4 requires each "
                   "stride at least bsize", routine, dst, sst, bsize);
    /* Both strides bsize: the blocks run on unbroken on either side, one
     * transfer of the whole. */
    if ((size_t)dst == bsize && (size_t)sst == bsize) {
        if (nblocks > SIZE_MAX / bsize)
            osoe_fatal("%s: %zu blocks of %zu elements are more than memory holds", routine,
                       nblocks, bsize);
        osoe_rma_put(routine, ctx, dest, source, nblocks * bsize, width, pe, 0);
        return;
    }
    char *d = dest;
    const char *s = source;
    ptrdiff_t w = (ptrdiff_t)width;
    /* Every block but the last says more follows, exactly as strided_put's
     * elements do, so a backend may submit the loop's puts together. */
    for (size_t b = 0; b < nblocks; b++)
        osoe_rma_put(routine, ctx, d + (ptrdiff_t)b * dst * w, s + (ptrdiff_t)b * sst * w, bsize,
                     width, pe, b + 1 < nblocks ? OSOE_TRANSPORT_MORE : 0);
}

static void strided_block_get(const char *routine, shmem_ctx_t ctx, void *dest,
                              const void *source, ptrdiff_t dst, ptrdiff_t sst, size_t bsize,
                              size_t nblocks, size_t width, int pe)
{
    if (nblocks == 0 || bsize == 0)
        return;
    if (dst < 0 || sst < 0 || (size_t)dst < bsize || (size_t)sst < bsize)
        osoe_fatal("%s: dst %td, sst %td below bsize %zu; spec §9.6.1.8 requires each "
                   "stride at least bsize", routine, dst, sst, bsize);
    /* Both strides bsize: one transfer of the whole, as for ibput. */
    if ((size_t)dst == bsize && (size_t)sst == bsize) {
        if (nblocks > SIZE_MAX / bsize)
            osoe_fatal("%s: %zu blocks of %zu elements are more than memory holds", routine,
                       nblocks, bsize);
        osoe_rma_get(routine, ctx, dest, source, nblocks * bsize, width, pe, 0);
        return;
    }
    char *d = dest;
    const char *s = source;
    ptrdiff_t w = (ptrdiff_t)width;
    /* Every block's get is nonblocking and all but the last say more follows,
     * exactly as strided_get's elements, so the wait after is for the gets
     * alone. */
    for (size_t b = 0; b < nblocks; b++)
        osoe_rma_get(routine, ctx, d + (ptrdiff_t)b * dst * w, s + (ptrdiff_t)b * sst * w, bsize,
                     width, pe,
                     OSOE_TRANSPORT_NONBLOCKING | (b + 1 < nblocks ? OSOE_TRANSPORT_MORE : 0));
    osoe_rma_complete_gets(routine, ctx);
}

/* ---- Typed put/get, block (spec §9.6.1.1, §9.6.1.5) -------------------- */

#define OSOE_RMA_PUT_GET(T, NAME)                                                         \
    OSOE_API void OSOE_FN(pshmem_, ctx_##NAME, _put)(shmem_ctx_t ctx, T *dest,            \
                                    const T *source, size_t nelems, int pe)               \
    {                                                                                     \
        osoe_rma_put("shmem_ctx_" #NAME "_put", ctx, dest, source, nelems, sizeof(T), pe, 0); \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, NAME, _put)(T *dest, const T *source, size_t nelems,   \
                                              int pe)                                     \
    {                                                                                     \
        osoe_rma_put("shmem_" #NAME "_put", SHMEM_CTX_DEFAULT, dest, source, nelems,      \
                     sizeof(T), pe, 0);                                                   \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, ctx_##NAME, _get)(shmem_ctx_t ctx, T *dest,            \
                                    const T *source, size_t nelems, int pe)               \
    {                                                                                     \
        osoe_rma_get("shmem_ctx_" #NAME "_get", ctx, dest, source, nelems, sizeof(T), pe, 0); \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, NAME, _get)(T *dest, const T *source, size_t nelems,   \
                                              int pe)                                     \
    {                                                                                     \
        osoe_rma_get("shmem_" #NAME "_get", SHMEM_CTX_DEFAULT, dest, source, nelems,      \
                     sizeof(T), pe, 0);                                                   \
    }

/* ---- Typed p/g, single element (spec §9.6.1.2, §9.6.1.6) --------------- */

#define OSOE_RMA_P_G(T, NAME)                                                             \
    OSOE_API void OSOE_FN(pshmem_, ctx_##NAME, _p)(shmem_ctx_t ctx, T *dest, T value,     \
                                                  int pe)                                 \
    {                                                                                     \
        osoe_rma_put("shmem_ctx_" #NAME "_p", ctx, dest, &value, 1, sizeof(T), pe, 0);    \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, NAME, _p)(T *dest, T value, int pe)                    \
    {                                                                                     \
        osoe_rma_put("shmem_" #NAME "_p", SHMEM_CTX_DEFAULT, dest, &value, 1, sizeof(T),  \
                     pe, 0);                                                              \
    }                                                                                     \
    OSOE_API T OSOE_FN(pshmem_, ctx_##NAME, _g)(shmem_ctx_t ctx, const T *source, int pe) \
    {                                                                                     \
        T value;                                                                          \
        osoe_rma_get("shmem_ctx_" #NAME "_g", ctx, &value, source, 1, sizeof(T), pe, 0);  \
        return value;                                                                     \
    }                                                                                     \
    OSOE_API T OSOE_FN(pshmem_, NAME, _g)(const T *source, int pe)                        \
    {                                                                                     \
        T value;                                                                          \
        osoe_rma_get("shmem_" #NAME "_g", SHMEM_CTX_DEFAULT, &value, source, 1,           \
                     sizeof(T), pe, 0);                                                   \
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

/* ---- Typed ibput/ibget, block-strided (spec §9.6.1.4, §9.6.1.8) -------- */

#define OSOE_RMA_IBPUT_IBGET(T, NAME)                                                     \
    OSOE_API void OSOE_FN(pshmem_, ctx_##NAME, _ibput)(shmem_ctx_t ctx, T *dest,          \
                            const T *source, ptrdiff_t dst, ptrdiff_t sst,                \
                            size_t bsize, size_t nblocks, int pe)                         \
    {                                                                                     \
        strided_block_put("shmem_ctx_" #NAME "_ibput", ctx, dest, source, dst, sst,      \
                          bsize, nblocks, sizeof(T), pe);                                 \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, NAME, _ibput)(T *dest, const T *source, ptrdiff_t dst, \
                            ptrdiff_t sst, size_t bsize, size_t nblocks, int pe)           \
    {                                                                                     \
        strided_block_put("shmem_" #NAME "_ibput", SHMEM_CTX_DEFAULT, dest, source, dst, \
                          sst, bsize, nblocks, sizeof(T), pe);                            \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, ctx_##NAME, _ibget)(shmem_ctx_t ctx, T *dest,          \
                            const T *source, ptrdiff_t dst, ptrdiff_t sst,                \
                            size_t bsize, size_t nblocks, int pe)                         \
    {                                                                                     \
        strided_block_get("shmem_ctx_" #NAME "_ibget", ctx, dest, source, dst, sst,      \
                          bsize, nblocks, sizeof(T), pe);                                 \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, NAME, _ibget)(T *dest, const T *source, ptrdiff_t dst, \
                            ptrdiff_t sst, size_t bsize, size_t nblocks, int pe)           \
    {                                                                                     \
        strided_block_get("shmem_" #NAME "_ibget", SHMEM_CTX_DEFAULT, dest, source, dst, \
                          sst, bsize, nblocks, sizeof(T), pe);                            \
    }

/* ---- Typed put_nbi/get_nbi (spec §9.6.2.1-§9.6.2.2) --------------------- */

#define OSOE_RMA_NBI(T, NAME)                                                             \
    OSOE_API void OSOE_FN(pshmem_, ctx_##NAME, _put_nbi)(shmem_ctx_t ctx, T *dest,        \
                            const T *source, size_t nelems, int pe)                       \
    {                                                                                     \
        osoe_rma_put("shmem_ctx_" #NAME "_put_nbi", ctx, dest, source, nelems, sizeof(T), \
                     pe, 0);                                                              \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, NAME, _put_nbi)(T *dest, const T *source,              \
                                                  size_t nelems, int pe)                  \
    {                                                                                     \
        osoe_rma_put("shmem_" #NAME "_put_nbi", SHMEM_CTX_DEFAULT, dest, source, nelems,  \
                     sizeof(T), pe, 0);                                                   \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, ctx_##NAME, _get_nbi)(shmem_ctx_t ctx, T *dest,        \
                            const T *source, size_t nelems, int pe)                       \
    {                                                                                     \
        osoe_rma_get("shmem_ctx_" #NAME "_get_nbi", ctx, dest, source, nelems, sizeof(T), \
                     pe, OSOE_TRANSPORT_NONBLOCKING);                                     \
    }                                                                                     \
    OSOE_API void OSOE_FN(pshmem_, NAME, _get_nbi)(T *dest, const T *source,              \
                                                  size_t nelems, int pe)                  \
    {                                                                                     \
        osoe_rma_get("shmem_" #NAME "_get_nbi", SHMEM_CTX_DEFAULT, dest, source, nelems,  \
                     sizeof(T), pe, OSOE_TRANSPORT_NONBLOCKING);                          \
    }

OSOE_RMA_STANDARD_TYPES(OSOE_RMA_PUT_GET)
OSOE_RMA_STANDARD_TYPES(OSOE_RMA_P_G)
OSOE_RMA_STANDARD_TYPES(OSOE_RMA_IPUT_IGET)
OSOE_RMA_STANDARD_TYPES(OSOE_RMA_IBPUT_IBGET)
OSOE_RMA_STANDARD_TYPES(OSOE_RMA_NBI)

/* ---- Sized put/get, strided and nonblocking (spec §9.6.1, §9.6.2) ------
 *
 * SIZE is in bits (8, 16, 32, 64, 128), so an element is SIZE / 8 bytes. */

#define OSOE_RMA_SIZED(SIZE)                                                              \
    OSOE_API void pshmem_ctx_put##SIZE(shmem_ctx_t ctx, void *dest, const void *source,   \
                                       size_t nelems, int pe)                             \
    {                                                                                     \
        osoe_rma_put("shmem_ctx_put" #SIZE, ctx, dest, source, nelems, SIZE / 8, pe, 0);  \
    }                                                                                     \
    OSOE_API void pshmem_put##SIZE(void *dest, const void *source, size_t nelems, int pe) \
    {                                                                                     \
        osoe_rma_put("shmem_put" #SIZE, SHMEM_CTX_DEFAULT, dest, source, nelems,          \
                     SIZE / 8, pe, 0);                                                    \
    }                                                                                     \
    OSOE_API void pshmem_ctx_get##SIZE(shmem_ctx_t ctx, void *dest, const void *source,   \
                                       size_t nelems, int pe)                             \
    {                                                                                     \
        osoe_rma_get("shmem_ctx_get" #SIZE, ctx, dest, source, nelems, SIZE / 8, pe, 0);  \
    }                                                                                     \
    OSOE_API void pshmem_get##SIZE(void *dest, const void *source, size_t nelems, int pe) \
    {                                                                                     \
        osoe_rma_get("shmem_get" #SIZE, SHMEM_CTX_DEFAULT, dest, source, nelems,          \
                     SIZE / 8, pe, 0);                                                    \
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
    OSOE_API void pshmem_ctx_ibput##SIZE(shmem_ctx_t ctx, void *dest, const void *source, \
                                         ptrdiff_t dst, ptrdiff_t sst, size_t bsize,      \
                                         size_t nblocks, int pe)                          \
    {                                                                                     \
        strided_block_put("shmem_ctx_ibput" #SIZE, ctx, dest, source, dst, sst, bsize,   \
                          nblocks, SIZE / 8, pe);                                         \
    }                                                                                     \
    OSOE_API void pshmem_ibput##SIZE(void *dest, const void *source, ptrdiff_t dst,       \
                                     ptrdiff_t sst, size_t bsize, size_t nblocks, int pe) \
    {                                                                                     \
        strided_block_put("shmem_ibput" #SIZE, SHMEM_CTX_DEFAULT, dest, source, dst, sst, \
                          bsize, nblocks, SIZE / 8, pe);                                  \
    }                                                                                     \
    OSOE_API void pshmem_ctx_ibget##SIZE(shmem_ctx_t ctx, void *dest, const void *source, \
                                         ptrdiff_t dst, ptrdiff_t sst, size_t bsize,      \
                                         size_t nblocks, int pe)                          \
    {                                                                                     \
        strided_block_get("shmem_ctx_ibget" #SIZE, ctx, dest, source, dst, sst, bsize,   \
                          nblocks, SIZE / 8, pe);                                         \
    }                                                                                     \
    OSOE_API void pshmem_ibget##SIZE(void *dest, const void *source, ptrdiff_t dst,       \
                                     ptrdiff_t sst, size_t bsize, size_t nblocks, int pe) \
    {                                                                                     \
        strided_block_get("shmem_ibget" #SIZE, SHMEM_CTX_DEFAULT, dest, source, dst, sst, \
                          bsize, nblocks, SIZE / 8, pe);                                  \
    }                                                                                     \
    OSOE_API void pshmem_ctx_put##SIZE##_nbi(shmem_ctx_t ctx, void *dest,                 \
                                             const void *source, size_t nelems, int pe)   \
    {                                                                                     \
        osoe_rma_put("shmem_ctx_put" #SIZE "_nbi", ctx, dest, source, nelems, SIZE / 8,   \
                     pe, 0);                                                              \
    }                                                                                     \
    OSOE_API void pshmem_put##SIZE##_nbi(void *dest, const void *source, size_t nelems,   \
                                         int pe)                                          \
    {                                                                                     \
        osoe_rma_put("shmem_put" #SIZE "_nbi", SHMEM_CTX_DEFAULT, dest, source, nelems,   \
                     SIZE / 8, pe, 0);                                                    \
    }                                                                                     \
    OSOE_API void pshmem_ctx_get##SIZE##_nbi(shmem_ctx_t ctx, void *dest,                 \
                                             const void *source, size_t nelems, int pe)   \
    {                                                                                     \
        osoe_rma_get("shmem_ctx_get" #SIZE "_nbi", ctx, dest, source, nelems, SIZE / 8,   \
                     pe, OSOE_TRANSPORT_NONBLOCKING);                                     \
    }                                                                                     \
    OSOE_API void pshmem_get##SIZE##_nbi(void *dest, const void *source, size_t nelems,   \
                                         int pe)                                          \
    {                                                                                     \
        osoe_rma_get("shmem_get" #SIZE "_nbi", SHMEM_CTX_DEFAULT, dest, source, nelems,   \
                     SIZE / 8, pe, OSOE_TRANSPORT_NONBLOCKING);                           \
    }

OSOE_RMA_SIZES(OSOE_RMA_SIZED)

/* ---- The byte forms' nonblocking spellings (spec §9.6.2.1-§9.6.2.2) ---- */

OSOE_API void pshmem_ctx_putmem_nbi(shmem_ctx_t ctx, void *dest, const void *source,
                                    size_t nelems, int pe)
{
    osoe_rma_put("shmem_ctx_putmem_nbi", ctx, dest, source, nelems, 1, pe, 0);
}

OSOE_API void pshmem_putmem_nbi(void *dest, const void *source, size_t nelems, int pe)
{
    osoe_rma_put("shmem_putmem_nbi", SHMEM_CTX_DEFAULT, dest, source, nelems, 1, pe, 0);
}

OSOE_API void pshmem_ctx_getmem_nbi(shmem_ctx_t ctx, void *dest, const void *source,
                                    size_t nelems, int pe)
{
    osoe_rma_get("shmem_ctx_getmem_nbi", ctx, dest, source, nelems, 1, pe,
                 OSOE_TRANSPORT_NONBLOCKING);
}

OSOE_API void pshmem_getmem_nbi(void *dest, const void *source, size_t nelems, int pe)
{
    osoe_rma_get("shmem_getmem_nbi", SHMEM_CTX_DEFAULT, dest, source, nelems, 1, pe,
                 OSOE_TRANSPORT_NONBLOCKING);
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
    OSOE_ALIAS(ctx_##NAME##_ibput) OSOE_ALIAS(NAME##_ibput)                               \
    OSOE_ALIAS(ctx_##NAME##_ibget) OSOE_ALIAS(NAME##_ibget)                               \
    OSOE_ALIAS(ctx_##NAME##_put_nbi) OSOE_ALIAS(NAME##_put_nbi)                           \
    OSOE_ALIAS(ctx_##NAME##_get_nbi) OSOE_ALIAS(NAME##_get_nbi)

#define OSOE_RMA_ALIAS_SIZED(SIZE)                                                        \
    OSOE_ALIAS(ctx_put##SIZE) OSOE_ALIAS(put##SIZE)                                       \
    OSOE_ALIAS(ctx_get##SIZE) OSOE_ALIAS(get##SIZE)                                       \
    OSOE_ALIAS(ctx_iput##SIZE) OSOE_ALIAS(iput##SIZE)                                     \
    OSOE_ALIAS(ctx_iget##SIZE) OSOE_ALIAS(iget##SIZE)                                     \
    OSOE_ALIAS(ctx_ibput##SIZE) OSOE_ALIAS(ibput##SIZE)                                   \
    OSOE_ALIAS(ctx_ibget##SIZE) OSOE_ALIAS(ibget##SIZE)                                   \
    OSOE_ALIAS(ctx_put##SIZE##_nbi) OSOE_ALIAS(put##SIZE##_nbi)                           \
    OSOE_ALIAS(ctx_get##SIZE##_nbi) OSOE_ALIAS(get##SIZE##_nbi)

OSOE_RMA_STANDARD_TYPES(OSOE_RMA_ALIAS_TYPED)
OSOE_RMA_SIZES(OSOE_RMA_ALIAS_SIZED)

OSOE_ALIAS(ctx_putmem_nbi)
OSOE_ALIAS(putmem_nbi)
OSOE_ALIAS(ctx_getmem_nbi)
OSOE_ALIAS(getmem_nbi)
