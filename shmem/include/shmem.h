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

/* Atomic memory operations (§9.7). Phase 1 step 1 provides the 64-bit and
 * 32-bit unsigned types; the rest of Table 6 follows mechanically. */
void shmem_uint64_atomic_set(uint64_t *dest, uint64_t value, int pe);
uint64_t shmem_uint64_atomic_swap(uint64_t *dest, uint64_t value, int pe);
uint64_t shmem_uint64_atomic_fetch(const uint64_t *source, int pe);
uint64_t shmem_uint64_atomic_compare_swap(uint64_t *dest, uint64_t cond, uint64_t value, int pe);
void shmem_uint64_atomic_add(uint64_t *dest, uint64_t value, int pe);
uint64_t shmem_uint64_atomic_fetch_add(uint64_t *dest, uint64_t value, int pe);
void shmem_uint64_atomic_inc(uint64_t *dest, int pe);
uint64_t shmem_uint64_atomic_fetch_inc(uint64_t *dest, int pe);
void shmem_uint64_atomic_and(uint64_t *dest, uint64_t value, int pe);
uint64_t shmem_uint64_atomic_fetch_and(uint64_t *dest, uint64_t value, int pe);
void shmem_uint64_atomic_or(uint64_t *dest, uint64_t value, int pe);
uint64_t shmem_uint64_atomic_fetch_or(uint64_t *dest, uint64_t value, int pe);
void shmem_uint64_atomic_xor(uint64_t *dest, uint64_t value, int pe);
uint64_t shmem_uint64_atomic_fetch_xor(uint64_t *dest, uint64_t value, int pe);

void shmem_uint32_atomic_set(uint32_t *dest, uint32_t value, int pe);
uint32_t shmem_uint32_atomic_swap(uint32_t *dest, uint32_t value, int pe);
uint32_t shmem_uint32_atomic_fetch(const uint32_t *source, int pe);
uint32_t shmem_uint32_atomic_compare_swap(uint32_t *dest, uint32_t cond, uint32_t value, int pe);
void shmem_uint32_atomic_add(uint32_t *dest, uint32_t value, int pe);
uint32_t shmem_uint32_atomic_fetch_add(uint32_t *dest, uint32_t value, int pe);
void shmem_uint32_atomic_inc(uint32_t *dest, int pe);
uint32_t shmem_uint32_atomic_fetch_inc(uint32_t *dest, int pe);
void shmem_uint32_atomic_and(uint32_t *dest, uint32_t value, int pe);
uint32_t shmem_uint32_atomic_fetch_and(uint32_t *dest, uint32_t value, int pe);
void shmem_uint32_atomic_or(uint32_t *dest, uint32_t value, int pe);
uint32_t shmem_uint32_atomic_fetch_or(uint32_t *dest, uint32_t value, int pe);
void shmem_uint32_atomic_xor(uint32_t *dest, uint32_t value, int pe);
uint32_t shmem_uint32_atomic_fetch_xor(uint32_t *dest, uint32_t value, int pe);

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

#ifdef __cplusplus
}
#endif

#endif /* OSOE_SHMEM_H */
