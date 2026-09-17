/* Process-wide library state, valid between the first shmem_init and the last
 * shmem_finalize. */
#ifndef OSOE_STATE_H
#define OSOE_STATE_H

#include <stdint.h>

#include "boot.h"
#include "node.h"

struct osoe_state {
    struct osoe_boot boot;
    struct osoe_node node;
    uint32_t world_epoch;           /* k for SHMEM_TEAM_WORLD (§5.4) */
};

extern struct osoe_state osoe;

#endif /* OSOE_STATE_H */
