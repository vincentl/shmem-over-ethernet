/*
 * shmem.h - OpenSHMEM 1.6 application programming interface, as implemented
 * by osoe (OpenSHMEM Over Ethernet).
 *
 * Only the routines osoe implements are declared; the header grows with the
 * implementation. Section numbers refer to the OpenSHMEM 1.6 specification.
 */
#ifndef OSOE_SHMEM_H
#define OSOE_SHMEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Library constants (§6) */
#define SHMEM_MAJOR_VERSION 1
#define SHMEM_MINOR_VERSION 6
#define SHMEM_MAX_NAME_LEN  256
#define SHMEM_VENDOR_STRING "osoe"

/* Deprecated spellings of the constants above (§6), kept for old programs */
#define _SHMEM_MAJOR_VERSION SHMEM_MAJOR_VERSION
#define _SHMEM_MINOR_VERSION SHMEM_MINOR_VERSION
#define _SHMEM_MAX_NAME_LEN  SHMEM_MAX_NAME_LEN
#define _SHMEM_VENDOR_STRING SHMEM_VENDOR_STRING

/*
 * The deprecated active-set collective constants (§6, Annex F.2.8).
 *
 * The active-set routines that take them run over the whole job only (design
 * §7.4): an active set of every PE - PE_start 0, logPE_stride 0, PE_size the
 * job's size - works, and any other ends the job by name. That is the set every
 * program in the corpus that still calls these routines passes, and forming a
 * group for an arbitrary set of PEs is machinery osoe does not have: an answer
 * over the wrong PEs would be worse than a named refusal. pSync and pWrk are
 * neither read nor written, which leaves pSync as the spec requires it to be
 * found afterwards.
 *
 * The sizes are this implementation's, as the spec says they are. They are
 * large enough for a conforming program's array and are otherwise unused.
 */
#define SHMEM_SYNC_VALUE               (-1L)
#define SHMEM_SYNC_SIZE                 256
#define SHMEM_BARRIER_SYNC_SIZE         256
#define SHMEM_BCAST_SYNC_SIZE           256
#define SHMEM_COLLECT_SYNC_SIZE         256
#define SHMEM_REDUCE_SYNC_SIZE          256
#define SHMEM_ALLTOALL_SYNC_SIZE        256
#define SHMEM_ALLTOALLS_SYNC_SIZE       256
#define SHMEM_REDUCE_MIN_WRKDATA_SIZE   256

#define _SHMEM_SYNC_VALUE              SHMEM_SYNC_VALUE
#define _SHMEM_BARRIER_SYNC_SIZE       SHMEM_BARRIER_SYNC_SIZE
#define _SHMEM_BCAST_SYNC_SIZE         SHMEM_BCAST_SYNC_SIZE
#define _SHMEM_COLLECT_SYNC_SIZE       SHMEM_COLLECT_SYNC_SIZE
#define _SHMEM_REDUCE_SYNC_SIZE        SHMEM_REDUCE_SYNC_SIZE
#define _SHMEM_REDUCE_MIN_WRKDATA_SIZE SHMEM_REDUCE_MIN_WRKDATA_SIZE

/* Library setup, exit, and query (§9.1) */
void shmem_init(void);
int shmem_my_pe(void);
int shmem_n_pes(void);
void shmem_finalize(void);
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Noreturn
#endif
void shmem_global_exit(int status);
void shmem_query_initialized(int *initialized);

/*
 * Thread levels (§6, §9.2). The spec requires increasing integer values, and
 * osoe provides up to SHMEM_THREAD_SERIALIZED: SHMEM_THREAD_MULTIPLE is §4.11
 * of the design notes and is not implemented, so asking for it yields
 * SERIALIZED rather than a failure, which is what the interface is shaped for.
 */
#define SHMEM_THREAD_SINGLE     0
#define SHMEM_THREAD_FUNNELED   1
#define SHMEM_THREAD_SERIALIZED 2
#define SHMEM_THREAD_MULTIPLE   3

int shmem_init_thread(int requested, int *provided);
void shmem_query_thread(int *provided);

/*
 * Teams and contexts (§6, §9.4, §9.5).
 *
 * Both handles are opaque, so their representation is this implementation's
 * choice. They are pointers to incomplete types rather than integers so that a
 * team passed where a context is expected is a compile error, and the
 * predefined handles are integer constants cast to those types rather than
 * exported objects - a library that exported `SHMEM_TEAM_WORLD` as data would
 * be exporting a symbol outside the `shmem_` namespace the export test
 * enforces, for no gain.
 *
 * osoe implements SHMEM_TEAM_WORLD, SHMEM_TEAM_SHARED and SHMEM_CTX_DEFAULT
 * (§7.3). `shmem_ctx_create`, `shmem_team_create_ctx` and the team splits
 * exist and return nonzero, which the spec permits: an unsuccessful context
 * creation "is not treated as an error". The parameter is on every entry point
 * from the start because retrofitting it later touches every function written.
 */
typedef struct shmem_team_opaque *shmem_team_t;
typedef struct shmem_ctx_opaque *shmem_ctx_t;

#define SHMEM_TEAM_INVALID ((shmem_team_t)0)
#define SHMEM_TEAM_WORLD   ((shmem_team_t)1)
#define SHMEM_TEAM_SHARED  ((shmem_team_t)2)

#define SHMEM_CTX_INVALID  ((shmem_ctx_t)0)
#define SHMEM_CTX_DEFAULT  ((shmem_ctx_t)1)

/* Context creation options (§6, §9.5.1) */
#define SHMEM_CTX_SERIALIZED 1L
#define SHMEM_CTX_PRIVATE    2L
#define SHMEM_CTX_NOSTORE    4L

/* Team configuration (§9.4.3) */
typedef struct {
    int num_contexts;
} shmem_team_config_t;

#define SHMEM_TEAM_NUM_CONTEXTS 1L

int shmem_team_my_pe(shmem_team_t team);
int shmem_team_n_pes(shmem_team_t team);
int shmem_team_get_config(shmem_team_t team, long config_mask,
                          shmem_team_config_t *config);
int shmem_team_translate_pe(shmem_team_t src_team, int src_pe, shmem_team_t dest_team);
int shmem_team_split_strided(shmem_team_t parent_team, int start, int stride, int size,
                             const shmem_team_config_t *config, long config_mask,
                             shmem_team_t *new_team);
int shmem_team_split_2d(shmem_team_t parent_team, int xrange,
                        const shmem_team_config_t *xaxis_config, long xaxis_mask,
                        shmem_team_t *xaxis_team,
                        const shmem_team_config_t *yaxis_config, long yaxis_mask,
                        shmem_team_t *yaxis_team);
void shmem_team_destroy(shmem_team_t team);
void *shmem_team_ptr(shmem_team_t team, const void *dest, int pe);

int shmem_ctx_create(long options, shmem_ctx_t *ctx);
int shmem_team_create_ctx(shmem_team_t team, long options, shmem_ctx_t *ctx);
void shmem_ctx_destroy(shmem_ctx_t ctx);
int shmem_ctx_get_team(shmem_ctx_t ctx, shmem_team_t *team);

/* Comparison constants for the wait and test routines (§6, §9.11) */
#define SHMEM_CMP_EQ 0
#define SHMEM_CMP_NE 1
#define SHMEM_CMP_GT 2
#define SHMEM_CMP_LE 3
#define SHMEM_CMP_LT 4
#define SHMEM_CMP_GE 5

/* Deprecated spellings (§6) */
#define _SHMEM_CMP_EQ SHMEM_CMP_EQ
#define _SHMEM_CMP_NE SHMEM_CMP_NE
#define _SHMEM_CMP_GT SHMEM_CMP_GT
#define _SHMEM_CMP_LE SHMEM_CMP_LE
#define _SHMEM_CMP_LT SHMEM_CMP_LT
#define _SHMEM_CMP_GE SHMEM_CMP_GE

/* Signal operators for the put-with-signal routines (§6, §9.8.2) */
#define SHMEM_SIGNAL_SET 0
#define SHMEM_SIGNAL_ADD 1

/* Signalling operations (§9.8) */
void shmem_putmem_signal(void *dest, const void *source, size_t nelems, uint64_t *sig_addr,
                         uint64_t signal, int sig_op, int pe);
void shmem_ctx_putmem_signal(shmem_ctx_t ctx, void *dest, const void *source,
                             size_t nelems, uint64_t *sig_addr, uint64_t signal,
                             int sig_op, int pe);
/* The nonblocking put-with-signal byte form (§9.8.4); "returns after
 * initiating the operation", completed by a subsequent shmem_quiet, exactly as
 * shmem_putmem_nbi below. Defined alongside shmem_putmem_signal in signal.c. */
void shmem_putmem_signal_nbi(void *dest, const void *source, size_t nelems,
                             uint64_t *sig_addr, uint64_t signal, int sig_op, int pe);
void shmem_ctx_putmem_signal_nbi(shmem_ctx_t ctx, void *dest, const void *source,
                                 size_t nelems, uint64_t *sig_addr, uint64_t signal,
                                 int sig_op, int pe);
void shmem_signal_set(uint64_t *sig_addr, uint64_t signal, int pe);
void shmem_signal_add(uint64_t *sig_addr, uint64_t signal, int pe);
void shmem_ctx_signal_set(shmem_ctx_t ctx, uint64_t *sig_addr, uint64_t signal, int pe);
void shmem_ctx_signal_add(shmem_ctx_t ctx, uint64_t *sig_addr, uint64_t signal, int pe);
uint64_t shmem_signal_fetch(const uint64_t *sig_addr);
uint64_t shmem_signal_wait_until(uint64_t *sig_addr, int cmp, uint64_t cmp_value);

/*
 * Atomic memory operations (spec §9.7).
 *
 * The spec names one routine per (operation, type), over three type sets:
 * Table 6 "standard" for compare_swap, inc, fetch_inc, add and fetch_add;
 * Table 7 "extended", which is Table 6 plus float and double, for fetch, set
 * and swap; and Table 8 "bitwise" for the and, or and xor families. They are
 * declared here by expanding the type lists, the same way the definitions are
 * generated, so a type appears once and the two cannot disagree.
 *
 * Every type in the three tables is four or eight bytes on this
 * implementation's targets (LP64 Linux), which is the width the transport
 * boundary's rmw takes. The library's own build asserts that, and asserts that
 * each exact-width typedef really is the canonical type the C11 selection below
 * lists, so a platform where they differ fails to build the library rather than
 * silently omitting a case here.
 */
#define OSOE_AMO_STANDARD_TYPES(X)                                            \
    X(int, int) X(long, long) X(long long, longlong)                          \
    X(unsigned int, uint) X(unsigned long, ulong)                             \
    X(unsigned long long, ulonglong)                                          \
    X(int32_t, int32) X(int64_t, int64) X(uint32_t, uint32) X(uint64_t, uint64) \
    X(size_t, size) X(ptrdiff_t, ptrdiff)

/* Table 7 is Table 6 plus these two. */
#define OSOE_AMO_FLOAT_TYPES(X) X(float, float) X(double, double)

#define OSOE_AMO_BITWISE_TYPES(X)                                             \
    X(unsigned int, uint) X(unsigned long, ulong)                             \
    X(unsigned long long, ulonglong)                                          \
    X(int32_t, int32) X(int64_t, int64) X(uint32_t, uint32) X(uint64_t, uint64)

/*
 * The declarations take the prefix as a parameter, so that pshmem.h can
 * generate the `pshmem_` spellings of the same 500 routines from the same
 * lists (§7.4; spec §10). Writing them out twice is the version of this that
 * drifts.
 */
#define OSOE_CAT2(a, b) a##b
#define OSOE_CAT(a, b) OSOE_CAT2(a, b)
#define OSOE_FN(P, NAME, SUF) OSOE_CAT(OSOE_CAT(P, NAME), SUF)

#define OSOE_AMO_DECL_STANDARD_P(P, T, NAME)                                  \
    T OSOE_FN(P, NAME, _atomic_compare_swap)(T *dest, T cond, T value, int pe); \
    void OSOE_FN(P, NAME, _atomic_inc)(T *dest, int pe);                      \
    T OSOE_FN(P, NAME, _atomic_fetch_inc)(T *dest, int pe);                   \
    void OSOE_FN(P, NAME, _atomic_add)(T *dest, T value, int pe);             \
    T OSOE_FN(P, NAME, _atomic_fetch_add)(T *dest, T value, int pe);

#define OSOE_AMO_DECL_EXTENDED_P(P, T, NAME)                                  \
    T OSOE_FN(P, NAME, _atomic_fetch)(const T *source, int pe);               \
    void OSOE_FN(P, NAME, _atomic_set)(T *dest, T value, int pe);             \
    T OSOE_FN(P, NAME, _atomic_swap)(T *dest, T value, int pe);

