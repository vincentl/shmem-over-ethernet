/*
 * A job that stays alive, for the launcher's signal tests (launcher-signals.sh).
 * Every PE says it is running once the whole job has initialized, then sleeps
 * for longer than any of those tests waits.
 */
#include <stdio.h>
#include <unistd.h>

#include <shmem.h>

int main(void)
{
    shmem_init();
    shmem_barrier_all();
    printf("running %d\n", shmem_my_pe());
    fflush(stdout);
    sleep(60);
    shmem_finalize();
    return 0;
}
