/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A symmetric-heap call whose PEs pass different arguments ends the job by
 * name (P23; malloc.c's same_arguments), where until then each PE's heap went
 * its own way and a put to one PE's object landed in another of another PE's
 * (the release review's conformance C5).
 *
 *   test-malloc-divergent MODE
 *
 * In each mode PE 1 passes another argument than the rest, every argument
 * valid on its own PE: `malloc` another size; `hints` other hints; `calloc`
 * another count and size of the same product; `align` another alignment;
 * `realloc` another size for the same block; `free` another of two blocks.
 * Each ends the job, every PE naming what it passed; "every check passed" is
 * never reached. And `equivalent`, which must pass: shmem_realloc of a null
 * pointer on PE 1 where the rest call shmem_malloc, then shmem_realloc to size
 * 0 where the rest call shmem_free - the same calls by spec §9.3.3.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <shmem.h>

#include "check.h"

int main(int argc, char **argv)
{
    CHECK_FATAL(argc == 2);
    const char *mode = argv[1];
    shmem_init();
    int me = shmem_my_pe();
    bool odd = me == 1;

    if (strcmp(mode, "malloc") == 0) {
        shmem_malloc(odd ? 1008 : 1000);
    } else if (strcmp(mode, "hints") == 0) {
        shmem_malloc_with_hints(1000, odd ? SHMEM_MALLOC_ATOMICS_REMOTE : 0);
    } else if (strcmp(mode, "calloc") == 0) {
        shmem_calloc(odd ? 4 : 2, odd ? 250 : 500);
    } else if (strcmp(mode, "align") == 0) {
        shmem_align(odd ? 64 : 16, 1000);
    } else if (strcmp(mode, "realloc") == 0) {
        void *p = shmem_malloc(1000);
        CHECK_FATAL(p != NULL);
        shmem_realloc(p, odd ? 2000 : 3000);
    } else if (strcmp(mode, "free") == 0) {
        void *a = shmem_malloc(1000), *b = shmem_malloc(1000);
        CHECK_FATAL(a != NULL && b != NULL);
        shmem_free(odd ? b : a);
    } else if (strcmp(mode, "equivalent") == 0) {
        void *p = odd ? shmem_realloc(NULL, 1000) : shmem_malloc(1000);
        CHECK_FATAL(p != NULL);
        void *q = shmem_malloc(1000);
        CHECK(q != NULL && q != p);
        if (odd)
            CHECK(shmem_realloc(p, 0) == NULL);
        else
            shmem_free(p);
        shmem_free(q);
        shmem_barrier_all();
        if (me == 0)
            printf("every check passed\n");
    } else {
        fprintf(stderr, "test-malloc-divergent: no mode '%s'\n", mode);
        return 2;
    }
    shmem_finalize();
    return CHECK_DONE();
}
