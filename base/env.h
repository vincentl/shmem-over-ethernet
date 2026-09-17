/*
 * Environment parsing. osoe's own variables use the OSOE_ prefix; SHMEM_
 * belongs to the specification (§6.2).
 *
 * Numbers are plain decimal: digits only, no sign, no surrounding space, no
 * base prefix. A malformed value is an error, never a default, so a typo in a
 * launch script fails at init instead of silently changing behaviour.
 */
#ifndef OSOE_BASE_ENV_H
#define OSOE_BASE_ENV_H

#include <stddef.h>
#include <stdint.h>

enum osoe_env_status {
    OSOE_ENV_UNSET,
    OSOE_ENV_OK,
    OSOE_ENV_INVALID,
};

/* 0 on success, -1 if s is not a decimal number within range. */
int osoe_parse_u64(const char *s, uint64_t *out);
int osoe_parse_u32(const char *s, uint32_t *out);

/* A comma-separated list of u32, e.g. SLURM_GTIDS "0,1,2". At least one
 * element, at most cap. 0 on success, -1 if malformed or longer than cap. */
int osoe_parse_u32_list(const char *s, uint32_t *out, size_t cap, size_t *n);

enum osoe_env_status osoe_env_u64(const char *name, uint64_t *out);
enum osoe_env_status osoe_env_u32(const char *name, uint32_t *out);

#endif /* OSOE_BASE_ENV_H */
