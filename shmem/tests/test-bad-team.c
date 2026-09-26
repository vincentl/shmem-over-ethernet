/*
 * Team handles the program must not use, each ending the job by name.
 *
 * Destroying a predefined team (spec §9.4.8: "If team compares equal to
 * SHMEM_TEAM_WORLD or any other predefined team, the behavior is undefined"),
 * and using a handle that is "otherwise invalid" (§9.4.1 and the rest): one
 * the library never issued, and one this PE has destroyed. Undefined is where
 * a library gets to choose, and the choice here is the project's usual one:
 * say what is wrong and stop. Continuing would leave a program using a team it
 * believes it has destroyed, or one that was never there.
 *
 * OSOE_TEST_BAD_TEAM chooses the misuse: `world` (the default) and `shared`
 * destroy a predefined team; `destroyed` asks a destroyed team its size, and
 * `destroyed-twice` destroys it again; `never` synchronizes on a pointer the
 * library never issued as a team.
 */
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

int main(void)
{
    const char *misuse = getenv("OSOE_TEST_BAD_TEAM");
    if (!misuse)
        misuse = "world";
    shmem_init();

    /* The no-op case first, so the failure below is unambiguous. */
    shmem_team_destroy(SHMEM_TEAM_INVALID);

    if (strcmp(misuse, "world") == 0)
        shmem_team_destroy(SHMEM_TEAM_WORLD);           /* must not return */
    else if (strcmp(misuse, "shared") == 0)
        shmem_team_destroy(SHMEM_TEAM_SHARED);          /* must not return */
    else if (strcmp(misuse, "destroyed") == 0 || strcmp(misuse, "destroyed-twice") == 0) {
        shmem_team_t team = SHMEM_TEAM_INVALID;
        if (shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, shmem_n_pes(), NULL, 0, &team) != 0)
            return 2;
        shmem_team_destroy(team);
        if (strcmp(misuse, "destroyed") == 0)
            shmem_team_n_pes(team);                     /* must not return */
        else
            shmem_team_destroy(team);                   /* must not return */
    } else if (strcmp(misuse, "never") == 0) {
        static long not_a_team;
        shmem_team_sync((shmem_team_t)(void *)&not_a_team);     /* must not return */
    }

    shmem_finalize();
    return 0;
}
