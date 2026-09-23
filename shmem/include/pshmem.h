/*
 * pshmem.h - the OpenSHMEM profiling interface (spec §10).
 *
 * Every routine of shmem.h has a `pshmem_` spelling that is the *strong*
 * definition in the library; `shmem_` is a weak alias to it. An application
 * that wants to profile, trace or intercept a routine defines its own
 * `shmem_X`, which replaces the weak alias at link time, and calls `pshmem_X`
 * to reach the real implementation. The spec requires the interface to exist;
 * the weak-alias arrangement is what makes it usable with static linking as
 * well as shared.
 *
 * The generated families are declared here from the same lists and the same
 * declaration macros shmem.h uses, with the prefix as a parameter, so a routine
 * cannot exist in one spelling and not the other. The rest are written out,
 * which is the whole of the hand-maintained surface.
 *
 * There are no `pshmem_` spellings of the C11 generic macros. A macro is not a
 * symbol and there is nothing to interpose: a program that wants the profiling
 * path calls the named routine.
 */
#ifndef OSOE_PSHMEM_H
#define OSOE_PSHMEM_H

#include <shmem.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Library setup, exit, and query (§9.1, §9.2) */
void pshmem_init(void);
int pshmem_my_pe(void);
int pshmem_n_pes(void);
void pshmem_finalize(void);
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Noreturn
#endif
void pshmem_global_exit(int status);
void pshmem_query_initialized(int *initialized);
int pshmem_init_thread(int requested, int *provided);
void pshmem_query_thread(int *provided);

/* Teams and contexts (§9.4, §9.5) */
int pshmem_team_my_pe(shmem_team_t team);
int pshmem_team_n_pes(shmem_team_t team);
int pshmem_team_get_config(shmem_team_t team, long config_mask,
                           shmem_team_config_t *config);
int pshmem_team_translate_pe(shmem_team_t src_team, int src_pe, shmem_team_t dest_team);
int pshmem_team_split_strided(shmem_team_t parent_team, int start, int stride, int size,
                              const shmem_team_config_t *config, long config_mask,
                              shmem_team_t *new_team);
int pshmem_team_split_2d(shmem_team_t parent_team, int xrange,
                         const shmem_team_config_t *xaxis_config, long xaxis_mask,
                         shmem_team_t *xaxis_team,
                         const shmem_team_config_t *yaxis_config, long yaxis_mask,
                         shmem_team_t *yaxis_team);
void pshmem_team_destroy(shmem_team_t team);
void *pshmem_team_ptr(shmem_team_t team, const void *dest, int pe);
int pshmem_ctx_create(long options, shmem_ctx_t *ctx);
int pshmem_team_create_ctx(shmem_team_t team, long options, shmem_ctx_t *ctx);
void pshmem_ctx_destroy(shmem_ctx_t ctx);
int pshmem_ctx_get_team(shmem_ctx_t ctx, shmem_team_t *team);

/* Memory management (§9.3) */
void *pshmem_malloc(size_t size);
void *pshmem_calloc(size_t count, size_t size);
void *pshmem_realloc(void *ptr, size_t size);
void *pshmem_align(size_t alignment, size_t size);
void *pshmem_malloc_with_hints(size_t size, long hints);
void pshmem_free(void *ptr);

/* Remote memory access (§9.6) and the symmetric pointer (§9.1.9) */
void pshmem_putmem(void *dest, const void *source, size_t nelems, int pe);
void pshmem_getmem(void *dest, const void *source, size_t nelems, int pe);
void pshmem_ctx_putmem(shmem_ctx_t ctx, void *dest, const void *source, size_t nelems,
                       int pe);
void pshmem_ctx_getmem(shmem_ctx_t ctx, void *dest, const void *source, size_t nelems,
                       int pe);
void *pshmem_ptr(const void *dest, int pe);

/* Signalling operations (§9.8) */
void pshmem_putmem_signal(void *dest, const void *source, size_t nelems,
                          uint64_t *sig_addr, uint64_t signal, int sig_op, int pe);
void pshmem_ctx_putmem_signal(shmem_ctx_t ctx, void *dest, const void *source,
                              size_t nelems, uint64_t *sig_addr, uint64_t signal,
                              int sig_op, int pe);
void pshmem_signal_set(uint64_t *sig_addr, uint64_t signal, int pe);
void pshmem_signal_add(uint64_t *sig_addr, uint64_t signal, int pe);
void pshmem_ctx_signal_set(shmem_ctx_t ctx, uint64_t *sig_addr, uint64_t signal, int pe);
void pshmem_ctx_signal_add(shmem_ctx_t ctx, uint64_t *sig_addr, uint64_t signal, int pe);
uint64_t pshmem_signal_fetch(const uint64_t *sig_addr);
uint64_t pshmem_signal_wait_until(uint64_t *sig_addr, int cmp, uint64_t cmp_value);

/* Memory ordering (§9.12) */
void pshmem_fence(void);
void pshmem_quiet(void);
void pshmem_pe_quiet(const int *target_pes, size_t npes);
void pshmem_ctx_fence(shmem_ctx_t ctx);
void pshmem_ctx_quiet(shmem_ctx_t ctx);
void pshmem_ctx_pe_quiet(shmem_ctx_t ctx, const int *target_pes, size_t npes);

