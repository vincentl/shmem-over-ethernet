/* Barrier and sync on SHMEM_TEAM_WORLD (§5.3-§5.6). */
#ifndef OSOE_BARRIER_H
#define OSOE_BARRIER_H

/* shmem_barrier_all without the API entry: also used by the allocator and by
 * shmem_finalize. `routine` names the caller for the diagnostic a safety sweep
 * may print (base/sweep.h), since a PE stuck here is stuck on behalf of
 * whichever collective called it. */
void osoe_barrier_all(const char *routine);

#endif /* OSOE_BARRIER_H */
