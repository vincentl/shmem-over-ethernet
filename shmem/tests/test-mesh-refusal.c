/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A caller the mesh's schedule does not give, refused (tier 2; H23). In a job of
 * three nodes node 0 calls node 1, node 1 calls node 2 and node 2 calls node 0
 * (mesh-schedule.h), so a call from node 0 to node 2 is no call of the
 * schedule's. Nodes 1 and 2 open their mesh as a lead does, without a
 * rendezvous (wire.c, which finds each peer by the port rule); node 0 calls
 * node 2 instead, and node 2 must end naming it - the check mesh_model.py's
 * lower_check shows a mesh needs the schedule's own rule in, and without
 * which node 2 would take the call for its wire toward node 0 and wait for
 * ever for its own call to be answered.
 *
 * Below the library, as shmem.wire-pair is: no shmem_init, the bootstrap from
 * the launcher's environment.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "bootstrap.h"
#include "listener.h"
#include "ring.h"
#include "wire.h"

_Noreturn void osoe_fatal(const char *fmt, ...);
_Noreturn void osoe_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "osoe_fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

void osoe_notice(const char *fmt, ...);
void osoe_notice(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "osoe_notice: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static uint32_t env_u32(const char *name)
{
    const char *v = getenv(name);
    return v ? (uint32_t)strtoul(v, NULL, 10) : 0;
}

int main(void)
{
    struct osoe_bootstrap b = {
        .job = strtoull(getenv("OSOE_JOB_ID") ? getenv("OSOE_JOB_ID") : "0", NULL, 10),
        .step = env_u32("OSOE_STEP_ID"),
        .node = env_u32("OSOE_NODEID"),
        .node_count = 3,
        .node_list = getenv("OSOE_STEP_NODELIST"),
        .no_progress_ns = UINT64_C(20000000000),  /* fail rather than hang */
        .port_low = OSOE_DEFAULT_PORT_LOW,
        .port_high = OSOE_DEFAULT_PORT_HIGH,
        .keepalive_off = true,
    };
    if (b.node == 0) {
        struct sockaddr_in at;
        osoe_listener_node_address(&b, 2, &at);
        int fd = osoe_listener_connect(&b, 0, &at, 2, OSOE_HELLO_DATA_WIRE);
        fprintf(stderr, "node 0 called node 2 (socket %d), a call the schedule does not give\n",
                fd);
        sleep(30);                      /* node 2's end ends the job */
        return 0;
    }
    struct osoe_wire wires[4];
    struct osoe_ring ring;
    osoe_wires_open(wires, (struct osoe_ring *[]){ &ring }, 0, &b, 0, NULL);
    fprintf(stderr, "node %u opened its mesh, taking a call the schedule does not give\n",
            b.node);
    return 1;
}