#define OSOE_AMO_DECL_BITWISE_P(P, T, NAME)                                   \
    void OSOE_FN(P, NAME, _atomic_and)(T *dest, T value, int pe);             \
    T OSOE_FN(P, NAME, _atomic_fetch_and)(T *dest, T value, int pe);          \
    void OSOE_FN(P, NAME, _atomic_or)(T *dest, T value, int pe);              \
    T OSOE_FN(P, NAME, _atomic_fetch_or)(T *dest, T value, int pe);           \
    void OSOE_FN(P, NAME, _atomic_xor)(T *dest, T value, int pe);             \
    T OSOE_FN(P, NAME, _atomic_fetch_xor)(T *dest, T value, int pe);

#define OSOE_AMO_DECL_CTX_STANDARD_P(P, T, NAME)                              \
    T OSOE_FN(P, ctx_##NAME, _atomic_compare_swap)(shmem_ctx_t ctx, T *dest,  \
                                                   T cond, T value, int pe);  \
    void OSOE_FN(P, ctx_##NAME, _atomic_inc)(shmem_ctx_t ctx, T *dest, int pe); \
    T OSOE_FN(P, ctx_##NAME, _atomic_fetch_inc)(shmem_ctx_t ctx, T *dest, int pe); \
    void OSOE_FN(P, ctx_##NAME, _atomic_add)(shmem_ctx_t ctx, T *dest, T value, \
                                             int pe);                         \
    T OSOE_FN(P, ctx_##NAME, _atomic_fetch_add)(shmem_ctx_t ctx, T *dest, T value, \
                                                int pe);

#define OSOE_AMO_DECL_CTX_EXTENDED_P(P, T, NAME)                              \
    T OSOE_FN(P, ctx_##NAME, _atomic_fetch)(shmem_ctx_t ctx, const T *source, int pe); \
    void OSOE_FN(P, ctx_##NAME, _atomic_set)(shmem_ctx_t ctx, T *dest, T value, int pe); \
    T OSOE_FN(P, ctx_##NAME, _atomic_swap)(shmem_ctx_t ctx, T *dest, T value, int pe);

#define OSOE_AMO_DECL_CTX_BITWISE_P(P, T, NAME)                               \
    void OSOE_FN(P, ctx_##NAME, _atomic_and)(shmem_ctx_t ctx, T *dest, T value, int pe); \
    T OSOE_FN(P, ctx_##NAME, _atomic_fetch_and)(shmem_ctx_t ctx, T *dest, T value, int pe); \
    void OSOE_FN(P, ctx_##NAME, _atomic_or)(shmem_ctx_t ctx, T *dest, T value, int pe); \
    T OSOE_FN(P, ctx_##NAME, _atomic_fetch_or)(shmem_ctx_t ctx, T *dest, T value, int pe); \
    void OSOE_FN(P, ctx_##NAME, _atomic_xor)(shmem_ctx_t ctx, T *dest, T value, int pe); \
    T OSOE_FN(P, ctx_##NAME, _atomic_fetch_xor)(shmem_ctx_t ctx, T *dest, T value, int pe);

#define OSOE_AMO_DECL_STANDARD(T, NAME) OSOE_AMO_DECL_STANDARD_P(shmem_, T, NAME)
#define OSOE_AMO_DECL_EXTENDED(T, NAME) OSOE_AMO_DECL_EXTENDED_P(shmem_, T, NAME)
#define OSOE_AMO_DECL_BITWISE(T, NAME) OSOE_AMO_DECL_BITWISE_P(shmem_, T, NAME)
#define OSOE_AMO_DECL_CTX_STANDARD(T, NAME) OSOE_AMO_DECL_CTX_STANDARD_P(shmem_, T, NAME)
#define OSOE_AMO_DECL_CTX_EXTENDED(T, NAME) OSOE_AMO_DECL_CTX_EXTENDED_P(shmem_, T, NAME)
#define OSOE_AMO_DECL_CTX_BITWISE(T, NAME) OSOE_AMO_DECL_CTX_BITWISE_P(shmem_, T, NAME)

OSOE_AMO_STANDARD_TYPES(OSOE_AMO_DECL_STANDARD)
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_DECL_EXTENDED)
OSOE_AMO_FLOAT_TYPES(OSOE_AMO_DECL_EXTENDED)
OSOE_AMO_BITWISE_TYPES(OSOE_AMO_DECL_BITWISE)
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_DECL_CTX_STANDARD)
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_DECL_CTX_EXTENDED)
OSOE_AMO_FLOAT_TYPES(OSOE_AMO_DECL_CTX_EXTENDED)
OSOE_AMO_BITWISE_TYPES(OSOE_AMO_DECL_CTX_BITWISE)

/*
 * The nonblocking fetching AMOs (§9.7.2): the same eight operations as
 * above - fetch, swap, compare_swap, fetch_inc, fetch_add, fetch_and,
 * fetch_or, fetch_xor - each with a leading `fetch` output parameter in place
 * of the blocking form's return value, over the same three type lists.
 * amo.c says what "nonblocking" means for osoe's definitions and why storing
 * through *fetch before return is conforming.
 */
#define OSOE_AMO_DECL_STANDARD_NBI_P(P, T, NAME)                             \
    void OSOE_FN(P, NAME, _atomic_compare_swap_nbi)(T *fetch, T *dest, T cond, \
                                                    T value, int pe);         \
    void OSOE_FN(P, NAME, _atomic_fetch_inc_nbi)(T *fetch, T *dest, int pe); \
    void OSOE_FN(P, NAME, _atomic_fetch_add_nbi)(T *fetch, T *dest, T value, int pe);

#define OSOE_AMO_DECL_EXTENDED_NBI_P(P, T, NAME)                             \
    void OSOE_FN(P, NAME, _atomic_fetch_nbi)(T *fetch, const T *source, int pe); \
    void OSOE_FN(P, NAME, _atomic_swap_nbi)(T *fetch, T *dest, T value, int pe);

#define OSOE_AMO_DECL_BITWISE_NBI_P(P, T, NAME)                              \
    void OSOE_FN(P, NAME, _atomic_fetch_and_nbi)(T *fetch, T *dest, T value, int pe); \
    void OSOE_FN(P, NAME, _atomic_fetch_or_nbi)(T *fetch, T *dest, T value, int pe); \
    void OSOE_FN(P, NAME, _atomic_fetch_xor_nbi)(T *fetch, T *dest, T value, int pe);

#define OSOE_AMO_DECL_CTX_STANDARD_NBI_P(P, T, NAME)                         \
    void OSOE_FN(P, ctx_##NAME, _atomic_compare_swap_nbi)(shmem_ctx_t ctx,   \
                                                          T *fetch, T *dest, \
                                                          T cond, T value, int pe); \
    void OSOE_FN(P, ctx_##NAME, _atomic_fetch_inc_nbi)(shmem_ctx_t ctx, T *fetch, \
                                                       T *dest, int pe);      \
    void OSOE_FN(P, ctx_##NAME, _atomic_fetch_add_nbi)(shmem_ctx_t ctx, T *fetch, \
                                                       T *dest, T value, int pe);

#define OSOE_AMO_DECL_CTX_EXTENDED_NBI_P(P, T, NAME)                         \
    void OSOE_FN(P, ctx_##NAME, _atomic_fetch_nbi)(shmem_ctx_t ctx, T *fetch, \
                                                   const T *source, int pe);  \
    void OSOE_FN(P, ctx_##NAME, _atomic_swap_nbi)(shmem_ctx_t ctx, T *fetch, \
                                                  T *dest, T value, int pe);

#define OSOE_AMO_DECL_CTX_BITWISE_NBI_P(P, T, NAME)                          \
    void OSOE_FN(P, ctx_##NAME, _atomic_fetch_and_nbi)(shmem_ctx_t ctx, T *fetch, \
                                                       T *dest, T value, int pe); \
    void OSOE_FN(P, ctx_##NAME, _atomic_fetch_or_nbi)(shmem_ctx_t ctx, T *fetch, \
                                                      T *dest, T value, int pe); \
    void OSOE_FN(P, ctx_##NAME, _atomic_fetch_xor_nbi)(shmem_ctx_t ctx, T *fetch, \
                                                       T *dest, T value, int pe);

#define OSOE_AMO_DECL_STANDARD_NBI(T, NAME) OSOE_AMO_DECL_STANDARD_NBI_P(shmem_, T, NAME)
#define OSOE_AMO_DECL_EXTENDED_NBI(T, NAME) OSOE_AMO_DECL_EXTENDED_NBI_P(shmem_, T, NAME)
#define OSOE_AMO_DECL_BITWISE_NBI(T, NAME) OSOE_AMO_DECL_BITWISE_NBI_P(shmem_, T, NAME)
#define OSOE_AMO_DECL_CTX_STANDARD_NBI(T, NAME) \
    OSOE_AMO_DECL_CTX_STANDARD_NBI_P(shmem_, T, NAME)
#define OSOE_AMO_DECL_CTX_EXTENDED_NBI(T, NAME) \
    OSOE_AMO_DECL_CTX_EXTENDED_NBI_P(shmem_, T, NAME)
#define OSOE_AMO_DECL_CTX_BITWISE_NBI(T, NAME) \
    OSOE_AMO_DECL_CTX_BITWISE_NBI_P(shmem_, T, NAME)

OSOE_AMO_STANDARD_TYPES(OSOE_AMO_DECL_STANDARD_NBI)
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_DECL_EXTENDED_NBI)
OSOE_AMO_FLOAT_TYPES(OSOE_AMO_DECL_EXTENDED_NBI)
OSOE_AMO_BITWISE_TYPES(OSOE_AMO_DECL_BITWISE_NBI)
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_DECL_CTX_STANDARD_NBI)
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_DECL_CTX_EXTENDED_NBI)
OSOE_AMO_FLOAT_TYPES(OSOE_AMO_DECL_CTX_EXTENDED_NBI)
OSOE_AMO_BITWISE_TYPES(OSOE_AMO_DECL_CTX_BITWISE_NBI)

/*
 * Point-to-point synchronization (spec §9.11).
 *
 * The wait and test routines take the standard AMO types of Table 6. The
 * scalar shmem_wait_until and shmem_test additionally take the deprecated
 * `short` and `unsigned short` of Table 12, which are two bytes wide - a width
 * no AMO type has, and the only reason the transport boundary's coherent load
 * takes a width of 2 at all. There is no AMO for them, so a conforming program
 * cannot update one (1.5 change #17 requires wait variables to be written with
 * AMOs); they exist for programs written before that rule.
 */
#define OSOE_WAIT_SHORT_TYPES(X) X(short, short) X(unsigned short, ushort)

/* The deprecated shmem_TYPENAME_wait, which is wait_until with SHMEM_CMP_NE. */
#define OSOE_WAIT_DEPRECATED_TYPES(X)                                         \
    X(short, short) X(int, int) X(long, long) X(long long, longlong)

#define OSOE_WAIT_DECL_SCALAR_P(P, T, NAME)                                   \
    void OSOE_FN(P, NAME, _wait_until)(T *ivar, int cmp, T cmp_value);        \
    int OSOE_FN(P, NAME, _test)(T *ivar, int cmp, T cmp_value);

#define OSOE_WAIT_DECL_MANY_P(P, T, NAME)                                     \
    void OSOE_FN(P, NAME, _wait_until_all)(T *ivars, size_t nelems,           \
                                           const int *status, int cmp, T cmp_value); \
    size_t OSOE_FN(P, NAME, _wait_until_any)(T *ivars, size_t nelems,         \
                                             const int *status, int cmp, T cmp_value); \
    size_t OSOE_FN(P, NAME, _wait_until_some)(T *ivars, size_t nelems, size_t *indices, \
                                              const int *status, int cmp, T cmp_value); \
    void OSOE_FN(P, NAME, _wait_until_all_vector)(T *ivars, size_t nelems,    \
                                                  const int *status, int cmp, \
                                                  const T *cmp_values);       \
    size_t OSOE_FN(P, NAME, _wait_until_any_vector)(T *ivars, size_t nelems,  \
                                                    const int *status, int cmp, \
                                                    const T *cmp_values);     \
    size_t OSOE_FN(P, NAME, _wait_until_some_vector)(T *ivars, size_t nelems, \
                                                     size_t *indices,         \
                                                     const int *status, int cmp, \
                                                     const T *cmp_values);    \
    int OSOE_FN(P, NAME, _test_all)(T *ivars, size_t nelems, const int *status, \
                                    int cmp, T cmp_value);                    \
    size_t OSOE_FN(P, NAME, _test_any)(T *ivars, size_t nelems, const int *status, \
                                       int cmp, T cmp_value);                 \
    size_t OSOE_FN(P, NAME, _test_some)(T *ivars, size_t nelems, size_t *indices, \
                                        const int *status, int cmp, T cmp_value); \
    int OSOE_FN(P, NAME, _test_all_vector)(T *ivars, size_t nelems,           \
                                           const int *status, int cmp,        \
                                           const T *cmp_values);              \
    size_t OSOE_FN(P, NAME, _test_any_vector)(T *ivars, size_t nelems,        \
                                              const int *status, int cmp,     \
                                              const T *cmp_values);           \
    size_t OSOE_FN(P, NAME, _test_some_vector)(T *ivars, size_t nelems,       \
                                               size_t *indices, const int *status, \
                                               int cmp, const T *cmp_values);

#define OSOE_WAIT_DECL_DEPRECATED_P(P, T, NAME)                               \
    void OSOE_FN(P, NAME, _wait)(T *ivar, T cmp_value);

#define OSOE_WAIT_DECL_SCALAR(T, NAME) OSOE_WAIT_DECL_SCALAR_P(shmem_, T, NAME)
#define OSOE_WAIT_DECL_MANY(T, NAME) OSOE_WAIT_DECL_MANY_P(shmem_, T, NAME)
#define OSOE_WAIT_DECL_DEPRECATED(T, NAME) OSOE_WAIT_DECL_DEPRECATED_P(shmem_, T, NAME)

OSOE_AMO_STANDARD_TYPES(OSOE_WAIT_DECL_SCALAR)
OSOE_WAIT_SHORT_TYPES(OSOE_WAIT_DECL_SCALAR)
OSOE_AMO_STANDARD_TYPES(OSOE_WAIT_DECL_MANY)
OSOE_WAIT_DEPRECATED_TYPES(OSOE_WAIT_DECL_DEPRECATED)

/* The unprefixed deprecated spellings, which the spec fixes at `long`. They
 * are declared before the C11 generic macros of the same name are defined, so
 * a program can still reach them as (shmem_wait)(...). */
void shmem_wait(long *ivar, long cmp_value);
void shmem_wait_until(long *ivar, int cmp, long cmp_value);

/* Symmetric pointer (§9.1.9) */
void *shmem_ptr(const void *dest, int pe);

/* Memory usage hints for shmem_malloc_with_hints (§6, §9.3.5, Table 4) */
#define SHMEM_MALLOC_ATOMICS_REMOTE 1L
#define SHMEM_MALLOC_SIGNAL_REMOTE  2L

/* Memory management (§9.3) */
void *shmem_malloc(size_t size);
void *shmem_calloc(size_t count, size_t size);
void *shmem_realloc(void *ptr, size_t size);
void *shmem_align(size_t alignment, size_t size);
void *shmem_malloc_with_hints(size_t size, long hints);
void shmem_free(void *ptr);

/* Remote memory access (§9.6.1) */
void shmem_putmem(void *dest, const void *source, size_t nelems, int pe);
void shmem_getmem(void *dest, const void *source, size_t nelems, int pe);
void shmem_ctx_putmem(shmem_ctx_t ctx, void *dest, const void *source, size_t nelems,
                      int pe);
void shmem_ctx_getmem(shmem_ctx_t ctx, void *dest, const void *source, size_t nelems,
                      int pe);

/* The nonblocking byte forms (§9.6.2.1-§9.6.2.2): "return after initiating the
 * operation", completed by a subsequent shmem_quiet. Defined in rma-typed.c
 * beside the typed and sized nonblocking forms below. */
void shmem_putmem_nbi(void *dest, const void *source, size_t nelems, int pe);
void shmem_getmem_nbi(void *dest, const void *source, size_t nelems, int pe);
void shmem_ctx_putmem_nbi(shmem_ctx_t ctx, void *dest, const void *source, size_t nelems,
                          int pe);
void shmem_ctx_getmem_nbi(shmem_ctx_t ctx, void *dest, const void *source, size_t nelems,
                          int pe);

/*
 * Typed and sized RMA (spec §9.6, §9.8.3-§9.8.4): the standard RMA types of
 * Table 5, and the sized putSIZE/getSIZE that take nelems of that many bits
 * each. Declared here by expanding the type list, the same way rma-typed.c
 * (and signal.c, for the put-with-signal families) generates the definitions,
 * so a type appears once (§7.4's rule for the AMO tables, applied to RMA).
 *
 * The list is Table 5's own order: the fourteen types the C11 generic
 * selection below dispatches on directly, then ten that are typedefs of one of
 * those fourteen on this implementation's targets (LP64 Linux) - int8_t/
 * uint8_t are signed/unsigned char, int16_t/uint16_t are short/unsigned short,
 * int32_t/uint32_t are int/unsigned int, int64_t/uint64_t/ptrdiff_t/size_t are
 * long/unsigned long. rma-typed.c's static assertions check the two narrower
 * identities that amo.c's own do not already establish for Tables 6-8.
 */
#define OSOE_RMA_STANDARD_TYPES(X)                                           \
    X(float, float) X(double, double) X(long double, longdouble)             \
    X(char, char) X(signed char, schar) X(short, short)                      \
    X(int, int) X(long, long) X(long long, longlong)                         \
    X(unsigned char, uchar) X(unsigned short, ushort)                        \
    X(unsigned int, uint) X(unsigned long, ulong)                            \
    X(unsigned long long, ulonglong)                                         \
    X(int8_t, int8) X(int16_t, int16) X(int32_t, int32) X(int64_t, int64)    \
    X(uint8_t, uint8) X(uint16_t, uint16) X(uint32_t, uint32)                \
    X(uint64_t, uint64) X(size_t, size) X(ptrdiff_t, ptrdiff)

/* The sized forms' widths, in bits (e.g. spec §9.6.1.1: "SIZE is one of 8, 16,
 * 32, 64, 128"). A routine's byte count is nelems * (SIZE / 8). */
#define OSOE_RMA_SIZES(X) X(8) X(16) X(32) X(64) X(128)

/* Declared with the prefix as a parameter, as the AMO families above:
 * pshmem.h generates the `pshmem_` spellings from the same lists and macros. */
#define OSOE_RMA_DECL_PUT_GET_P(P, T, NAME)                                  \
    void OSOE_FN(P, ctx_##NAME, _put)(shmem_ctx_t ctx, T *dest,              \
                        const T *source, size_t nelems, int pe);             \
    void OSOE_FN(P, NAME, _put)(T *dest, const T *source, size_t nelems, int pe); \
    void OSOE_FN(P, ctx_##NAME, _get)(shmem_ctx_t ctx, T *dest,              \
                        const T *source, size_t nelems, int pe);             \
    void OSOE_FN(P, NAME, _get)(T *dest, const T *source, size_t nelems, int pe);

#define OSOE_RMA_DECL_P_G_P(P, T, NAME)                                      \
    void OSOE_FN(P, ctx_##NAME, _p)(shmem_ctx_t ctx, T *dest, T value, int pe); \
    void OSOE_FN(P, NAME, _p)(T *dest, T value, int pe);                     \
    T OSOE_FN(P, ctx_##NAME, _g)(shmem_ctx_t ctx, const T *source, int pe);  \
    T OSOE_FN(P, NAME, _g)(const T *source, int pe);

#define OSOE_RMA_DECL_IPUT_IGET_P(P, T, NAME)                                \
    void OSOE_FN(P, ctx_##NAME, _iput)(shmem_ctx_t ctx, T *dest,             \
                        const T *source, ptrdiff_t dst, ptrdiff_t sst,       \
                        size_t nelems, int pe);                              \
    void OSOE_FN(P, NAME, _iput)(T *dest, const T *source, ptrdiff_t dst,    \
                        ptrdiff_t sst, size_t nelems, int pe);               \
    void OSOE_FN(P, ctx_##NAME, _iget)(shmem_ctx_t ctx, T *dest,             \
                        const T *source, ptrdiff_t dst, ptrdiff_t sst,       \
                        size_t nelems, int pe);                              \
    void OSOE_FN(P, NAME, _iget)(T *dest, const T *source, ptrdiff_t dst,    \
                        ptrdiff_t sst, size_t nelems, int pe);

#define OSOE_RMA_DECL_NBI_P(P, T, NAME)                                      \
    void OSOE_FN(P, ctx_##NAME, _put_nbi)(shmem_ctx_t ctx, T *dest,          \
                        const T *source, size_t nelems, int pe);             \
    void OSOE_FN(P, NAME, _put_nbi)(T *dest, const T *source, size_t nelems, int pe); \
    void OSOE_FN(P, ctx_##NAME, _get_nbi)(shmem_ctx_t ctx, T *dest,          \
                        const T *source, size_t nelems, int pe);             \
    void OSOE_FN(P, NAME, _get_nbi)(T *dest, const T *source, size_t nelems, int pe);

/* Typed put-with-signal (spec §9.8.3-§9.8.4). The byte form is declared above
 * with the rest of §9.8; these and it are all defined together in signal.c. */
#define OSOE_RMA_DECL_PUT_SIGNAL_P(P, T, NAME)                               \
    void OSOE_FN(P, ctx_##NAME, _put_signal)(shmem_ctx_t ctx, T *dest,       \
                        const T *source, size_t nelems, uint64_t *sig_addr,  \
                        uint64_t signal, int sig_op, int pe);                \
    void OSOE_FN(P, NAME, _put_signal)(T *dest, const T *source, size_t nelems, \
                        uint64_t *sig_addr, uint64_t signal, int sig_op, int pe);

#define OSOE_RMA_DECL_PUT_SIGNAL_NBI_P(P, T, NAME)                           \
    void OSOE_FN(P, ctx_##NAME, _put_signal_nbi)(shmem_ctx_t ctx, T *dest,   \
                        const T *source, size_t nelems, uint64_t *sig_addr,  \
                        uint64_t signal, int sig_op, int pe);                \
    void OSOE_FN(P, NAME, _put_signal_nbi)(T *dest, const T *source, size_t nelems, \
                        uint64_t *sig_addr, uint64_t signal, int sig_op, int pe);

#define OSOE_RMA_DECL_PUT_GET(T, NAME) OSOE_RMA_DECL_PUT_GET_P(shmem_, T, NAME)
#define OSOE_RMA_DECL_P_G(T, NAME) OSOE_RMA_DECL_P_G_P(shmem_, T, NAME)
#define OSOE_RMA_DECL_IPUT_IGET(T, NAME) OSOE_RMA_DECL_IPUT_IGET_P(shmem_, T, NAME)
#define OSOE_RMA_DECL_NBI(T, NAME) OSOE_RMA_DECL_NBI_P(shmem_, T, NAME)
#define OSOE_RMA_DECL_PUT_SIGNAL(T, NAME) OSOE_RMA_DECL_PUT_SIGNAL_P(shmem_, T, NAME)
#define OSOE_RMA_DECL_PUT_SIGNAL_NBI(T, NAME) OSOE_RMA_DECL_PUT_SIGNAL_NBI_P(shmem_, T, NAME)

OSOE_RMA_STANDARD_TYPES(OSOE_RMA_DECL_PUT_GET)
OSOE_RMA_STANDARD_TYPES(OSOE_RMA_DECL_P_G)
OSOE_RMA_STANDARD_TYPES(OSOE_RMA_DECL_IPUT_IGET)
OSOE_RMA_STANDARD_TYPES(OSOE_RMA_DECL_NBI)
OSOE_RMA_STANDARD_TYPES(OSOE_RMA_DECL_PUT_SIGNAL)
OSOE_RMA_STANDARD_TYPES(OSOE_RMA_DECL_PUT_SIGNAL_NBI)

/* The sized forms: putSIZE/getSIZE, their strided and nonblocking forms, and
 * the sized put-with-signal and put-with-signal-nbi. */
#define OSOE_RMA_DECL_SIZE_PUT_GET_P(P, SIZE)                                \
    void P##ctx_put##SIZE(shmem_ctx_t ctx, void *dest, const void *source,   \
                          size_t nelems, int pe);                            \
    void P##put##SIZE(void *dest, const void *source, size_t nelems, int pe); \
    void P##ctx_get##SIZE(shmem_ctx_t ctx, void *dest, const void *source,   \
                          size_t nelems, int pe);                            \
    void P##get##SIZE(void *dest, const void *source, size_t nelems, int pe);

#define OSOE_RMA_DECL_SIZE_IPUT_IGET_P(P, SIZE)                              \
    void P##ctx_iput##SIZE(shmem_ctx_t ctx, void *dest, const void *source,  \
                           ptrdiff_t dst, ptrdiff_t sst, size_t nelems, int pe); \
    void P##iput##SIZE(void *dest, const void *source, ptrdiff_t dst,        \
                       ptrdiff_t sst, size_t nelems, int pe);                 \
    void P##ctx_iget##SIZE(shmem_ctx_t ctx, void *dest, const void *source,  \
                           ptrdiff_t dst, ptrdiff_t sst, size_t nelems, int pe); \
    void P##iget##SIZE(void *dest, const void *source, ptrdiff_t dst,        \
                       ptrdiff_t sst, size_t nelems, int pe);

#define OSOE_RMA_DECL_SIZE_NBI_P(P, SIZE)                                    \
    void P##ctx_put##SIZE##_nbi(shmem_ctx_t ctx, void *dest, const void *source, \
                                size_t nelems, int pe);                      \
    void P##put##SIZE##_nbi(void *dest, const void *source, size_t nelems, int pe); \
    void P##ctx_get##SIZE##_nbi(shmem_ctx_t ctx, void *dest, const void *source, \
                                size_t nelems, int pe);                      \
    void P##get##SIZE##_nbi(void *dest, const void *source, size_t nelems, int pe);

