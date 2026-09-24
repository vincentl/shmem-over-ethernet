/* Process-wide library state, valid between the first shmem_init and the last
 * shmem_finalize. */
#ifndef OSOE_STATE_H
#define OSOE_STATE_H

#include <stdatomic.h>
#include <stdint.h>

#include <stdbool.h>

#include "allocator.h"
#include "bootstrap.h"
#include "job.h"
#include "node.h"

/*
 * What every part of the library may read: the job, the node, the heap and the
 * collectives' own epoch counts. **Nothing of any backend's.** The AM layer's
 * wires, connections, reply pool and engine are in am-state.h, which only the
 * AM backend's files include; before slice 3 they were fields here, so every
 * §4 file could name them, and the boundary rested on nobody doing so (second
 * review, roce-seam F6). cmake/CheckBoundary.cmake now makes that a test.
 */
struct osoe_state {
    struct osoe_bootstrap bootstrap;
    struct osoe_node node;
    struct osoe_job job;
    struct osoe_allocator allocator;
    uint32_t world_epoch;           /* k for SHMEM_TEAM_WORLD (§5.4) */
    uint32_t shared_epoch;          /* and for SHMEM_TEAM_SHARED */
    uint32_t finalize_epoch;        /* and for the finalize team (team.h) */
    uint32_t world_data_epoch;      /* d for SHMEM_TEAM_WORLD: its data collectives (§5.4) */
    uint32_t shared_data_epoch;     /* and for SHMEM_TEAM_SHARED */
};

extern struct osoe_state osoe;

/*
 * Fatal, naming `routine`, unless the library is between the first
 * shmem_init (or shmem_init_thread) and the last shmem_finalize - everything
 * above is undefined outside that window (Annex C), and calling in anyway is
 * the commonest beginner mistake, not a case worth a segfault on a freed
 * mapping instead of a diagnosis (conformance review F5). Inline: one acquire
 * load (a plain load on x86-64, an ldar on aarch64) and a branch against the
 * flag shmem_query_initialized already publishes, with the fatal out of line,
 * so every routine that dereferences `osoe` can afford it.
 */
extern _Atomic int osoe_initialized;
_Noreturn void osoe_not_initialized(const char *routine);
static inline void osoe_require_initialized(const char *routine)
{
    if (!atomic_load_explicit(&osoe_initialized, memory_order_acquire))
        osoe_not_initialized(routine);
}

#endif /* OSOE_STATE_H */
