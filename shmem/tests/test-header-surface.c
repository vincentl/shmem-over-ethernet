/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Two header-surface fixes from the conformance review, both proven by what
 * compiles rather than by a runtime CHECK:
 *
 *   F7: shmem_pcontrol must be declared by <shmem.h> alone (spec §10.1.1:
 *       "any user code must be able to control the profiler", and shmem.h is
 *       where user code looks). It used to be declared only by <pshmem.h>.
 *   F8: <mpp/shmemx.h> must resolve, the same way <mpp/shmem.h> already does
 *       (Annex F.2.1).
 *
 * <pshmem.h> is deliberately not included here: it used to be the only header
 * declaring shmem_pcontrol, and including it would let that regress unnoticed.
 */
#include <mpp/shmemx.h>
#include <shmem.h>

int main(void)
{
    /* §10.1.1: "OpenSHMEM libraries make no use of this routine and simply
     * return immediately to the user code" - nothing to check but that it
     * links, with the variadic argument list the spec's synopsis gives it. */
    shmem_pcontrol(0);
    shmem_pcontrol(1, 42, "ignored");
    return 0;
}