#define OSOE_RMA_DECL_SIZE_PUT_SIGNAL_P(P, SIZE)                             \
    void P##ctx_put##SIZE##_signal(shmem_ctx_t ctx, void *dest, const void *source, \
                                   size_t nelems, uint64_t *sig_addr,        \
                                   uint64_t signal, int sig_op, int pe);     \
    void P##put##SIZE##_signal(void *dest, const void *source, size_t nelems, \
                               uint64_t *sig_addr, uint64_t signal, int sig_op, int pe);

#define OSOE_RMA_DECL_SIZE_PUT_SIGNAL_NBI_P(P, SIZE)                         \
    void P##ctx_put##SIZE##_signal_nbi(shmem_ctx_t ctx, void *dest, const void *source, \
                                       size_t nelems, uint64_t *sig_addr,    \
                                       uint64_t signal, int sig_op, int pe); \
    void P##put##SIZE##_signal_nbi(void *dest, const void *source, size_t nelems, \
                                   uint64_t *sig_addr, uint64_t signal, int sig_op, int pe);

#define OSOE_RMA_DECL_SIZE_PUT_GET(SIZE) OSOE_RMA_DECL_SIZE_PUT_GET_P(shmem_, SIZE)
#define OSOE_RMA_DECL_SIZE_IPUT_IGET(SIZE) OSOE_RMA_DECL_SIZE_IPUT_IGET_P(shmem_, SIZE)
#define OSOE_RMA_DECL_SIZE_NBI(SIZE) OSOE_RMA_DECL_SIZE_NBI_P(shmem_, SIZE)
#define OSOE_RMA_DECL_SIZE_PUT_SIGNAL(SIZE) OSOE_RMA_DECL_SIZE_PUT_SIGNAL_P(shmem_, SIZE)
#define OSOE_RMA_DECL_SIZE_PUT_SIGNAL_NBI(SIZE) OSOE_RMA_DECL_SIZE_PUT_SIGNAL_NBI_P(shmem_, SIZE)

