/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Profiling control (spec §10.1.1).
 *
 * "OpenSHMEM libraries make no use of this routine and simply return
 * immediately to the user code." The routine exists so that a *profiling*
 * library, which interposes it along with everything else (export.h), has a
 * documented place for the application to say when to start and stop
 * measuring. There is nothing for the library underneath to do with it, and
 * inventing a meaning would be inventing an interface the spec declines to
 * define.
 */
#include <stdarg.h>

#include <pshmem.h>
#include <shmem.h>

#include "export.h"

OSOE_API void pshmem_pcontrol(int level, ...)
{
    (void)level;
}

OSOE_ALIAS(pcontrol)
