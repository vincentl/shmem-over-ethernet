#include <string.h>

#include <pshmem.h>
#include <shmem.h>

#include "export.h"

_Static_assert(sizeof SHMEM_VENDOR_STRING <= SHMEM_MAX_NAME_LEN,
               "SHMEM_VENDOR_STRING must fit in SHMEM_MAX_NAME_LEN, including its terminator");

OSOE_API void pshmem_info_get_version(int *major, int *minor)
{
    *major = SHMEM_MAJOR_VERSION;
    *minor = SHMEM_MINOR_VERSION;
}

OSOE_API void pshmem_info_get_name(char *name)
{
    /* The caller provides at least SHMEM_MAX_NAME_LEN bytes (§9.1.12). */
    memcpy(name, SHMEM_VENDOR_STRING, sizeof SHMEM_VENDOR_STRING);
}

/* The weak `shmem_` aliases (export.h). */
OSOE_ALIAS(info_get_version)
OSOE_ALIAS(info_get_name)
