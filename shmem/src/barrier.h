/* The world-team barrier every collective and every allocator call uses
 * internally (§5.6). `routine` names the caller in the "blocked in" record. */
#ifndef OSOE_BARRIER_H
#define OSOE_BARRIER_H

void osoe_barrier_all(const char *routine);

#endif /* OSOE_BARRIER_H */
