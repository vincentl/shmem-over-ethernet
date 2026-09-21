/* The world-team barrier every collective and every allocator call uses
 * internally (§5.6). `routine` names the caller in the "blocked in" record. */
#ifndef OSOE_BARRIER_H
#define OSOE_BARRIER_H

void osoe_barrier_all(const char *routine);

/* shmem_finalize's barrier, on the finalize team (barrier.c). */
void osoe_barrier_finalize(void);

#endif /* OSOE_BARRIER_H */
