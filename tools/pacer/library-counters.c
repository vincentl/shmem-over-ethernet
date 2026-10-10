/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The library's counters, read by each node's first PE from its node's
 * control segment (library-counters.h). The segment's name is the library's:
 * /osoe.<job>.<step>.<node>.<session>, from the launcher's variables - osoe-
 * launch's OSOE_ names when OSOE_PROCID is set, srun's SLURM_ names otherwise
 * (design §6.1) - and session 0, the pacer initializing once. The name is
 * gone once the job is up, and the reader finds the segment through a process
 * that holds it: this one.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "library-counters.h"
#include "segment.h"

const char *const pacer_library_counter_names[PACER_LIBRARY_COUNTERS] = {
    "connection.bytes_sent", "am.gets_nonblocking", "am.landing_full", "am.reserve_stalls",
    "am.wakes_sent", "shmem.barrier.ns"
};
const char *const pacer_library_field_names[PACER_LIBRARY_COUNTERS] = {
    "bytes_sent", "gets_nonblocking", "landing_full", "reserve_stalls", "wakes_sent",
    "barrier_ns"
};

static struct {
    int open;
    struct osoe_segment segment;
    const struct osoe_metric_descriptor *metrics[PACER_LIBRARY_COUNTERS];
    double last[PACER_LIBRARY_COUNTERS];
} library;

static const char *launcher(const char *suffix)
{
    char name[64];
    snprintf(name, sizeof name, "%s%s", getenv("OSOE_PROCID") ? "OSOE_" : "SLURM_", suffix);
    return getenv(name);
}

/* A counter's total over its scope: every counter block, every connection
 * toward a peer node, or every owner of the node's wires (layout 36). */
static double total(const struct osoe_metric_descriptor *m)
{
    uint32_t records = m->scope == OSOE_MSCOPE_CONNECTION ? library.segment.connection_count
                       : m->scope == OSOE_MSCOPE_THREAD ? library.segment.dir->block_count
                       : m->scope == OSOE_MSCOPE_OWNER ? library.segment.owner_count
                                                        : 1;
    double sum = 0;
    for (uint32_t i = 0; i < records; i++) {
        uint64_t v;
        if (osoe_segment_read(&library.segment, m, i, &v))
            sum += (double)v;
    }
    return sum;
}

int pacer_library_counters_open(void)
{
    library.open = 0;
    if (shmem_team_my_pe(SHMEM_TEAM_SHARED) != 0)
        return 0;
    const char *job = launcher("JOB_ID"), *step = launcher("STEP_ID"), *node = launcher("NODEID");
    char name[OSOE_SEGMENT_NAME_MAX], error[256];
    snprintf(name, sizeof name, "/osoe.%s.%s.%s.0", job ? job : "0", step ? step : "0",
             node ? node : "0");
    if (osoe_segment_open(&library.segment, name, error, sizeof error) != 0)
        return 0;
    for (int k = 0; k < PACER_LIBRARY_COUNTERS; k++)
        if (!(library.metrics[k] = osoe_segment_metric(&library.segment,
                                                       pacer_library_counter_names[k]))) {
            osoe_segment_close(&library.segment);
            return 0;
        }
    for (int k = 0; k < PACER_LIBRARY_COUNTERS; k++)
        library.last[k] = total(library.metrics[k]);
    library.open = 1;
    return 1;
}

void pacer_library_counters_growth(double growth[PACER_LIBRARY_COUNTERS])
{
    for (int k = 0; k < PACER_LIBRARY_COUNTERS; k++) {
        growth[k] = 0;
        if (!library.open)
            continue;
        double now = total(library.metrics[k]);
        growth[k] = now - library.last[k];
        library.last[k] = now;
    }
}

void pacer_library_counters_close(void)
{
    if (library.open)
        osoe_segment_close(&library.segment);
    library.open = 0;
}