OSOE_RMA_SIZES(OSOE_RMA_DECL_SIZE_PUT_GET)
OSOE_RMA_SIZES(OSOE_RMA_DECL_SIZE_IPUT_IGET)
OSOE_RMA_SIZES(OSOE_RMA_DECL_SIZE_NBI)
OSOE_RMA_SIZES(OSOE_RMA_DECL_SIZE_PUT_SIGNAL)
OSOE_RMA_SIZES(OSOE_RMA_DECL_SIZE_PUT_SIGNAL_NBI)

/* Memory ordering (§9.12) */
void shmem_fence(void);
void shmem_quiet(void);
void shmem_pe_quiet(const int *target_pes, size_t npes);
void shmem_ctx_fence(shmem_ctx_t ctx);
void shmem_ctx_quiet(shmem_ctx_t ctx);
void shmem_ctx_pe_quiet(shmem_ctx_t ctx, const int *target_pes, size_t npes);

/* Collective routines (§9.10). §5.6: barrier_all includes a quiet on the
 * default context; sync does not. */
void shmem_barrier_all(void);
void shmem_sync_all(void);
int shmem_team_sync(shmem_team_t team);

/* The deprecated active-set forms (Annex F), over the whole job only (above):
 * shmem_barrier is shmem_barrier_all and shmem_sync is shmem_sync_all. The spec's
 * C11 shmem_sync(team) shares the second's name; C11 counts the arguments to
 * tell them apart (with the generics, below) and C++ overloads (at the end). */
void shmem_barrier(int PE_start, int logPE_stride, int PE_size, long *pSync);
void shmem_sync(int PE_start, int logPE_stride, int PE_size, long *pSync);

/*
 * Reductions (§9.10.9), team-based, over Table 10's types by operator: the
 * bitwise operators on the unsigned and exact-width integer types; the ordered
 * operators on those, the signed integer types and the floating types; the
 * arithmetic operators on those and, where the C translation environment has
 * them, the two complex types (the spec permits leaving them out otherwise,
 * and C++ has no _Complex). Each returns 0 on success.
 */
#define OSOE_REDUCE_BITWISE_TYPES(X)                                         \
    X(unsigned char, uchar) X(unsigned short, ushort) X(unsigned int, uint)  \
    X(unsigned long, ulong) X(unsigned long long, ulonglong)                 \
    X(int8_t, int8) X(int16_t, int16) X(int32_t, int32) X(int64_t, int64)    \
    X(uint8_t, uint8) X(uint16_t, uint16) X(uint32_t, uint32)                \
    X(uint64_t, uint64) X(size_t, size)

#define OSOE_REDUCE_INTEGER_TYPES(X)                                         \
    X(char, char) X(signed char, schar) X(short, short) X(int, int)          \
    X(long, long) X(long long, longlong) X(ptrdiff_t, ptrdiff)               \
    OSOE_REDUCE_BITWISE_TYPES(X)

#define OSOE_REDUCE_FLOATING_TYPES(X)                                        \
    X(float, float) X(double, double) X(long double, longdouble)

/*
 * C has _Complex; C++ does not, but GCC and Clang both accept it in C++ as an
 * extension with the same layout as C's - defined(__GNUC__) is true for both
 * g++ and clang++ - and SOS's C++ test suite calls shmem_double_sum_reduce and
 * shmem_complexd_sum_reduce and needs both declared. So the guard admits
 * __GNUC__ in C++ too, and excludes only a C++ compiler that does not accept
 * the extension.
 */
#if (!defined(__cplusplus) && !defined(__STDC_NO_COMPLEX__)) ||              \
    (defined(__cplusplus) && defined(__GNUC__))
#define OSOE_REDUCE_COMPLEX_TYPES(X)                                         \
    X(double _Complex, complexd) X(float _Complex, complexf)
#else
#define OSOE_REDUCE_COMPLEX_TYPES(X)
#endif

/* The whole routine name is pasted as a suffix, never the operator alone:
 * `and`, `or` and `xor` are alternative tokens in C++ and macros under
 * <iso646.h>. Declared with the prefix as a parameter, so that pshmem.h
 * generates the `pshmem_` spellings from the same lists. */
#define OSOE_REDUCE_DECL_ONE_P(P, T, NAME, SUFFIX)                           \
    int OSOE_FN(P, NAME, SUFFIX)(shmem_team_t team, T *dest, const T *source, \
                                size_t nreduce);
#define OSOE_REDUCE_DECL_BITWISE_P(P, T, NAME)                               \
    OSOE_REDUCE_DECL_ONE_P(P, T, NAME, _and_reduce)                          \
    OSOE_REDUCE_DECL_ONE_P(P, T, NAME, _or_reduce)                           \
    OSOE_REDUCE_DECL_ONE_P(P, T, NAME, _xor_reduce)
#define OSOE_REDUCE_DECL_ORDERED_P(P, T, NAME)                               \
    OSOE_REDUCE_DECL_ONE_P(P, T, NAME, _max_reduce)                          \
    OSOE_REDUCE_DECL_ONE_P(P, T, NAME, _min_reduce)
#define OSOE_REDUCE_DECL_ARITHMETIC_P(P, T, NAME)                            \
    OSOE_REDUCE_DECL_ONE_P(P, T, NAME, _sum_reduce)                          \
    OSOE_REDUCE_DECL_ONE_P(P, T, NAME, _prod_reduce)

#define OSOE_REDUCE_DECL_BITWISE(T, NAME) OSOE_REDUCE_DECL_BITWISE_P(shmem_, T, NAME)
#define OSOE_REDUCE_DECL_ORDERED(T, NAME) OSOE_REDUCE_DECL_ORDERED_P(shmem_, T, NAME)
#define OSOE_REDUCE_DECL_ARITHMETIC(T, NAME) OSOE_REDUCE_DECL_ARITHMETIC_P(shmem_, T, NAME)

OSOE_REDUCE_BITWISE_TYPES(OSOE_REDUCE_DECL_BITWISE)
OSOE_REDUCE_INTEGER_TYPES(OSOE_REDUCE_DECL_ORDERED)
OSOE_REDUCE_FLOATING_TYPES(OSOE_REDUCE_DECL_ORDERED)
OSOE_REDUCE_INTEGER_TYPES(OSOE_REDUCE_DECL_ARITHMETIC)
OSOE_REDUCE_FLOATING_TYPES(OSOE_REDUCE_DECL_ARITHMETIC)
/* _Complex in a C++ declaration is a GNU extension (above), and -Wpedantic
 * says so even where it changes nothing else about the translation - silenced
 * for exactly this one declaration list rather than for the file, so a real
 * ISO violation elsewhere in either header still fails the strict build. */
#if defined(__cplusplus) && defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
OSOE_REDUCE_COMPLEX_TYPES(OSOE_REDUCE_DECL_ARITHMETIC)
#if defined(__cplusplus) && defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/*
 * The deprecated active-set reductions (Annex F; Table 11), over the whole job
 * only (above): every operator on the four signed integer types, the ordered
 * and arithmetic ones on the three floating types, and sum and product on the
 * two complex types where the translation environment has them.
 */
#define OSOE_TO_ALL_INTEGER_TYPES(X)                                         \
    X(short, short) X(int, int) X(long, long) X(long long, longlong)
#define OSOE_TO_ALL_FLOATING_TYPES(X)                                        \
    X(float, float) X(double, double) X(long double, longdouble)
/* C++ and _Complex: as OSOE_REDUCE_COMPLEX_TYPES above. */
#if (!defined(__cplusplus) && !defined(__STDC_NO_COMPLEX__)) ||              \
    (defined(__cplusplus) && defined(__GNUC__))
#define OSOE_TO_ALL_COMPLEX_TYPES(X)                                         \
    X(double _Complex, complexd) X(float _Complex, complexf)
#else
#define OSOE_TO_ALL_COMPLEX_TYPES(X)
#endif

#define OSOE_TO_ALL_DECL_ONE_P(P, T, NAME, SUFFIX)                           \
    void OSOE_FN(P, NAME, SUFFIX)(T *dest, const T *source, int nreduce,     \
                                  int PE_start, int logPE_stride, int PE_size, \
                                  T *pWrk, long *pSync);
#define OSOE_TO_ALL_DECL_INTEGER_P(P, T, NAME)                               \
    OSOE_TO_ALL_DECL_ONE_P(P, T, NAME, _and_to_all)                          \
    OSOE_TO_ALL_DECL_ONE_P(P, T, NAME, _or_to_all)                           \
    OSOE_TO_ALL_DECL_ONE_P(P, T, NAME, _xor_to_all)                          \
    OSOE_TO_ALL_DECL_FLOATING_P(P, T, NAME)
#define OSOE_TO_ALL_DECL_FLOATING_P(P, T, NAME)                              \
    OSOE_TO_ALL_DECL_ONE_P(P, T, NAME, _max_to_all)                          \
    OSOE_TO_ALL_DECL_ONE_P(P, T, NAME, _min_to_all)                          \
    OSOE_TO_ALL_DECL_COMPLEX_P(P, T, NAME)
#define OSOE_TO_ALL_DECL_COMPLEX_P(P, T, NAME)                               \
    OSOE_TO_ALL_DECL_ONE_P(P, T, NAME, _sum_to_all)                          \
    OSOE_TO_ALL_DECL_ONE_P(P, T, NAME, _prod_to_all)

#define OSOE_TO_ALL_DECL_INTEGER(T, NAME) OSOE_TO_ALL_DECL_INTEGER_P(shmem_, T, NAME)
#define OSOE_TO_ALL_DECL_FLOATING(T, NAME) OSOE_TO_ALL_DECL_FLOATING_P(shmem_, T, NAME)
#define OSOE_TO_ALL_DECL_COMPLEX(T, NAME) OSOE_TO_ALL_DECL_COMPLEX_P(shmem_, T, NAME)

OSOE_TO_ALL_INTEGER_TYPES(OSOE_TO_ALL_DECL_INTEGER)
OSOE_TO_ALL_FLOATING_TYPES(OSOE_TO_ALL_DECL_FLOATING)
/* -Wpedantic and _Complex in C++, as the reductions above. */
#if defined(__cplusplus) && defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
OSOE_TO_ALL_COMPLEX_TYPES(OSOE_TO_ALL_DECL_COMPLEX)
#if defined(__cplusplus) && defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* Broadcast (§9.10.7), team-based, over every standard RMA type of Table 5, and
 * its byte form. The dest is updated on every PE of the team, PE_root's
 * included. Each returns 0 on success. */
#define OSOE_BROADCAST_DECL_P(P, T, NAME)                                    \
    int OSOE_FN(P, NAME, _broadcast)(shmem_team_t team, T *dest, const T *source, \
                                    size_t nelems, int PE_root);
