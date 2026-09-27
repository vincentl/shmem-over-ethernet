/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Environment parsing. osoe's own variables use the OSOE_ prefix; SHMEM_
 * belongs to the specification (§6.2).
 *
 * Numbers are plain decimal: digits only, no sign, no surrounding space, no
 * base prefix. A malformed value is an error, never a default, so a typo in a
 * launch script fails at init instead of silently changing behaviour.
 */
#ifndef OSOE_BASE_ENVIRONMENT_H
#define OSOE_BASE_ENVIRONMENT_H

#include <stddef.h>
#include <stdint.h>

enum osoe_environment_status {
    OSOE_ENVIRONMENT_UNSET,
    OSOE_ENVIRONMENT_OK,
    OSOE_ENVIRONMENT_INVALID,
};

/* 0 on success, -1 if s is not a decimal number within range. */
int osoe_parse_u64(const char *s, uint64_t *out);
int osoe_parse_u32(const char *s, uint32_t *out);

/* A comma-separated list of u32, e.g. SLURM_GTIDS "0,1,2". At least one
 * element, at most capacity. 0 on success, -1 if malformed or longer than capacity. */
int osoe_parse_u32_list(const char *s, uint32_t *out, size_t capacity, size_t *n);

enum osoe_environment_status osoe_environment_u64(const char *name, uint64_t *out);
enum osoe_environment_status osoe_environment_u32(const char *name, uint32_t *out);

#endif /* OSOE_BASE_ENVIRONMENT_H */
