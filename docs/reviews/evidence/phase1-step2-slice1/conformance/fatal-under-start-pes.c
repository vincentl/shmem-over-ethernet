/* A loud failure (design section 3.15) in a program that initialized with
 * start_pes. osoe_fatal ends in exit(), exit() runs the atexit handler that
 * start_pes registered, and that handler is shmem_finalize - a collective.
 *
 * argv[1] = "init" or "start_pes". PE 0 makes a put to a private address, which
 * is the library's own test-bad-put case; every other PE waits for a value PE 0
 * will now never send. */
#include <shmem.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int legacy = argc > 1 && !strcmp(argv[1], "start_pes");
    if (legacy)
        start_pes(0);
    else
        shmem_init();
    long *flag = shmem_calloc(1, sizeof(long));
    if (shmem_my_pe() == 0) {
        long private_variable = 0;
        shmem_putmem(&private_variable, &private_variable, sizeof private_variable, 1);
    } else {
        shmem_long_wait_until(flag, SHMEM_CMP_NE, 0);
    }
    if (!legacy)
        shmem_finalize();
    return 0;
}
