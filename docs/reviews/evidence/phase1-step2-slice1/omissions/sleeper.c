/* A job that is alive for a while, for signal experiments. PE `argv[1]` (if
 * given) leaves in the way argv[2] names instead of finalizing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <shmem.h>
int main(int argc, char **argv)
{
    shmem_init();
    int me = shmem_my_pe();
    long *x = shmem_malloc(sizeof(long));
    shmem_barrier_all();
    if (me == 0) { printf("running\n"); fflush(stdout); }
    int who = argc > 1 ? atoi(argv[1]) : -1;
    const char *how = argc > 2 ? argv[2] : "sleep";
    if (me == who) {
        if (!strcmp(how, "exit")) exit(0);              /* exit() without finalize */
        if (!strcmp(how, "return")) return 0;
        if (!strcmp(how, "abort")) abort();
    }
    if (!strcmp(how, "sleep")) sleep(30);
    shmem_free(x);
    shmem_finalize();
    return 0;
}