#define OSOE_BROADCAST_DECL(T, NAME) OSOE_BROADCAST_DECL_P(shmem_, T, NAME)
OSOE_RMA_STANDARD_TYPES(OSOE_BROADCAST_DECL)
int shmem_broadcastmem(shmem_team_t team, void *dest, const void *source, size_t nelems,
                       int PE_root);

/* The deprecated active-set broadcasts (Annex F), over the whole job only
 * (above), of four- and eight-byte elements. The root's dest is left as it was:
 * "the dest object is updated on all PEs other than the root PE". */
void shmem_broadcast32(void *dest, const void *source, size_t nelems, int PE_root,
                       int PE_start, int logPE_stride, int PE_size, long *pSync);
void shmem_broadcast64(void *dest, const void *source, size_t nelems, int PE_root,
                       int PE_start, int logPE_stride, int PE_size, long *pSync);

/* Distributed locks (§9.13): first come, first served (lock.c). */
void shmem_set_lock(long *lock);
void shmem_clear_lock(long *lock);
int shmem_test_lock(long *lock);

/* collect and fcollect (§9.10.8), team-based, over every standard RMA type of
 * Table 5, and their byte forms: every PE's block, in team rank order, into
 * every PE's dest - fcollect's blocks all nelems long, collect's each its own
 * PE's. alltoall and alltoalls (§9.10.5, §9.10.6): PE i's block j to PE j's
 * dest block i, with strides in elements for alltoalls. Each returns 0 on
 * success. */
#define OSOE_COLLECT_DECL_P(P, T, NAME)                                       \
    int OSOE_FN(P, NAME, _collect)(shmem_team_t team, T *dest, const T *source, \
                                   size_t nelems);                            \
    int OSOE_FN(P, NAME, _fcollect)(shmem_team_t team, T *dest, const T *source, \
                                    size_t nelems);
#define OSOE_ALLTOALL_DECL_P(P, T, NAME)                                      \
    int OSOE_FN(P, NAME, _alltoall)(shmem_team_t team, T *dest, const T *source, \
                                    size_t nelems);                           \
    int OSOE_FN(P, NAME, _alltoalls)(shmem_team_t team, T *dest, const T *source, \
                                     ptrdiff_t dst, ptrdiff_t sst, size_t nelems);
#define OSOE_COLLECT_DECL(T, NAME) OSOE_COLLECT_DECL_P(shmem_, T, NAME)
#define OSOE_ALLTOALL_DECL(T, NAME) OSOE_ALLTOALL_DECL_P(shmem_, T, NAME)
OSOE_RMA_STANDARD_TYPES(OSOE_COLLECT_DECL)
OSOE_RMA_STANDARD_TYPES(OSOE_ALLTOALL_DECL)
int shmem_collectmem(shmem_team_t team, void *dest, const void *source, size_t nelems);
int shmem_fcollectmem(shmem_team_t team, void *dest, const void *source, size_t nelems);
int shmem_alltoallmem(shmem_team_t team, void *dest, const void *source, size_t nelems);
int shmem_alltoallsmem(shmem_team_t team, void *dest, const void *source, ptrdiff_t dst,
                       ptrdiff_t sst, size_t nelems);

/* Their deprecated active-set forms (Annex F), over the whole job only (above),
 * of four- and eight-byte elements. */
void shmem_collect32(void *dest, const void *source, size_t nelems, int PE_start,
                     int logPE_stride, int PE_size, long *pSync);
void shmem_collect64(void *dest, const void *source, size_t nelems, int PE_start,
                     int logPE_stride, int PE_size, long *pSync);
void shmem_fcollect32(void *dest, const void *source, size_t nelems, int PE_start,
                      int logPE_stride, int PE_size, long *pSync);
void shmem_fcollect64(void *dest, const void *source, size_t nelems, int PE_start,
                      int logPE_stride, int PE_size, long *pSync);
void shmem_alltoall32(void *dest, const void *source, size_t nelems, int PE_start,
                      int logPE_stride, int PE_size, long *pSync);
void shmem_alltoall64(void *dest, const void *source, size_t nelems, int PE_start,
                      int logPE_stride, int PE_size, long *pSync);
void shmem_alltoalls32(void *dest, const void *source, ptrdiff_t dst, ptrdiff_t sst,
                       size_t nelems, int PE_start, int logPE_stride, int PE_size,
                       long *pSync);
void shmem_alltoalls64(void *dest, const void *source, ptrdiff_t dst, ptrdiff_t sst,
                       size_t nelems, int PE_start, int logPE_stride, int PE_size,
                       long *pSync);

/* Library information (§9.1.11, §9.1.12) */
void shmem_info_get_version(int *major, int *minor);
void shmem_info_get_name(char *name);

/* Accessibility queries (§9.1.7, §9.1.8) */
int shmem_pe_accessible(int pe);
int shmem_addr_accessible(const void *addr, int pe);

/*
 * Communication sessions (§9.9). A session is a hint about what a context is
 * about to be used for and "sessions do not affect the completion or ordering
 * semantics of any OpenSHMEM routines", so a library that does nothing with
 * them is conforming. osoe does nothing with them yet: the aggregation they
 * would inform is §3.9's, which arrives with the AM backend.
 */
typedef struct {
    size_t total_ops;
} shmem_ctx_session_config_t;

#define SHMEM_CTX_SESSION_TOTAL_OPS 1L
#define SHMEM_CTX_SESSION_BATCH     1L

void shmem_ctx_session_start(shmem_ctx_t ctx, long options,
                             const shmem_ctx_session_config_t *config, long config_mask);
void shmem_ctx_session_stop(shmem_ctx_t ctx);

/*
 * The deprecated routines (Annex F). They exist because the spec says they
 * must: "support for start_pes is still required in OpenSHMEM libraries".
 * start_pes takes an initialization argument that has meant nothing for
 * decades, and finalizes implicitly at exit, which is the behaviour programs
 * written against it expect.
 */
void start_pes(int npes);
int _my_pe(void);
int _num_pes(void);
void *shmalloc(size_t size);
void shfree(void *ptr);
void *shrealloc(void *ptr, size_t size);
void *shmemalign(size_t alignment, size_t size);

/* SHMEM_CACHE (Annex F.2.7): no-ops on every cache-coherent machine, which is
 * every machine osoe targets. */
void shmem_clear_cache_inv(void);
void shmem_set_cache_inv(void);
void shmem_set_cache_line_inv(void *dest);
void shmem_clear_cache_line_inv(void *dest);
void shmem_udcflush(void);
void shmem_udcflush_line(void *dest);

/*
 * The deprecated atomic memory operations (Annex F.2.12): the pre-§9.7 names,
 * one routine per (type, operation) instead of the modern
 * shmem_TYPENAME_atomic_* names. deprecated.c makes each exactly its modern
 * routine under its old name, so declaring both from one list here (as the
 * AMO families above) keeps the two from disagreeing about which types exist.
 *
 * The deprecated names never covered all of Tables 6 and 7, only a subset:
 * fetch, set and swap take the three plain integer types of Table 6 plus the
 * float and double Table 7 adds; cswap, finc, inc, fadd and add take the
 * three plain integer types only. There are no context forms - the spec never
 * gave these routines one - and no exact-width, unsigned or size/ptrdiff
 * types either.
 */
#define OSOE_AMO_DEPRECATED_TYPES(X) X(int, int) X(long, long) X(long long, longlong)

/* fetch, set and swap add float and double, as Table 7 adds them to Table 6. */
#define OSOE_AMO_DEPRECATED_EXTENDED_TYPES(X)                                \
    OSOE_AMO_DEPRECATED_TYPES(X) X(float, float) X(double, double)

#define OSOE_AMO_DECL_DEPRECATED_EXTENDED_P(P, T, NAME)                      \
    T OSOE_FN(P, NAME, _fetch)(const T *source, int pe);                    \
    void OSOE_FN(P, NAME, _set)(T *dest, T value, int pe);                  \
    T OSOE_FN(P, NAME, _swap)(T *dest, T value, int pe);

#define OSOE_AMO_DECL_DEPRECATED_STANDARD_P(P, T, NAME)                      \
    T OSOE_FN(P, NAME, _cswap)(T *dest, T cond, T value, int pe);           \
    T OSOE_FN(P, NAME, _finc)(T *dest, int pe);                             \
    void OSOE_FN(P, NAME, _inc)(T *dest, int pe);                           \
    T OSOE_FN(P, NAME, _fadd)(T *dest, T value, int pe);                    \
    void OSOE_FN(P, NAME, _add)(T *dest, T value, int pe);

#define OSOE_AMO_DECL_DEPRECATED_EXTENDED(T, NAME)                           \
    OSOE_AMO_DECL_DEPRECATED_EXTENDED_P(shmem_, T, NAME)
#define OSOE_AMO_DECL_DEPRECATED_STANDARD(T, NAME)                           \
    OSOE_AMO_DECL_DEPRECATED_STANDARD_P(shmem_, T, NAME)

OSOE_AMO_DEPRECATED_EXTENDED_TYPES(OSOE_AMO_DECL_DEPRECATED_EXTENDED)
OSOE_AMO_DEPRECATED_TYPES(OSOE_AMO_DECL_DEPRECATED_STANDARD)

/*
 * C11 type-generic interfaces (§7.4; spec §9.7).
 *
 * `_Generic` selects on the *type* of the pointer, so each association must
 * name a distinct C type. The exact-width types of Tables 6-8 are typedefs of
 * the canonical types on LP64 Linux - int32_t is int, uint64_t is unsigned
 * long, size_t is unsigned long, and so on - so listing both spellings would be
 * two associations for one type, which does not compile. The lists below are
 * therefore one entry per distinct type, and the library's build asserts that
 * each typedef really is the type assumed here.
 *
 * Each list emits a *leading* comma per entry, so `_Generic((dest) LIST)` is
 * well formed and there is no `default:` association. That is deliberate: a
 * type outside the table is then a compile error naming the call, rather than
 * being quietly routed to some other type's routine.
 *
 * The OSOE_-prefixed macros here are this implementation's, not part of the
 * API. They stay defined because the generic macros expand them at the point of
 * use.
 */
#if !defined(__cplusplus) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L

#define OSOE_AMO_G(T, NAME, OP)  , T *: shmem_##NAME##_atomic_##OP
/* For shmem_atomic_fetch, whose argument may be const-qualified. */
#define OSOE_AMO_GC(T, NAME, OP) , T *: shmem_##NAME##_atomic_##OP                \
                                 , const T *: shmem_##NAME##_atomic_##OP
/* The same, for the context-taking spellings. */
#define OSOE_AMO_GX(T, NAME, OP)  , T *: shmem_ctx_##NAME##_atomic_##OP
#define OSOE_AMO_GCX(T, NAME, OP) , T *: shmem_ctx_##NAME##_atomic_##OP           \
                                  , const T *: shmem_ctx_##NAME##_atomic_##OP

/*
 * Every AMO has two C11 spellings, with and without a leading context
 * (spec §9.7): shmem_atomic_add(dest, value, pe) and
 * shmem_atomic_add(ctx, dest, value, pe). `_Generic` cannot tell them apart -
 * they differ in arity, not in the type of one argument - so the count is
 * counted first and the selection is made on whichever argument is the target
 * pointer. Each selector below names the routines with N arguments and the ones
 * with N + 1, and lets the argument list pick.
 */
#define OSOE_PICK3(_1, _2, _3, NAME, ...) NAME
#define OSOE_PICK4(_1, _2, _3, _4, NAME, ...) NAME
#define OSOE_PICK5(_1, _2, _3, _4, _5, NAME, ...) NAME
/* shmem_atomic_compare_swap_nbi (5 or 6 arguments) needs this one. */
#define OSOE_PICK6(_1, _2, _3, _4, _5, _6, NAME, ...) NAME
/* iput/iget (6 or 7 arguments) and put_signal (7 or 8) need two wider ones. */
#define OSOE_PICK7(_1, _2, _3, _4, _5, _6, _7, NAME, ...) NAME
#define OSOE_PICK8(_1, _2, _3, _4, _5, _6, _7, _8, NAME, ...) NAME

/* Table 6, one entry per distinct C type. */
#define OSOE_AMO_G_STANDARD(X, OP)                                                \
    X(int, int, OP) X(long, long, OP) X(long long, longlong, OP)                  \
    X(unsigned int, uint, OP) X(unsigned long, ulong, OP)                         \
    X(unsigned long long, ulonglong, OP)

/* Table 7: Table 6 plus float and double. */
#define OSOE_AMO_G_EXTENDED(X, OP)                                                \
    OSOE_AMO_G_STANDARD(X, OP) X(float, float, OP) X(double, double, OP)

/* Table 8. int and long appear as int32_t and int64_t; there is no plain
 * signed int or long in the bitwise table, which is why they map to the
 * exact-width routine names. */
#define OSOE_AMO_G_BITWISE(X, OP)                                                 \
    X(int, int32, OP) X(long, int64, OP)                                          \
    X(unsigned int, uint, OP) X(unsigned long, ulong, OP)                         \
    X(unsigned long long, ulonglong, OP)

