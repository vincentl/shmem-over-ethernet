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

/* Memory management (§9.3) */
void *shmem_malloc(size_t size);
void shmem_free(void *ptr);

/* Collective routines (§9.10) */
void shmem_barrier_all(void);

/* Library information (§9.1.11, §9.1.12) */
void shmem_info_get_version(int *major, int *minor);
void shmem_info_get_name(char *name);

#ifdef __cplusplus
}
#endif

#endif /* OSOE_SHMEM_H */
