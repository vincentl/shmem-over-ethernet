/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * No death of any process strands a named object (§3.14, decided 2026-09-20).
 *
 * Every PE initializes, meets the others in a barrier - after which init has
 * completed everywhere, so every name the job created has been removed - and
 * then dies of the signal in OSOE_TEST_SIGNAL (KILL or TERM; the library
 * installs no handlers, so TERM's default action applies). The launcher's own
 * check then finds whatever the job left in /dev/shm, and ctest fails the test
 * on "left behind". Before slice 2 a SIGTERM to every PE left all six objects
 * of a two-node job behind (second review, D13).
 *
 * With OSOE_KEEP_CONTROL_SEGMENT=1 the control segments are left on purpose,
 * for post-mortem reading; the registration then runs osoe-stat --gc after the
 * job and expects it to remove them.
 */
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

int main(void)
{
    shmem_init();
    shmem_barrier_all();
    const char *which = getenv("OSOE_TEST_SIGNAL");
    raise(which && strcmp(which, "TERM") == 0 ? SIGTERM : SIGKILL);
    return 1;
}
