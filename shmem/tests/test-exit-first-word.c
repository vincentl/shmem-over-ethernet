/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * An exit without shmem_finalize waits for a first word another thread is
 * still writing (fatal.h; init.c, exit_without_finalize), as every end of a
 * process does.
 *
 *   exit-first-word.sh LAUNCHER test-exit-first-word
 *
 * One PE, whose standard error is a pipe the script reads only once the PE
 * has made MARK and a second more has passed. After shmem_init the PE fills
 * the pipe to the brim, has a thread take the first word into it - the write
 * then waiting for the script - and a tenth of a second on makes MARK and
 * returns from main without shmem_finalize. Its exit's own line loses the
 * word, and the abort it declares wakes its agent, whose "ending" line loses
 * it too (abort.c): an end that did not wait for the word, either, ended the
 * process before the script read, with the line unwritten. The script
 * requires that line and neither of the ends', and the PE's failure.
 * White-box: the first word is the library's own, osoe_fatal_first_word, so
 * the library's sources are compiled in (CMakeLists.txt).
 *
 *   test-exit-first-word MARK
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <shmem.h>

#include "check.h"
#include "fatal.h"

static const char held_word[] = "THE FIRST WORD\n";
static _Atomic bool taking;         /* the thread is about to take the first word */

static void *say_held_word(void *unused)
{
    (void)unused;
    atomic_store_explicit(&taking, true, memory_order_release);
    osoe_fatal_first_word(held_word, sizeof held_word - 1);
    return NULL;
}

static void sleep_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000 * 1000 };
    nanosleep(&t, NULL);
}

int main(int argc, char **argv)
{
    CHECK_FATAL(argc == 2);
    shmem_init();
    int flags = fcntl(STDERR_FILENO, F_GETFL);
    CHECK_FATAL(flags >= 0 && fcntl(STDERR_FILENO, F_SETFL, flags | O_NONBLOCK) == 0);
    char fill[4096];                    /* PIPE_BUF: each write all or nothing */
    memset(fill, 0, sizeof fill);
    while (write(STDERR_FILENO, fill, sizeof fill) > 0)
        ;
    CHECK_FATAL(errno == EAGAIN && fcntl(STDERR_FILENO, F_SETFL, flags) == 0);
    pthread_t t;
    CHECK_FATAL(pthread_create(&t, NULL, say_held_word, NULL) == 0);
    pthread_detach(t);
    while (!atomic_load_explicit(&taking, memory_order_acquire))
        sleep_ms(1);
    sleep_ms(100);
    FILE *mark = fopen(argv[1], "w");
    CHECK_FATAL(mark != NULL && fclose(mark) == 0);
    return 0;                           /* without shmem_finalize */
}
