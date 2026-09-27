/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Library information routines and constants (§6, §9.1.11, §9.1.12). */
#include <stdio.h>
#include <string.h>

#include <shmem.h>

static int fails;

#define CHECK(cond) do {                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            fails++;                                                      \
        }                                                                 \
    } while (0)

int main(void)
{
    int major = -1, minor = -1;
    shmem_info_get_version(&major, &minor);
    CHECK(major == SHMEM_MAJOR_VERSION && minor == SHMEM_MINOR_VERSION);
    CHECK(major == 1 && minor == 6);

    char name[SHMEM_MAX_NAME_LEN];
    memset(name, 'x', sizeof name);
    shmem_info_get_name(name);
    CHECK(memchr(name, '\0', sizeof name) != NULL);   /* terminated within the limit */
    CHECK(strcmp(name, SHMEM_VENDOR_STRING) == 0);    /* consistent with the constant */

    CHECK(_SHMEM_MAJOR_VERSION == SHMEM_MAJOR_VERSION);
    CHECK(_SHMEM_MINOR_VERSION == SHMEM_MINOR_VERSION);
    CHECK(_SHMEM_MAX_NAME_LEN == SHMEM_MAX_NAME_LEN);
    CHECK(strcmp(_SHMEM_VENDOR_STRING, SHMEM_VENDOR_STRING) == 0);

    return fails != 0;
}
