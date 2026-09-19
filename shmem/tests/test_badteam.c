/*
 * Destroying a predefined team (spec §9.4.8: "If team compares equal to
 * SHMEM_TEAM_WORLD or any other predefined team, the behavior is undefined").
 *
 * Undefined is where a library gets to choose, and the choice here is the
 * project's usual one: say what is wrong and stop. Nothing else can hold a
 * valid team handle in this build - the splits all return SHMEM_TEAM_INVALID,
 * on which destroy is a documented no-op - so a call that reaches this point
 * named a predefined team deliberately, and continuing would leave a program
 * using a team it believes it has destroyed.
 */
#include <shmem.h>

int main(void)
{
    shmem_init();

    /* The no-op case first, so the failure below is unambiguous. */
    shmem_team_destroy(SHMEM_TEAM_INVALID);

    shmem_team_destroy(SHMEM_TEAM_WORLD);       /* must not return */

    shmem_finalize();
    return 0;
}