#define OSOE_G_FETCH_2(source, pe)                                                \
    (_Generic((source) OSOE_AMO_G_EXTENDED(OSOE_AMO_GC, fetch))((source), (pe)))
#define OSOE_G_FETCH_3(ctx, source, pe)                                           \
    (_Generic((source) OSOE_AMO_G_EXTENDED(OSOE_AMO_GCX, fetch))((ctx), (source), (pe)))
#define shmem_atomic_fetch(...)                                                   \
    OSOE_PICK3(__VA_ARGS__, OSOE_G_FETCH_3, OSOE_G_FETCH_2, )(__VA_ARGS__)
#define OSOE_G_SET_3(dest, value, pe)                                             \
    (_Generic((dest) OSOE_AMO_G_EXTENDED(OSOE_AMO_G, set))((dest), (value), (pe)))
#define OSOE_G_SET_4(ctx, dest, value, pe)                                        \
    (_Generic((dest) OSOE_AMO_G_EXTENDED(OSOE_AMO_GX, set))((ctx), (dest), (value), (pe)))
#define shmem_atomic_set(...)                                                     \
    OSOE_PICK4(__VA_ARGS__, OSOE_G_SET_4, OSOE_G_SET_3, , )(__VA_ARGS__)
#define OSOE_G_SWAP_3(dest, value, pe)                                            \
    (_Generic((dest) OSOE_AMO_G_EXTENDED(OSOE_AMO_G, swap))((dest), (value), (pe)))
#define OSOE_G_SWAP_4(ctx, dest, value, pe)                                       \
    (_Generic((dest) OSOE_AMO_G_EXTENDED(OSOE_AMO_GX, swap))((ctx), (dest), (value), (pe)))
#define shmem_atomic_swap(...)                                                    \
    OSOE_PICK4(__VA_ARGS__, OSOE_G_SWAP_4, OSOE_G_SWAP_3, , )(__VA_ARGS__)

#define OSOE_G_CSWAP_4(dest, cond, value, pe)                                     \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_G, compare_swap))               \
        ((dest), (cond), (value), (pe)))
#define OSOE_G_CSWAP_5(ctx, dest, cond, value, pe)                                \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_GX, compare_swap))              \
        ((ctx), (dest), (cond), (value), (pe)))
#define shmem_atomic_compare_swap(...)                                            \
    OSOE_PICK5(__VA_ARGS__, OSOE_G_CSWAP_5, OSOE_G_CSWAP_4, , , )(__VA_ARGS__)
#define OSOE_G_INC_2(dest, pe)                                                    \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_G, inc))((dest), (pe)))
#define OSOE_G_INC_3(ctx, dest, pe)                                               \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_GX, inc))((ctx), (dest), (pe)))
#define shmem_atomic_inc(...)                                                     \
    OSOE_PICK3(__VA_ARGS__, OSOE_G_INC_3, OSOE_G_INC_2, )(__VA_ARGS__)
#define OSOE_G_FINC_2(dest, pe)                                                   \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_G, fetch_inc))((dest), (pe)))
#define OSOE_G_FINC_3(ctx, dest, pe)                                              \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_GX, fetch_inc))((ctx), (dest), (pe)))
#define shmem_atomic_fetch_inc(...)                                               \
    OSOE_PICK3(__VA_ARGS__, OSOE_G_FINC_3, OSOE_G_FINC_2, )(__VA_ARGS__)
#define OSOE_G_ADD_3(dest, value, pe)                                             \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_G, add))((dest), (value), (pe)))
#define OSOE_G_ADD_4(ctx, dest, value, pe)                                        \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_GX, add))((ctx), (dest), (value), (pe)))
#define shmem_atomic_add(...)                                                     \
    OSOE_PICK4(__VA_ARGS__, OSOE_G_ADD_4, OSOE_G_ADD_3, , )(__VA_ARGS__)
#define OSOE_G_FADD_3(dest, value, pe)                                            \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_G, fetch_add))                  \
        ((dest), (value), (pe)))
#define OSOE_G_FADD_4(ctx, dest, value, pe)                                       \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_GX, fetch_add))                 \
        ((ctx), (dest), (value), (pe)))
#define shmem_atomic_fetch_add(...)                                               \
    OSOE_PICK4(__VA_ARGS__, OSOE_G_FADD_4, OSOE_G_FADD_3, , )(__VA_ARGS__)

#define OSOE_G_AND_3(dest, value, pe)                                         \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_G, and))((dest), (value), (pe)))
#define OSOE_G_AND_4(ctx, dest, value, pe)                                    \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_GX, and))                    \
        ((ctx), (dest), (value), (pe)))
#define shmem_atomic_and(...)                                                 \
    OSOE_PICK4(__VA_ARGS__, OSOE_G_AND_4, OSOE_G_AND_3, , )(__VA_ARGS__)
#define OSOE_G_FAND_3(dest, value, pe)                                         \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_G, fetch_and))((dest), (value), (pe)))
#define OSOE_G_FAND_4(ctx, dest, value, pe)                                    \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_GX, fetch_and))                    \
        ((ctx), (dest), (value), (pe)))
#define shmem_atomic_fetch_and(...)                                                 \
    OSOE_PICK4(__VA_ARGS__, OSOE_G_FAND_4, OSOE_G_FAND_3, , )(__VA_ARGS__)
#define OSOE_G_OR_3(dest, value, pe)                                         \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_G, or))((dest), (value), (pe)))
#define OSOE_G_OR_4(ctx, dest, value, pe)                                    \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_GX, or))                    \
        ((ctx), (dest), (value), (pe)))
#define shmem_atomic_or(...)                                                 \
    OSOE_PICK4(__VA_ARGS__, OSOE_G_OR_4, OSOE_G_OR_3, , )(__VA_ARGS__)
#define OSOE_G_FOR_3(dest, value, pe)                                         \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_G, fetch_or))((dest), (value), (pe)))
#define OSOE_G_FOR_4(ctx, dest, value, pe)                                    \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_GX, fetch_or))                    \
        ((ctx), (dest), (value), (pe)))
#define shmem_atomic_fetch_or(...)                                                 \
    OSOE_PICK4(__VA_ARGS__, OSOE_G_FOR_4, OSOE_G_FOR_3, , )(__VA_ARGS__)
#define OSOE_G_XOR_3(dest, value, pe)                                         \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_G, xor))((dest), (value), (pe)))
#define OSOE_G_XOR_4(ctx, dest, value, pe)                                    \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_GX, xor))                    \
        ((ctx), (dest), (value), (pe)))
#define shmem_atomic_xor(...)                                                 \
    OSOE_PICK4(__VA_ARGS__, OSOE_G_XOR_4, OSOE_G_XOR_3, , )(__VA_ARGS__)
#define OSOE_G_FXOR_3(dest, value, pe)                                         \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_G, fetch_xor))((dest), (value), (pe)))
#define OSOE_G_FXOR_4(ctx, dest, value, pe)                                    \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_GX, fetch_xor))                    \
        ((ctx), (dest), (value), (pe)))
#define shmem_atomic_fetch_xor(...)                                                 \
    OSOE_PICK4(__VA_ARGS__, OSOE_G_FXOR_4, OSOE_G_FXOR_3, , )(__VA_ARGS__)

/*
 * The nonblocking fetching AMOs' C11 spellings (§9.7.2), built the same way:
 * one selector per operation, picking on argument count between the plain and
 * ctx forms, and within each on the type of the pointer the blocking form
 * dispatches on - *source for fetch_nbi, *dest for the rest. compare_swap_nbi's
 * two forms are five and six arguments, one wider than any blocking AMO
 * reaches, hence OSOE_PICK6 above.
 */
#define OSOE_G_FETCH_NBI_3(fetch, source, pe)                                    \
    (_Generic((source) OSOE_AMO_G_EXTENDED(OSOE_AMO_GC, fetch_nbi))              \
        ((fetch), (source), (pe)))
#define OSOE_G_FETCH_NBI_4(ctx, fetch, source, pe)                               \
    (_Generic((source) OSOE_AMO_G_EXTENDED(OSOE_AMO_GCX, fetch_nbi))             \
        ((ctx), (fetch), (source), (pe)))
#define shmem_atomic_fetch_nbi(...)                                              \
    OSOE_PICK4(__VA_ARGS__, OSOE_G_FETCH_NBI_4, OSOE_G_FETCH_NBI_3, , )(__VA_ARGS__)

#define OSOE_G_SWAP_NBI_4(fetch, dest, value, pe)                                \
    (_Generic((dest) OSOE_AMO_G_EXTENDED(OSOE_AMO_G, swap_nbi))                  \
        ((fetch), (dest), (value), (pe)))
#define OSOE_G_SWAP_NBI_5(ctx, fetch, dest, value, pe)                           \
    (_Generic((dest) OSOE_AMO_G_EXTENDED(OSOE_AMO_GX, swap_nbi))                 \
        ((ctx), (fetch), (dest), (value), (pe)))
#define shmem_atomic_swap_nbi(...)                                               \
    OSOE_PICK5(__VA_ARGS__, OSOE_G_SWAP_NBI_5, OSOE_G_SWAP_NBI_4, , , )(__VA_ARGS__)

#define OSOE_G_CSWAP_NBI_5(fetch, dest, cond, value, pe)                         \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_G, compare_swap_nbi))          \
        ((fetch), (dest), (cond), (value), (pe)))
#define OSOE_G_CSWAP_NBI_6(ctx, fetch, dest, cond, value, pe)                    \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_GX, compare_swap_nbi))         \
        ((ctx), (fetch), (dest), (cond), (value), (pe)))
#define shmem_atomic_compare_swap_nbi(...)                                       \
    OSOE_PICK6(__VA_ARGS__, OSOE_G_CSWAP_NBI_6, OSOE_G_CSWAP_NBI_5, , , , )(__VA_ARGS__)

#define OSOE_G_FINC_NBI_3(fetch, dest, pe)                                       \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_G, fetch_inc_nbi))             \
        ((fetch), (dest), (pe)))
#define OSOE_G_FINC_NBI_4(ctx, fetch, dest, pe)                                  \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_GX, fetch_inc_nbi))            \
        ((ctx), (fetch), (dest), (pe)))
#define shmem_atomic_fetch_inc_nbi(...)                                          \
    OSOE_PICK4(__VA_ARGS__, OSOE_G_FINC_NBI_4, OSOE_G_FINC_NBI_3, , )(__VA_ARGS__)

#define OSOE_G_FADD_NBI_4(fetch, dest, value, pe)                                \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_G, fetch_add_nbi))             \
        ((fetch), (dest), (value), (pe)))
#define OSOE_G_FADD_NBI_5(ctx, fetch, dest, value, pe)                           \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_GX, fetch_add_nbi))            \
        ((ctx), (fetch), (dest), (value), (pe)))
#define shmem_atomic_fetch_add_nbi(...)                                          \
    OSOE_PICK5(__VA_ARGS__, OSOE_G_FADD_NBI_5, OSOE_G_FADD_NBI_4, , , )(__VA_ARGS__)

#define OSOE_G_FAND_NBI_4(fetch, dest, value, pe)                                \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_G, fetch_and_nbi))              \
        ((fetch), (dest), (value), (pe)))
#define OSOE_G_FAND_NBI_5(ctx, fetch, dest, value, pe)                           \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_GX, fetch_and_nbi))             \
        ((ctx), (fetch), (dest), (value), (pe)))
#define shmem_atomic_fetch_and_nbi(...)                                          \
    OSOE_PICK5(__VA_ARGS__, OSOE_G_FAND_NBI_5, OSOE_G_FAND_NBI_4, , , )(__VA_ARGS__)

#define OSOE_G_FOR_NBI_4(fetch, dest, value, pe)                                 \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_G, fetch_or_nbi))               \
        ((fetch), (dest), (value), (pe)))
#define OSOE_G_FOR_NBI_5(ctx, fetch, dest, value, pe)                            \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_GX, fetch_or_nbi))              \
        ((ctx), (fetch), (dest), (value), (pe)))
#define shmem_atomic_fetch_or_nbi(...)                                           \
    OSOE_PICK5(__VA_ARGS__, OSOE_G_FOR_NBI_5, OSOE_G_FOR_NBI_4, , , )(__VA_ARGS__)

#define OSOE_G_FXOR_NBI_4(fetch, dest, value, pe)                                \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_G, fetch_xor_nbi))              \
        ((fetch), (dest), (value), (pe)))
#define OSOE_G_FXOR_NBI_5(ctx, fetch, dest, value, pe)                           \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_GX, fetch_xor_nbi))             \
        ((ctx), (fetch), (dest), (value), (pe)))
#define shmem_atomic_fetch_xor_nbi(...)                                          \
    OSOE_PICK5(__VA_ARGS__, OSOE_G_FXOR_NBI_5, OSOE_G_FXOR_NBI_4, , , )(__VA_ARGS__)