/* Collectives (§9.10) */
void pshmem_barrier_all(void);
void pshmem_sync_all(void);
int pshmem_team_sync(shmem_team_t team);

/* Library information (§9.1.11, §9.1.12) */
void pshmem_info_get_version(int *major, int *minor);
void pshmem_info_get_name(char *name);

/* Accessibility queries (§9.1.7, §9.1.8) */
int pshmem_pe_accessible(int pe);
int pshmem_addr_accessible(const void *addr, int pe);

/* Communication sessions (§9.9) */
void pshmem_ctx_session_start(shmem_ctx_t ctx, long options,
                              const shmem_ctx_session_config_t *config, long config_mask);
void pshmem_ctx_session_stop(shmem_ctx_t ctx);

/*
 * SHMEM_CACHE (Annex F.2.7). The other deprecated routines - start_pes,
 * _my_pe, shmalloc and the rest - have no `shmem_` prefix, so there is no
 * `pshmem_` spelling of them: §10's profiling interface is defined over the
 * `shmem_` namespace, and a `pstart_pes` would be an invention.
 */
void pshmem_clear_cache_inv(void);
void pshmem_set_cache_inv(void);
void pshmem_set_cache_line_inv(void *dest);
void pshmem_clear_cache_line_inv(void *dest);
void pshmem_udcflush(void);
void pshmem_udcflush_line(void *dest);

/* The deprecated unprefixed waits (§9.11.1) */
void pshmem_wait(long *ivar, long cmp_value);
void pshmem_wait_until(long *ivar, int cmp, long cmp_value);

/* The generated families, from the same lists as shmem.h. */
#define OSOE_AMO_PDECL_STANDARD(T, NAME)     OSOE_AMO_DECL_STANDARD_P(pshmem_, T, NAME)
#define OSOE_AMO_PDECL_EXTENDED(T, NAME)     OSOE_AMO_DECL_EXTENDED_P(pshmem_, T, NAME)
#define OSOE_AMO_PDECL_BITWISE(T, NAME)      OSOE_AMO_DECL_BITWISE_P(pshmem_, T, NAME)
#define OSOE_AMO_PDECL_CTX_STANDARD(T, NAME) OSOE_AMO_DECL_CTX_STANDARD_P(pshmem_, T, NAME)
#define OSOE_AMO_PDECL_CTX_EXTENDED(T, NAME) OSOE_AMO_DECL_CTX_EXTENDED_P(pshmem_, T, NAME)
#define OSOE_AMO_PDECL_CTX_BITWISE(T, NAME)  OSOE_AMO_DECL_CTX_BITWISE_P(pshmem_, T, NAME)
#define OSOE_WAIT_PDECL_SCALAR(T, NAME)      OSOE_WAIT_DECL_SCALAR_P(pshmem_, T, NAME)
#define OSOE_WAIT_PDECL_MANY(T, NAME)        OSOE_WAIT_DECL_MANY_P(pshmem_, T, NAME)
#define OSOE_WAIT_PDECL_DEPRECATED(T, NAME)  OSOE_WAIT_DECL_DEPRECATED_P(pshmem_, T, NAME)

OSOE_AMO_STANDARD_TYPES(OSOE_AMO_PDECL_STANDARD)
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_PDECL_EXTENDED)
OSOE_AMO_FLOAT_TYPES(OSOE_AMO_PDECL_EXTENDED)
OSOE_AMO_BITWISE_TYPES(OSOE_AMO_PDECL_BITWISE)
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_PDECL_CTX_STANDARD)
OSOE_AMO_STANDARD_TYPES(OSOE_AMO_PDECL_CTX_EXTENDED)
OSOE_AMO_FLOAT_TYPES(OSOE_AMO_PDECL_CTX_EXTENDED)
OSOE_AMO_BITWISE_TYPES(OSOE_AMO_PDECL_CTX_BITWISE)
OSOE_AMO_STANDARD_TYPES(OSOE_WAIT_PDECL_SCALAR)
OSOE_WAIT_SHORT_TYPES(OSOE_WAIT_PDECL_SCALAR)
OSOE_AMO_STANDARD_TYPES(OSOE_WAIT_PDECL_MANY)
OSOE_WAIT_DEPRECATED_TYPES(OSOE_WAIT_PDECL_DEPRECATED)

#undef OSOE_AMO_PDECL_STANDARD
#undef OSOE_AMO_PDECL_EXTENDED
#undef OSOE_AMO_PDECL_BITWISE
#undef OSOE_AMO_PDECL_CTX_STANDARD
#undef OSOE_AMO_PDECL_CTX_EXTENDED
#undef OSOE_AMO_PDECL_CTX_BITWISE
#undef OSOE_WAIT_PDECL_SCALAR
#undef OSOE_WAIT_PDECL_MANY
#undef OSOE_WAIT_PDECL_DEPRECATED

/* Profiling control (§10.1.1). shmem_pcontrol is declared by shmem.h, which is
 * where user code looks (conformance review F7); this header only adds the
 * strong pshmem_ spelling a profiling library interposes. */
void pshmem_pcontrol(int level, ...);

#ifdef __cplusplus
}
#endif

#endif /* OSOE_PSHMEM_H */
