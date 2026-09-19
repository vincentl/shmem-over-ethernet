/*
 * start_pes, and the implicit finalize it must carry (Annex F.2.2, F.2.3).
 *
 * This program deliberately never calls shmem_finalize, because a program
 * written against start_pes never did: finalization used to be implicit, and
 * that is the behaviour such a program still expects. So the test is not what
 * this program *checks* but what it *leaves behind*. `osoe-launch` fails a job
 * that leaks shared memory, so if start_pes did not register a finalize at
 * exit, the control segment and the heaps would still be in /dev/shm and this
 * test would fail without asserting anything at all.
 *
 * That matters beyond tidiness: a leaked segment is exactly the state
 * `osoe-stat --gc` exists to clean up after a *crash* (§3.14), and a program
 * that exits normally must not look like one that crashed.
 */
#include <stdint.h>
#include <stdlib.h>

#include <mpp/shmem.h>

#include "check.h"

int main(void)
{
    /* The argument has meant nothing for decades, and passing a wrong one must
     * not matter. */
    start_pes(0);

    int me = _my_pe(), n = _num_pes();
    CHECK(n > 0);
    CHECK(me >= 0 && me < n);

    const char *procid = getenv("OSOE_PROCID");
    CHECK(procid && me == atoi(procid));

    /* A second call is a no-op, not a second initialization: start_pes predates
     * the reference counting that makes a repeated shmem_init meaningful, and a
     * second implicit finalize would tear down a library the first call owns. */
    start_pes(99);
    CHECK(_my_pe() == me);
    CHECK(_num_pes() == n);

    /* The library really is up. */
    uint64_t *v = shmalloc(sizeof *v);
    CHECK(v != NULL);
    *v = 0;
    shmem_barrier_all();
    shmem_uint64_atomic_add(v, 1, (me + 1) % n);
    shmem_barrier_all();
    CHECK(*v == 1);
    shfree(v);

    /* No shmem_finalize, on purpose. What proves the implicit one ran is that
     * osoe-launch does not report leaked shared memory. */
    return CHECK_DONE();
}
