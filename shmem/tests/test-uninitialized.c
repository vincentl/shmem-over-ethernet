/*
 * A call before shmem_init or after the last shmem_finalize must be a named
 * fatal, not a segfault, a misleading message, a valid-looking wrong answer,
 * or a silent success (conformance review F5). argv selects the scenario:
 *
 *   before <routine>   call `routine` with nothing else run first
 *   after  <routine>   shmem_init(); shmem_finalize(); then `routine`
 *
 * "before" is run directly - there is nothing to launch yet, and none of
 * these routines reads the launcher environment before osoe_require_
 * initialized() would fatal anyway. "after" needs a real job, so it runs
 * under osoe-launch. See shmem.refuse.uninitialized.* in CMakeLists.txt.
 */
#include <string.h>

#include <shmem.h>

static void call(const char *routine)
{
    static long ivar;
    static int dummy;
    if (strcmp(routine, "malloc") == 0) {
        shmem_malloc(8);
    } else if (strcmp(routine, "free") == 0) {
        shmem_free(&dummy);
    } else if (strcmp(routine, "wait_until") == 0) {
        shmem_long_wait_until(&ivar, SHMEM_CMP_EQ, 1);
    } else if (strcmp(routine, "barrier_all") == 0) {
        shmem_barrier_all();
    } else if (strcmp(routine, "putmem") == 0) {
        shmem_putmem(&dummy, &dummy, sizeof dummy, 0);
    } else if (strcmp(routine, "team_my_pe") == 0) {
        shmem_team_my_pe(SHMEM_TEAM_WORLD);
    } else if (strcmp(routine, "quiet") == 0) {
        shmem_quiet();
    } else if (strcmp(routine, "ptr") == 0) {
        shmem_ptr(&dummy, 0);
    } else if (strcmp(routine, "barrier") == 0) {
        static long pSync[SHMEM_BARRIER_SYNC_SIZE];
        shmem_barrier(0, 0, 1, pSync);
    } else if (strcmp(routine, "sum_to_all") == 0) {
        static long pSync[SHMEM_REDUCE_SYNC_SIZE], pWrk[SHMEM_REDUCE_MIN_WRKDATA_SIZE];
        shmem_long_sum_to_all(&ivar, &ivar, 1, 0, 0, 1, pWrk, pSync);
    }
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    if (strcmp(argv[1], "after") == 0) {
        shmem_init();
        shmem_finalize();
    }
    call(argv[2]);
    return 0;   /* reached only if the call did not fatal, which is the bug */
}
