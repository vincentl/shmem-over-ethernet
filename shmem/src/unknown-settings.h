/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The OSOE_ names osoe knows, and a notice for any other a PE's environment
 * holds (the round-three readiness review's diagnosability list, and round
 * four's: the misspelt OSOE_DEADLOCK_SECOND=0, meant to turn the deadlock
 * detector off, left it on, and a job waiting on a word was ended as
 * deadlocked at 10 s).
 *
 * An unknown name is said, not refused: a launch script that sets a name a
 * later osoe reads, or one osoe has retired, still runs - as the three retired
 * names do (bootstrap.c) - and a name osoe never read changes nothing to stop
 * for. What it costs a typo is a line, which is what was missing.
 */
#ifndef OSOE_UNKNOWN_SETTINGS_H
#define OSOE_UNKNOWN_SETTINGS_H

#include <stdbool.h>

/* Whether `name`, a whole variable name such as "OSOE_MAX_TEAMS", is one osoe
 * reads or retired: the library's settings, the launcher's names, the tools',
 * the retired ones, and the test suite's own (unknown-settings.c). */
bool osoe_unknown_settings_known(const char *name);

/*
 * A notice for each OSOE_ variable of this process's environment that
 * osoe_unknown_settings_known does not know, naming the nearest name it does
 * know where one is a letter or two away - "did you mean
 * OSOE_DEADLOCK_SECONDS?" - and otherwise how to list them. Called by the
 * job's rank 0 alone at each start (init.c), and said at the first, once for
 * the job: a program that starts the library again after shmem_finalize has
 * the same environment.
 */
void osoe_unknown_settings_notice(void);

#endif /* OSOE_UNKNOWN_SETTINGS_H */
