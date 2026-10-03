/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* osoe_fatal() leaves through exit(), which runs atexit handlers - and
 * start_pes registers shmem_finalize as one. So a PE that the library has just
 * declared dead goes on to enter a *collective*. PE 0 makes a put the library
 * refuses (a stack address); the others are in barrier_all.
 * argv[1] = "init" uses shmem_init instead, as the control. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <shmem.h>
int main(int argc, char **argv)
{
    int modern = argc > 1 && !strcmp(argv[1], "init");
    if (modern) shmem_init(); else start_pes(0);
    int me = shmem_my_pe();
    long *x = shmem_malloc(sizeof(long));
    if (me == 0) {
        long on_stack = 0, v = 1;
        shmem_putmem(&on_stack, &v, sizeof v, 1);       /* fatal: not symmetric */
        fprintf(stderr, "PE 0: NOT REACHED\n");
    }
    shmem_barrier_all();        /* PEs 1.. wait here for a PE 0 that is dead */
    fprintf(stderr, "PE %d: barrier_all returned although PE 0 never called it\n", me);
    *x = me;
    shmem_barrier_all();
    fprintf(stderr, "PE %d: second barrier_all returned\n", me);
    if (modern) shmem_finalize();
    return 0;
}
