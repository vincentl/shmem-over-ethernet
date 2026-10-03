/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* An LD_PRELOAD tool in the style of spec §10.2: defines shmem_X, calls pshmem_X. */
#include <stdio.h>
#include <pshmem.h>
static int puts_seen, barriers_seen;
void shmem_putmem(void *d, const void *s, size_t n, int pe) { puts_seen++; pshmem_putmem(d, s, n, pe); }
void shmem_barrier_all(void) { barriers_seen++; pshmem_barrier_all(); }
void shmem_finalize(void) { fprintf(stderr, "profiler: PE %d saw %d putmem, %d barrier_all\n", pshmem_my_pe(), puts_seen, barriers_seen); pshmem_finalize(); }
