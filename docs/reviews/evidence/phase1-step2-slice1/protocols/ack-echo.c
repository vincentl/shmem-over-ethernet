/*
 * Evidence for the acknowledgement echo (report.md, finding P1).
 *
 * Two logical nodes, one PE each. PE 0 does ONE put to PE 1 and ONE quiet, and
 * then both PEs sleep in the application for OSOE_ECHO_SLEEP_MS with no SHMEM
 * traffic at all. A private build instrumented with ack-echo.patch prints the
 * engine's segments_sent / acks_only counters at shmem_finalize. A quiescent
 * library sends a handful of segments; an echoing one sends thousands.
 */
#include <shmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(void)
{
    shmem_init();
    int me = shmem_my_pe();
    long *x = shmem_malloc(sizeof *x);
    *x = 0;
    shmem_barrier_all();
    const char *q = getenv("OSOE_ECHO_QUIET");
    if (me == 0 && q && q[0] == '1') {
        long v = 42;
        shmem_putmem(x, &v, sizeof v, 1);
        shmem_quiet();
    }
    const char *ms = getenv("OSOE_ECHO_SLEEP_MS");
    long sleep_ms = ms ? atol(ms) : 1000;
    struct timespec t = { sleep_ms / 1000, (sleep_ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
    shmem_free(x);
    shmem_finalize();
    return 0;
}