/*
 * The deprecated C11 generic spellings (Annex F.2.12): shmem_fetch, shmem_set,
 * shmem_swap, shmem_cswap, shmem_finc, shmem_inc, shmem_fadd and shmem_add,
 * dispatching on the type of the pointer argument to the deprecated
 * TYPENAME-spelled routine above rather than to the modern shmem_TYPENAME_
 * atomic_* one - the deprecated C/C++ name is the modern routine under its
 * old name (deprecated.c), and the deprecated C11 name only has to reach that
 * same old name. There are no context forms and so no arity to pick between,
 * unlike shmem_atomic_* above.
 */
#define OSOE_AMO_G_DEPRECATED(T, NAME, OP)  , T *: shmem_##NAME##_##OP
/* For shmem_fetch, whose argument may be const-qualified. */
#define OSOE_AMO_GC_DEPRECATED(T, NAME, OP) , T *: shmem_##NAME##_##OP            \
                                            , const T *: shmem_##NAME##_##OP

/* The three-type and five-type lists above, as association lists (T, NAME, OP). */
#define OSOE_AMO_G_DEPRECATED_STANDARD(X, OP)                                    \
    X(int, int, OP) X(long, long, OP) X(long long, longlong, OP)
#define OSOE_AMO_G_DEPRECATED_EXTENDED(X, OP)                                    \
    OSOE_AMO_G_DEPRECATED_STANDARD(X, OP) X(float, float, OP) X(double, double, OP)

#define shmem_fetch(source, pe)                                                  \
    (_Generic((source) OSOE_AMO_G_DEPRECATED_EXTENDED(OSOE_AMO_GC_DEPRECATED, fetch)) \
        ((source), (pe)))
#define shmem_set(dest, value, pe)                                               \
    (_Generic((dest) OSOE_AMO_G_DEPRECATED_EXTENDED(OSOE_AMO_G_DEPRECATED, set))  \
        ((dest), (value), (pe)))
#define shmem_swap(dest, value, pe)                                              \
    (_Generic((dest) OSOE_AMO_G_DEPRECATED_EXTENDED(OSOE_AMO_G_DEPRECATED, swap)) \
        ((dest), (value), (pe)))
#define shmem_cswap(dest, cond, value, pe)                                       \
    (_Generic((dest) OSOE_AMO_G_DEPRECATED_STANDARD(OSOE_AMO_G_DEPRECATED, cswap)) \
        ((dest), (cond), (value), (pe)))
#define shmem_finc(dest, pe)                                                     \
    (_Generic((dest) OSOE_AMO_G_DEPRECATED_STANDARD(OSOE_AMO_G_DEPRECATED, finc)) \
        ((dest), (pe)))
#define shmem_inc(dest, pe)                                                      \
    (_Generic((dest) OSOE_AMO_G_DEPRECATED_STANDARD(OSOE_AMO_G_DEPRECATED, inc))  \
        ((dest), (pe)))
#define shmem_fadd(dest, value, pe)                                              \
    (_Generic((dest) OSOE_AMO_G_DEPRECATED_STANDARD(OSOE_AMO_G_DEPRECATED, fadd)) \
        ((dest), (value), (pe)))
#define shmem_add(dest, value, pe)                                               \
    (_Generic((dest) OSOE_AMO_G_DEPRECATED_STANDARD(OSOE_AMO_G_DEPRECATED, add))  \
        ((dest), (value), (pe)))

/* ---- Remote memory access (spec §9.6, §9.8.3) ------------------------- */

/*
 * Table 5, the fourteen distinct C types (as OSOE_AMO_G_STANDARD above): the
 * ten aliased TYPENAMEs of §9.6's declarations are typedefs of one of these
 * fourteen on this implementation's targets, so a fifteenth `_Generic`
 * association for one of them would be a duplicate case rather than a
 * fifteenth type.
 */
#define OSOE_RMA_G_STANDARD(X, OP)                                               \
    X(float, float, OP) X(double, double, OP) X(long double, longdouble, OP)     \
    X(char, char, OP) X(signed char, schar, OP) X(short, short, OP)              \
    X(int, int, OP) X(long, long, OP) X(long long, longlong, OP)                 \
    X(unsigned char, uchar, OP) X(unsigned short, ushort, OP)                    \
    X(unsigned int, uint, OP) X(unsigned long, ulong, OP)                        \
    X(unsigned long long, ulonglong, OP)

#define OSOE_RMA_G(T, NAME, OP)   , T *: shmem_##NAME##_##OP
/* For shmem_g, whose argument may be const-qualified, as OSOE_AMO_GC. */
#define OSOE_RMA_GC(T, NAME, OP)  , T *: shmem_##NAME##_##OP                     \
                                  , const T *: shmem_##NAME##_##OP
/* The same, for the context-taking spellings. */
#define OSOE_RMA_GX(T, NAME, OP)  , T *: shmem_ctx_##NAME##_##OP
#define OSOE_RMA_GCX(T, NAME, OP) , T *: shmem_ctx_##NAME##_##OP                 \
                                  , const T *: shmem_ctx_##NAME##_##OP

/*
 * Every routine dispatches on `dest` - the local buffer for get/iget/g, the
 * symmetric one for put/iput/p/put_signal - which Table 5's C11 SYNOPSIS
 * always types as plain `TYPE *`, never const. shmem_g is the one exception:
 * its only pointer argument is `source`, typed `const TYPE *`, so it dispatches
 * on that instead and needs both the const and non-const associations (OSOE_
 * RMA_GC/GCX), exactly as shmem_atomic_fetch does for the same reason.
 */
#define OSOE_G_PUT_4(dest, source, nelems, pe)                                   \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_G, put))((dest), (source), (nelems), (pe)))
#define OSOE_G_PUT_5(ctx, dest, source, nelems, pe)                              \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_GX, put))                      \
        ((ctx), (dest), (source), (nelems), (pe)))
#define shmem_put(...)                                                           \
    OSOE_PICK5(__VA_ARGS__, OSOE_G_PUT_5, OSOE_G_PUT_4, , , )(__VA_ARGS__)

#define OSOE_G_GET_4(dest, source, nelems, pe)                                   \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_G, get))((dest), (source), (nelems), (pe)))
#define OSOE_G_GET_5(ctx, dest, source, nelems, pe)                              \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_GX, get))                      \
        ((ctx), (dest), (source), (nelems), (pe)))
#define shmem_get(...)                                                           \
    OSOE_PICK5(__VA_ARGS__, OSOE_G_GET_5, OSOE_G_GET_4, , , )(__VA_ARGS__)

#define OSOE_G_P_3(dest, value, pe)                                              \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_G, p))((dest), (value), (pe)))
#define OSOE_G_P_4(ctx, dest, value, pe)                                         \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_GX, p))((ctx), (dest), (value), (pe)))
#define shmem_p(...)                                                             \
    OSOE_PICK4(__VA_ARGS__, OSOE_G_P_4, OSOE_G_P_3, , )(__VA_ARGS__)

#define OSOE_G_G_2(source, pe)                                                   \
    (_Generic((source) OSOE_RMA_G_STANDARD(OSOE_RMA_GC, g))((source), (pe)))
#define OSOE_G_G_3(ctx, source, pe)                                              \
    (_Generic((source) OSOE_RMA_G_STANDARD(OSOE_RMA_GCX, g))((ctx), (source), (pe)))
#define shmem_g(...)                                                             \
    OSOE_PICK3(__VA_ARGS__, OSOE_G_G_3, OSOE_G_G_2, )(__VA_ARGS__)

#define OSOE_G_IPUT_6(dest, source, dst, sst, nelems, pe)                        \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_G, iput))                      \
        ((dest), (source), (dst), (sst), (nelems), (pe)))
#define OSOE_G_IPUT_7(ctx, dest, source, dst, sst, nelems, pe)                   \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_GX, iput))                     \
        ((ctx), (dest), (source), (dst), (sst), (nelems), (pe)))
#define shmem_iput(...)                                                          \
    OSOE_PICK7(__VA_ARGS__, OSOE_G_IPUT_7, OSOE_G_IPUT_6, , , , , )(__VA_ARGS__)

#define OSOE_G_IGET_6(dest, source, dst, sst, nelems, pe)                        \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_G, iget))                      \
        ((dest), (source), (dst), (sst), (nelems), (pe)))
#define OSOE_G_IGET_7(ctx, dest, source, dst, sst, nelems, pe)                   \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_GX, iget))                     \
        ((ctx), (dest), (source), (dst), (sst), (nelems), (pe)))
#define shmem_iget(...)                                                          \
    OSOE_PICK7(__VA_ARGS__, OSOE_G_IGET_7, OSOE_G_IGET_6, , , , , )(__VA_ARGS__)

#define OSOE_G_PUT_NBI_4(dest, source, nelems, pe)                               \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_G, put_nbi))                   \
        ((dest), (source), (nelems), (pe)))
#define OSOE_G_PUT_NBI_5(ctx, dest, source, nelems, pe)                          \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_GX, put_nbi))                  \
        ((ctx), (dest), (source), (nelems), (pe)))
#define shmem_put_nbi(...)                                                       \
    OSOE_PICK5(__VA_ARGS__, OSOE_G_PUT_NBI_5, OSOE_G_PUT_NBI_4, , , )(__VA_ARGS__)

#define OSOE_G_GET_NBI_4(dest, source, nelems, pe)                               \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_G, get_nbi))                   \
        ((dest), (source), (nelems), (pe)))
#define OSOE_G_GET_NBI_5(ctx, dest, source, nelems, pe)                          \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_GX, get_nbi))                  \
        ((ctx), (dest), (source), (nelems), (pe)))
#define shmem_get_nbi(...)                                                       \
    OSOE_PICK5(__VA_ARGS__, OSOE_G_GET_NBI_5, OSOE_G_GET_NBI_4, , , )(__VA_ARGS__)

/* The two signalling routines the spec gives a C11 generic SYNOPSIS (§9.8.3,
 * §9.8.4). */
#define OSOE_G_PUT_SIGNAL_7(dest, source, nelems, sig_addr, signal, sig_op, pe)  \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_G, put_signal))                \
        ((dest), (source), (nelems), (sig_addr), (signal), (sig_op), (pe)))
#define OSOE_G_PUT_SIGNAL_8(ctx, dest, source, nelems, sig_addr, signal, sig_op, pe) \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_GX, put_signal))               \
        ((ctx), (dest), (source), (nelems), (sig_addr), (signal), (sig_op), (pe)))
#define shmem_put_signal(...)                                                    \
    OSOE_PICK8(__VA_ARGS__, OSOE_G_PUT_SIGNAL_8, OSOE_G_PUT_SIGNAL_7, , , , , , )(__VA_ARGS__)

#define OSOE_G_PUT_SIGNAL_NBI_7(dest, source, nelems, sig_addr, signal, sig_op, pe) \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_G, put_signal_nbi))            \
        ((dest), (source), (nelems), (sig_addr), (signal), (sig_op), (pe)))
#define OSOE_G_PUT_SIGNAL_NBI_8(ctx, dest, source, nelems, sig_addr, signal, sig_op, pe) \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_GX, put_signal_nbi))           \
        ((ctx), (dest), (source), (nelems), (sig_addr), (signal), (sig_op), (pe)))
#define shmem_put_signal_nbi(...)                                                \
    OSOE_PICK8(__VA_ARGS__, OSOE_G_PUT_SIGNAL_NBI_8, OSOE_G_PUT_SIGNAL_NBI_7, , , , , , ) \
        (__VA_ARGS__)


/* ---- Collectives (spec §9.10.7, §9.10.9) ------------------------------ */

/*
 * The reductions' C11 interfaces dispatch on dest's type over the distinct C
 * types of Table 10's operator groups. The exact-width and size types are
 * typedefs of these on this implementation's targets (LP64 Linux), so the
 * bitwise group reaches its signed exact-width members through their base
 * types: an int* is int32's, a long* int64's. A type an operator does not take
 * - a long long* to shmem_and_reduce - has no association and does not compile.
 */
#define OSOE_REDUCE_G_BITWISE(X, SUFFIX)                                         \
    X(unsigned char, uchar, SUFFIX) X(unsigned short, ushort, SUFFIX)            \
    X(unsigned int, uint, SUFFIX) X(unsigned long, ulong, SUFFIX)                \
    X(unsigned long long, ulonglong, SUFFIX) X(signed char, int8, SUFFIX)        \
    X(short, int16, SUFFIX) X(int, int32, SUFFIX) X(long, int64, SUFFIX)
#define OSOE_REDUCE_G_ORDERED(X, SUFFIX)                                         \
    X(char, char, SUFFIX) X(signed char, schar, SUFFIX) X(short, short, SUFFIX)  \
    X(int, int, SUFFIX) X(long, long, SUFFIX) X(long long, longlong, SUFFIX)     \
    X(unsigned char, uchar, SUFFIX) X(unsigned short, ushort, SUFFIX)            \
    X(unsigned int, uint, SUFFIX) X(unsigned long, ulong, SUFFIX)                \
    X(unsigned long long, ulonglong, SUFFIX) X(float, float, SUFFIX)             \
    X(double, double, SUFFIX) X(long double, longdouble, SUFFIX)
