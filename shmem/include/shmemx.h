/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * shmemx.h - vendor extensions (spec §5).
 *
 * "The shmemx.h header file must exist, even if no extensions are provided."
 * osoe provides none, so this header is empty by design rather than by
 * oversight. Anything added here takes the `shmemx_` prefix, which the export
 * test admits alongside `shmem_` and `pshmem_`. The cache routines, which left
 * the specification after 1.4, are declared in shmem.h, not here: shmem.h
 * says why.
 *
 * Introspection is deliberately *not* an extension: §3.14's rule is that the
 * library writes state into shared memory and an external tool reads it, so
 * `osoe-stat` needs no entry point here and a hung job stays readable.
 */
#ifndef OSOE_SHMEMX_H
#define OSOE_SHMEMX_H

#include <shmem.h>

#ifdef __cplusplus
extern "C" {
#endif

/* No extensions. */

#ifdef __cplusplus
}
#endif

#endif /* OSOE_SHMEMX_H */
