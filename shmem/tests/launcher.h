/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The launcher's variables, for a test, under the bootstrap's own rule (§6.1):
 * osoe-launch's OSOE_ names when OSOE_PROCID is set, srun's SLURM_ names
 * otherwise - so that a test started by srun, or by osoe-launch -s, finds its
 * own job's objects by name as the library does.
 */
#ifndef OSOE_TEST_LAUNCHER_H
#define OSOE_TEST_LAUNCHER_H

#include <stdio.h>
#include <stdlib.h>

/* The launcher variable named by `suffix` - "JOB_ID", "NODEID" - or NULL. */
static inline const char *osoe_test_launcher(const char *suffix)
{
    char name[64];
    snprintf(name, sizeof name, "%s%s", getenv("OSOE_PROCID") ? "OSOE_" : "SLURM_", suffix);
    return getenv(name);
}

#endif /* OSOE_TEST_LAUNCHER_H */