#if !defined(__STDC_NO_COMPLEX__)
#define OSOE_REDUCE_G_ARITHMETIC(X, SUFFIX)                                      \
    OSOE_REDUCE_G_ORDERED(X, SUFFIX)                                             \
    X(double _Complex, complexd, SUFFIX) X(float _Complex, complexf, SUFFIX)
#else
#define OSOE_REDUCE_G_ARITHMETIC(X, SUFFIX) OSOE_REDUCE_G_ORDERED(X, SUFFIX)
#endif
#define OSOE_REDUCE_G(T, NAME, SUFFIX) , T *: OSOE_FN(shmem_, NAME, SUFFIX)

#define shmem_and_reduce(team, dest, source, nreduce)                            \
    (_Generic((dest) OSOE_REDUCE_G_BITWISE(OSOE_REDUCE_G, _and_reduce))          \
        ((team), (dest), (source), (nreduce)))
#define shmem_or_reduce(team, dest, source, nreduce)                             \
    (_Generic((dest) OSOE_REDUCE_G_BITWISE(OSOE_REDUCE_G, _or_reduce))           \
        ((team), (dest), (source), (nreduce)))
#define shmem_xor_reduce(team, dest, source, nreduce)                            \
    (_Generic((dest) OSOE_REDUCE_G_BITWISE(OSOE_REDUCE_G, _xor_reduce))          \
        ((team), (dest), (source), (nreduce)))
#define shmem_max_reduce(team, dest, source, nreduce)                            \
    (_Generic((dest) OSOE_REDUCE_G_ORDERED(OSOE_REDUCE_G, _max_reduce))          \
        ((team), (dest), (source), (nreduce)))
#define shmem_min_reduce(team, dest, source, nreduce)                            \
    (_Generic((dest) OSOE_REDUCE_G_ORDERED(OSOE_REDUCE_G, _min_reduce))          \
        ((team), (dest), (source), (nreduce)))
#define shmem_sum_reduce(team, dest, source, nreduce)                            \
    (_Generic((dest) OSOE_REDUCE_G_ARITHMETIC(OSOE_REDUCE_G, _sum_reduce))       \
        ((team), (dest), (source), (nreduce)))
#define shmem_prod_reduce(team, dest, source, nreduce)                           \
    (_Generic((dest) OSOE_REDUCE_G_ARITHMETIC(OSOE_REDUCE_G, _prod_reduce))      \
        ((team), (dest), (source), (nreduce)))

/* Broadcast dispatches on dest over Table 5's distinct types, as put does. */
#define shmem_broadcast(team, dest, source, nelems, PE_root)                     \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_G, broadcast))                 \
        ((team), (dest), (source), (nelems), (PE_root)))

/* So do collect, fcollect, alltoall and alltoalls. */
#define shmem_collect(team, dest, source, nelems)                                \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_G, collect))                   \
        ((team), (dest), (source), (nelems)))
#define shmem_fcollect(team, dest, source, nelems)                               \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_G, fcollect))                  \
        ((team), (dest), (source), (nelems)))
#define shmem_alltoall(team, dest, source, nelems)                               \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_G, alltoall))                  \
        ((team), (dest), (source), (nelems)))
#define shmem_alltoalls(team, dest, source, dst, sst, nelems)                    \
    (_Generic((dest) OSOE_RMA_G_STANDARD(OSOE_RMA_G, alltoalls))                 \
        ((team), (dest), (source), (dst), (sst), (nelems)))

/* spec §9.10.3's shmem_sync(team), beside the deprecated active-set shmem_sync
 * of four arguments (declared above): the count picks, as it does between the
 * forms with and without a context, and the deprecated one is called by its
 * parenthesized name, which no function-like macro expands. Two or three
 * arguments name an identifier nothing declares, so the mistake does not
 * compile - where an empty choice would have compiled to a sync that never
 * happened. */
#define OSOE_SYNC_1(team) shmem_team_sync(team)
#define OSOE_SYNC_4(PE_start, logPE_stride, PE_size, pSync)                      \
    (shmem_sync)((PE_start), (logPE_stride), (PE_size), (pSync))
#define shmem_sync(...)                                                          \
    OSOE_PICK4(__VA_ARGS__, OSOE_SYNC_4, osoe_shmem_sync_takes_one_or_four_arguments,  \
               osoe_shmem_sync_takes_one_or_four_arguments, OSOE_SYNC_1, )(__VA_ARGS__)


/* ---- Point-to-point synchronization (spec §9.11) --------------------- */

/* Table 6, one entry per distinct C type, as above. */
#define OSOE_WAIT_G_STANDARD(X, OP)                                               \
    X(int, int, OP) X(long, long, OP) X(long long, longlong, OP)                  \
    X(unsigned int, uint, OP) X(unsigned long, ulong, OP)                         \
    X(unsigned long long, ulonglong, OP)

/* shmem_wait_until and shmem_test also take the deprecated two-byte types. */
#define OSOE_WAIT_G_SCALAR(X, OP)                                                 \
    OSOE_WAIT_G_STANDARD(X, OP) X(short, short, OP)                               \
    X(unsigned short, ushort, OP)

/* The deprecated shmem_wait. */
#define OSOE_WAIT_G_DEPRECATED(X, OP)                                             \
    X(short, short, OP) X(int, int, OP) X(long, long, OP)                         \
    X(long long, longlong, OP)

#define OSOE_WAIT_G(T, NAME, OP)  , T *: shmem_##NAME##_##OP

#define shmem_wait_until(ivar, cmp, cmp_value)                                    \
    (_Generic((ivar) OSOE_WAIT_G_SCALAR(OSOE_WAIT_G, wait_until))                 \
        ((ivar), (cmp), (cmp_value)))
#define shmem_test(ivar, cmp, cmp_value)                                          \
    (_Generic((ivar) OSOE_WAIT_G_SCALAR(OSOE_WAIT_G, test))                       \
        ((ivar), (cmp), (cmp_value)))
#define shmem_wait(ivar, cmp_value)                                               \
    (_Generic((ivar) OSOE_WAIT_G_DEPRECATED(OSOE_WAIT_G, wait))((ivar), (cmp_value)))

#define shmem_wait_until_all(ivars, nelems, status, cmp, cmp_value)               \
    (_Generic((ivars) OSOE_WAIT_G_STANDARD(OSOE_WAIT_G, wait_until_all))          \
        ((ivars), (nelems), (status), (cmp), (cmp_value)))
#define shmem_wait_until_any(ivars, nelems, status, cmp, cmp_value)               \
    (_Generic((ivars) OSOE_WAIT_G_STANDARD(OSOE_WAIT_G, wait_until_any))          \
        ((ivars), (nelems), (status), (cmp), (cmp_value)))
#define shmem_wait_until_some(ivars, nelems, indices, status, cmp, cmp_value)     \
    (_Generic((ivars) OSOE_WAIT_G_STANDARD(OSOE_WAIT_G, wait_until_some))         \
        ((ivars), (nelems), (indices), (status), (cmp), (cmp_value)))
#define shmem_wait_until_all_vector(ivars, nelems, status, cmp, cmp_values)       \
    (_Generic((ivars) OSOE_WAIT_G_STANDARD(OSOE_WAIT_G, wait_until_all_vector))   \
        ((ivars), (nelems), (status), (cmp), (cmp_values)))
#define shmem_wait_until_any_vector(ivars, nelems, status, cmp, cmp_values)       \
    (_Generic((ivars) OSOE_WAIT_G_STANDARD(OSOE_WAIT_G, wait_until_any_vector))   \
        ((ivars), (nelems), (status), (cmp), (cmp_values)))
#define shmem_wait_until_some_vector(ivars, nelems, indices, status, cmp, cmp_values) \
    (_Generic((ivars) OSOE_WAIT_G_STANDARD(OSOE_WAIT_G, wait_until_some_vector))  \
        ((ivars), (nelems), (indices), (status), (cmp), (cmp_values)))

#define shmem_test_all(ivars, nelems, status, cmp, cmp_value)                     \
    (_Generic((ivars) OSOE_WAIT_G_STANDARD(OSOE_WAIT_G, test_all))                \
        ((ivars), (nelems), (status), (cmp), (cmp_value)))
#define shmem_test_any(ivars, nelems, status, cmp, cmp_value)                     \
    (_Generic((ivars) OSOE_WAIT_G_STANDARD(OSOE_WAIT_G, test_any))                \
        ((ivars), (nelems), (status), (cmp), (cmp_value)))
#define shmem_test_some(ivars, nelems, indices, status, cmp, cmp_value)           \
    (_Generic((ivars) OSOE_WAIT_G_STANDARD(OSOE_WAIT_G, test_some))               \
        ((ivars), (nelems), (indices), (status), (cmp), (cmp_value)))
#define shmem_test_all_vector(ivars, nelems, status, cmp, cmp_values)             \
    (_Generic((ivars) OSOE_WAIT_G_STANDARD(OSOE_WAIT_G, test_all_vector))         \
        ((ivars), (nelems), (status), (cmp), (cmp_values)))
#define shmem_test_any_vector(ivars, nelems, status, cmp, cmp_values)             \
    (_Generic((ivars) OSOE_WAIT_G_STANDARD(OSOE_WAIT_G, test_any_vector))         \
        ((ivars), (nelems), (status), (cmp), (cmp_values)))
#define shmem_test_some_vector(ivars, nelems, indices, status, cmp, cmp_values)   \
    (_Generic((ivars) OSOE_WAIT_G_STANDARD(OSOE_WAIT_G, test_some_vector))        \
        ((ivars), (nelems), (indices), (status), (cmp), (cmp_values)))

#endif /* C11 */

/*
 * Profiling control (§10.1.1). Spec §10.1: "Any user code must be able to
 * control the profiler", and shmem.h is where user code looks - pshmem.h is
 * only the interposition surface (conformance review F7). Defined under
 * pshmem_pcontrol (profile.c), with shmem_pcontrol the usual weak alias.
 */
void shmem_pcontrol(int level, ...);

#undef OSOE_AMO_DECL_STANDARD
#undef OSOE_AMO_DECL_EXTENDED
#undef OSOE_AMO_DECL_BITWISE
#undef OSOE_AMO_DECL_CTX_STANDARD
#undef OSOE_AMO_DECL_CTX_EXTENDED
#undef OSOE_AMO_DECL_CTX_BITWISE
#undef OSOE_AMO_DECL_STANDARD_NBI
#undef OSOE_AMO_DECL_EXTENDED_NBI
#undef OSOE_AMO_DECL_BITWISE_NBI
#undef OSOE_AMO_DECL_CTX_STANDARD_NBI
#undef OSOE_AMO_DECL_CTX_EXTENDED_NBI
#undef OSOE_AMO_DECL_CTX_BITWISE_NBI
#undef OSOE_AMO_DECL_DEPRECATED_EXTENDED
#undef OSOE_AMO_DECL_DEPRECATED_STANDARD
#undef OSOE_WAIT_DECL_SCALAR
#undef OSOE_WAIT_DECL_MANY
#undef OSOE_WAIT_DECL_DEPRECATED
#undef OSOE_RMA_DECL_PUT_GET
#undef OSOE_RMA_DECL_P_G
#undef OSOE_RMA_DECL_IPUT_IGET
#undef OSOE_RMA_DECL_NBI
#undef OSOE_RMA_DECL_PUT_SIGNAL
#undef OSOE_RMA_DECL_PUT_SIGNAL_NBI
#undef OSOE_RMA_DECL_SIZE_PUT_GET
#undef OSOE_RMA_DECL_SIZE_IPUT_IGET
#undef OSOE_RMA_DECL_SIZE_NBI
#undef OSOE_RMA_DECL_SIZE_PUT_SIGNAL
#undef OSOE_RMA_DECL_SIZE_PUT_SIGNAL_NBI
#undef OSOE_REDUCE_DECL_BITWISE
#undef OSOE_REDUCE_DECL_ORDERED
#undef OSOE_REDUCE_DECL_ARITHMETIC
#undef OSOE_BROADCAST_DECL
#undef OSOE_COLLECT_DECL
#undef OSOE_ALLTOALL_DECL
#undef OSOE_TO_ALL_DECL_INTEGER
#undef OSOE_TO_ALL_DECL_FLOATING
#undef OSOE_TO_ALL_DECL_COMPLEX

#ifdef __cplusplus
}

/* spec §9.10.3's shmem_sync(team), which C11 dispatches by argument count
 * (above): C++ overloads it beside the deprecated active-set shmem_sync, the one
 * with C linkage. */
static inline int shmem_sync(shmem_team_t team)
{
    return shmem_team_sync(team);
}
#endif

#endif /* OSOE_SHMEM_H */
