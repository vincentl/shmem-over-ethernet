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

/* Library setup, exit, and query (§9.1) */
void shmem_init(void);
int shmem_my_pe(void);
int shmem_n_pes(void);
void shmem_finalize(void);
void shmem_query_initialized(int *initialized);

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
void shmem_signal_set(uint64_t *sig_addr, uint64_t signal, int pe);
void shmem_signal_add(uint64_t *sig_addr, uint64_t signal, int pe);
uint64_t shmem_signal_fetch(const uint64_t *sig_addr);
uint64_t shmem_signal_wait_until(uint64_t *sig_addr, int cmp, uint64_t cmp_value);

/* Point-to-point synchronization (§9.11.1, §9.11.8) */
void shmem_uint64_wait_until(uint64_t *ivar, int cmp, uint64_t cmp_value);
void shmem_uint32_wait_until(uint32_t *ivar, int cmp, uint32_t cmp_value);
int shmem_uint64_test(uint64_t *ivar, int cmp, uint64_t cmp_value);
int shmem_uint32_test(uint32_t *ivar, int cmp, uint32_t cmp_value);

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

#define OSOE_AMO_DECL_STANDARD(T, NAME)                                       \
    T shmem_##NAME##_atomic_compare_swap(T *dest, T cond, T value, int pe);   \
    void shmem_##NAME##_atomic_inc(T *dest, int pe);                          \
    T shmem_##NAME##_atomic_fetch_inc(T *dest, int pe);                       \
    void shmem_##NAME##_atomic_add(T *dest, T value, int pe);                 \
    T shmem_##NAME##_atomic_fetch_add(T *dest, T value, int pe);

#define OSOE_AMO_DECL_EXTENDED(T, NAME)                                       \
    T shmem_##NAME##_atomic_fetch(const T *source, int pe);                   \
    void shmem_##NAME##_atomic_set(T *dest, T value, int pe);                 \
    T shmem_##NAME##_atomic_swap(T *dest, T value, int pe);

#define OSOE_AMO_DECL_BITWISE(T, NAME)                                        \
    void shmem_##NAME##_atomic_and(T *dest, T value, int pe);                 \
    T shmem_##NAME##_atomic_fetch_and(T *dest, T value, int pe);              \
    void shmem_##NAME##_atomic_or(T *dest, T value, int pe);                  \
    T shmem_##NAME##_atomic_fetch_or(T *dest, T value, int pe);               \
    void shmem_##NAME##_atomic_xor(T *dest, T value, int pe);                 \
    T shmem_##NAME##_atomic_fetch_xor(T *dest, T value, int pe);

OSOE_AMO_STANDARD_TYPES(OSOE_AMO_DECL_STANDARD)
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_DECL_EXTENDED)
OSOE_AMO_FLOAT_TYPES(OSOE_AMO_DECL_EXTENDED)
OSOE_AMO_BITWISE_TYPES(OSOE_AMO_DECL_BITWISE)

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

/* Memory ordering (§9.12) */
void shmem_fence(void);
void shmem_quiet(void);
void shmem_pe_quiet(const int *target_pes, size_t npes);

/* Collective routines (§9.10) */
void shmem_barrier_all(void);

/* Library information (§9.1.11, §9.1.12) */
void shmem_info_get_version(int *major, int *minor);
void shmem_info_get_name(char *name);

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

#define shmem_atomic_fetch(source, pe)                                            \
    (_Generic((source) OSOE_AMO_G_EXTENDED(OSOE_AMO_GC, fetch))((source), (pe)))
#define shmem_atomic_set(dest, value, pe)                                         \
    (_Generic((dest) OSOE_AMO_G_EXTENDED(OSOE_AMO_G, set))((dest), (value), (pe)))
#define shmem_atomic_swap(dest, value, pe)                                        \
    (_Generic((dest) OSOE_AMO_G_EXTENDED(OSOE_AMO_G, swap))((dest), (value), (pe)))

#define shmem_atomic_compare_swap(dest, cond, value, pe)                          \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_G, compare_swap))               \
        ((dest), (cond), (value), (pe)))
#define shmem_atomic_inc(dest, pe)                                                \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_G, inc))((dest), (pe)))
#define shmem_atomic_fetch_inc(dest, pe)                                          \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_G, fetch_inc))((dest), (pe)))
#define shmem_atomic_add(dest, value, pe)                                         \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_G, add))((dest), (value), (pe)))
#define shmem_atomic_fetch_add(dest, value, pe)                                   \
    (_Generic((dest) OSOE_AMO_G_STANDARD(OSOE_AMO_G, fetch_add))                  \
        ((dest), (value), (pe)))

#define shmem_atomic_and(dest, value, pe)                                         \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_G, and))((dest), (value), (pe)))
#define shmem_atomic_fetch_and(dest, value, pe)                                   \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_G, fetch_and))                   \
        ((dest), (value), (pe)))
#define shmem_atomic_or(dest, value, pe)                                          \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_G, or))((dest), (value), (pe)))
#define shmem_atomic_fetch_or(dest, value, pe)                                    \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_G, fetch_or))                    \
        ((dest), (value), (pe)))
#define shmem_atomic_xor(dest, value, pe)                                         \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_G, xor))((dest), (value), (pe)))
#define shmem_atomic_fetch_xor(dest, value, pe)                                   \
    (_Generic((dest) OSOE_AMO_G_BITWISE(OSOE_AMO_G, fetch_xor))                   \
        ((dest), (value), (pe)))

#endif /* C11 */

#undef OSOE_AMO_DECL_STANDARD
#undef OSOE_AMO_DECL_EXTENDED
#undef OSOE_AMO_DECL_BITWISE

#ifdef __cplusplus
}
#endif

#endif /* OSOE_SHMEM_H */
