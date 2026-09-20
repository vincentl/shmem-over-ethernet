/* Spec 10.1.1 gives shmem_pcontrol as a routine "any user code" calls; user
 * code includes <shmem.h>, not the profiling tool's <pshmem.h>. */
#include <shmem.h>

int main(void)
{
    shmem_init();
    shmem_pcontrol(1);
    shmem_finalize();
    return 0;
}
