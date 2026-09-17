/* Minimal test assertions shared by the tier-1 tests. */
#ifndef OSOE_TEST_CHECK_H
#define OSOE_TEST_CHECK_H

#include <stdio.h>

static int check_fails;

#define CHECK(cond) do {                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            check_fails++;                                                    \
        }                                                                     \
    } while (0)

#define CHECK_DONE() (check_fails != 0)

#endif /* OSOE_TEST_CHECK_H */
