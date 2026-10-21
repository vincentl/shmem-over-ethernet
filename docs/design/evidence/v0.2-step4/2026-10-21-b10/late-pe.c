/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * B10's test program: a job whose start is slow because one PE is late.
 *
 * A PE that finds B10_DELAY_SECONDS in its environment sleeps that long before
 * it calls shmem_init (osoe-launch -e RANK:NAME=VALUE gives it to one rank
 * only). The others call shmem_init at once and wait inside it for the late PE,
 * with the names the start has made - the node's control segment and the heaps
 * - still in /dev/shm, which is what systemd-logind's RemoveIPC removes when
 * the user's last login session has ended. Once every PE is in, each runs a
 * barrier and a ring of puts, checks what it was given, and finalizes.
 *
 * B10_HOLD_SECONDS, if set, makes every PE sleep that long once shmem_init has
 * returned, so that the job is running, past its start, when the user's last
 * login session has ended and logind's cleanup comes.
 *
 * Every line goes to standard error with the wall clock, so that a run's record
 * can be laid beside the journal's.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <shmem.h>

static void say(const char *format, ...)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    const char *rank = getenv("OSOE_PROCID");
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    fprintf(stderr, "%ld.%03ld rank %s: %s\n", (long)now.tv_sec, now.tv_nsec / 1000000,
            rank ? rank : "?", line);
}

int main(void)
{
    const char *delay = getenv("B10_DELAY_SECONDS");
    if (delay) {
        say("sleeping %s s before shmem_init", delay);
        sleep((unsigned)atoi(delay));
    }
    say("calling shmem_init");
    shmem_init();
    int me = shmem_my_pe();
    int npes = shmem_n_pes();
    say("shmem_init returned, PE %d of %d", me, npes);
    const char *hold = getenv("B10_HOLD_SECONDS");
    if (hold) {
        say("holding %s s after shmem_init", hold);
        sleep((unsigned)atoi(hold));
    }

    long *word = shmem_malloc(sizeof *word);
    int ok = word != NULL;
    if (ok) {
        *word = -1;
        shmem_barrier_all();
        shmem_long_p(word, me, (me + 1) % npes);
        shmem_barrier_all();
        ok = *word == (me + npes - 1) % npes;
        shmem_free(word);
    }
    say("ring %s", ok ? "ok" : "WRONG");
    shmem_finalize();
    return ok ? 0 : 1;
}
