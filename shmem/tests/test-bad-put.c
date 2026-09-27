/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* A put to an address outside the symmetric heap fails loudly, naming the
 * routine (§7.4). Run by a test that expects the message. */
#include <shmem.h>

int main(void)
{
    shmem_init();
    int private_variable = 0;
    shmem_putmem(&private_variable, &private_variable, sizeof private_variable, 0);
    shmem_finalize();
    return 0;
}
