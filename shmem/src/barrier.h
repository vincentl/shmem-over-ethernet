/* Barrier and sync on SHMEM_TEAM_WORLD (§5.3-§5.6). */
#ifndef OSOE_BARRIER_H
#define OSOE_BARRIER_H

/* shmem_barrier_all without the API entry: also used by the allocator and by
 * shmem_finalize. */
void osoe_barrier_all(void);

#endif /* OSOE_BARRIER_H */
