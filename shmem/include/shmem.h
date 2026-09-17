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

/* Memory management (§9.3) */
void *shmem_malloc(size_t size);
void shmem_free(void *ptr);

/* Remote memory access (§9.6.1) */
void shmem_putmem(void *dest, const void *source, size_t nelems, int pe);
void shmem_getmem(void *dest, const void *source, size_t nelems, int pe);

/* Collective routines (§9.10) */
void shmem_barrier_all(void);

/* Library information (§9.1.11, §9.1.12) */
void shmem_info_get_version(int *major, int *minor);
void shmem_info_get_name(char *name);

#ifdef __cplusplus
}
#endif

#endif /* OSOE_SHMEM_H */
