/*
 * The generic gather's own checks, each ending the job by name before a byte
 * of the gather lands (internode-generic.c; phase 1 step 5 slice 6). The test
 * passes when the job ends with the fatal's words (CMakeLists.txt); any other
 * ending fails it. OSOE_TEST_MISUSE says which:
 *
 *   - short-dest: SOS's shmem_team_b2b_collectives' mistake, a collect of ten
 *     longs from every PE into a dest of ten. On 0,0,1,1 node 1's lead's own
 *     copy of its members' blocks runs on past dest into the room that holds
 *     collect's places, and the gather finds its layout out of order
 *     (layout_overwritten);
 *   - local-count: node 1's lead's descriptor of the world says one member
 *     more on the node than the rank map puts there, and the first gather's
 *     walk, which finds the team's members from the map, finds the two
 *     disagree. The leads call internode.h's gather themselves, since the
 *     intranode phase would wait for the member that is not there.
 *
 * White-box for the second: it changes the descriptor, so the library's
 * sources are compiled into it, as test-team-storage.c's are.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "internode.h"
#include "launcher.h"
#include "state.h"
#include "team.h"

enum { COUNT = 10 };

static void short_dest(void)
{
    long *dest = shmem_malloc(COUNT * sizeof *dest);
    long *source = shmem_malloc(COUNT * sizeof *source);
    CHECK_FATAL(dest && source);
    for (int i = 0; i < COUNT; i++)
        source[i] = shmem_my_pe();
    shmem_long_collect(SHMEM_TEAM_WORLD, dest, source, COUNT);
}

static void local_count(void)
{
    int npes = shmem_n_pes();
    long *dest = shmem_malloc((size_t)npes * sizeof *dest);
    CHECK_FATAL(dest);
    const char *node = osoe_test_launcher("NODEID");
    if (osoe.bootstrap.local_id == 0) {
        struct osoe_team *world = osoe_team_numbered(0);
        CHECK_FATAL(world);
        if (node && atoi(node) == 1)
            world->local_members++;
        dest[world->me] = world->me;
        osoe_internode_collect(0, world->epoch + 1, dest, (size_t)npes * sizeof *dest,
                               sizeof *dest, NULL, "test-gather-misuse");
    }
    shmem_barrier_all();
}

int main(void)
{
    shmem_init();
    const char *misuse = getenv("OSOE_TEST_MISUSE");
    CHECK_FATAL(misuse);
    if (strcmp(misuse, "short-dest") == 0)
        short_dest();
    else if (strcmp(misuse, "local-count") == 0)
        local_count();
    printf("PE %d returned from its gather, which it must not\n", shmem_my_pe());
    shmem_finalize();
    return 1;
}
