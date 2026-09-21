/* Minimal test assertions shared by the tier-1 tests. */
#ifndef OSOE_TEST_CHECK_H
#define OSOE_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

static int check_fails;

#define CHECK(cond) do {                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            check_fails++;                                                    \
        }                                                                     \
    } while (0)

/*
 * CHECK, then leave at once with a nonzero status. For a check whose failure
 * would send the test on into a call that can no longer return - a wait for a
 * value that is now never coming - where a recorded failure turns into a hang
 * until the launcher's timeout, and the timeout is what gets reported rather
 * than the check (review round two, C9). _Exit, not exit: an atexit handler
 * such as start_pes's finalize would run a barrier the other PEs never reach.
 */
#define CHECK_FATAL(cond) do {                                                \
        (void)check_fails;          /* a test may use only this form */      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "%s:%d: CHECK(%s) failed; stopping here\n",       \
                    __FILE__, __LINE__, #cond);                               \
            fflush(stderr);                                                   \
            _Exit(1);                                                         \
        }                                                                     \
    } while (0)

#define CHECK_DONE() (check_fails != 0)

#endif /* OSOE_TEST_CHECK_H */
