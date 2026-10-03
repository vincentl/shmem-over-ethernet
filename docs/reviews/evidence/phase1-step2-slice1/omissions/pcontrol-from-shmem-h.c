/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Spec §10.1: "Any user code must be able to control the profiler". User code
 * includes <shmem.h>; does it get shmem_pcontrol? */
#include <shmem.h>
int main(void) { shmem_init(); shmem_pcontrol(0); shmem_finalize(); return 0; }
