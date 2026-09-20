/* The generic names the spec's own examples use that shmem.h does not define. */
#include <shmem.h>
int main(void) {
    static int targ = 0;
    shmem_init();
    int src = 33;
    shmem_put(&targ, &src, 1, 0);          /* spec Example 5 */
    long y = shmem_g(&targ, 0);            /* spec Example 7 */
    (void)y;
    shmem_finalize();
    return 0;
}
