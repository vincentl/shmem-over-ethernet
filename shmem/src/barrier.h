/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* The world-team barrier every collective and every allocator call uses
 * internally (§5.6). `routine` names the caller in the "blocked in" record. */
#ifndef OSOE_BARRIER_H
#define OSOE_BARRIER_H

void osoe_barrier_all(const char *routine);

/* shmem_finalize's barrier, on the finalize team (barrier.c). */
void osoe_barrier_finalize(void);

/* A barrier over team `t` - the quiet, then the team's sync - for a collective
 * that ends with one (alltoall), whose call (`call`; collectives.h, P24) the
 * sync's members say and compare in place of a sync's. Not counted as the
 * application's barrier. */
struct osoe_team;
struct osoe_call;
void osoe_team_barrier(struct osoe_team *t, const char *routine, const struct osoe_call *call);

/* A sync over team `t` and no quiet (§5.6), for the library's own use: the
 * split's sync over its parent and destroy's over the team (team.c). Not
 * counted as the application's barrier either. */
void osoe_team_sync(struct osoe_team *t, const char *routine);

#endif /* OSOE_BARRIER_H */
