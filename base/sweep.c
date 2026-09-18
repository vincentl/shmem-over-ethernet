#include "base/sweep.h"

/* Weak, so linking the SHMEM layer's watchdog.c into the same library replaces
 * it. base's own tests link base alone and get this one. */
__attribute__((weak)) void osoe_sweep_expired(const char *routine)
{
    (void)routine;
}
