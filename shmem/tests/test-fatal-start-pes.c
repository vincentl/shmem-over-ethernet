/*
 * A loud failure ends the job, and does not first join a collective (§3.15;
 * second review, conformance F1).
 *
 * start_pes registers shmem_finalize as an atexit handler (Annex F.2.3), and
 * shmem_finalize is a barrier. osoe_fatal used to end in exit(), which runs
 * atexit handlers - so a PE failing loudly in a start_pes program walked into a
 * barrier the others would never reach, and the job hung until the launcher's
 * timeout. The review measured 7-8 s against a 0 s shmem_init control.
 *
 * PE 0 makes a put to a private address - the library's own bad-put case, a
 * fatal - and every other PE waits for a value PE 0 will now never send. The
 * job must end promptly, with PE 0's message; ctest's short launcher timeout is
 * what "promptly" means, and a timeout fails the test.
 */
#include <stdio.h>

#include <shmem.h>

int main(void)
{
    start_pes(0);
    long *flag = shmem_calloc(1, sizeof *flag);
    if (!flag)
        return 2;
    if (shmem_my_pe() == 0) {
        long private_variable = 0;
        shmem_putmem(&private_variable, &private_variable, sizeof private_variable, 1);
        printf("PUT TO A PRIVATE ADDRESS RETURNED\n");
    } else {
        shmem_long_wait_until(flag, SHMEM_CMP_NE, 0);
    }
    return 0;
}
