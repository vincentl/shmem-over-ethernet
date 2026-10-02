/* OpenSHMEM 1.6 Example 5 (section 9.1.1), the specification's first complete
 * program, verbatim apart from this comment and the notice at the end. */
#include <shmem.h>
#include <stdio.h>
int main(void) {
  static int targ = 0;
    shmem_init();
    int mype = shmem_my_pe();
    int receiver = 1 % shmem_n_pes();
    if (mype == 0) {
      int src = 33;
      shmem_put(&targ, &src, 1, receiver);
    }
    shmem_barrier_all(); /* Synchronizes sender and receiver */
    if (mype == receiver)
      printf("PE %d targ=%d (expect 33)\n", mype, targ);
    shmem_finalize();
    return 0;
}

/*
 * Notice. This program is Example 5 (section 9.1.1) of "OpenSHMEM Application
 * Programming Interface, Version 1.6" (1 November 2024), copied from it. The
 * specification belongs to Open Source Software Solutions, Inc. (OSSS), a
 * nonprofit organization, under an agreement with HPE, and permits copying all
 * or part of it without fee provided the OSSS notice and the document's title
 * appear and notice is given that the copying is by permission of OSSS. This
 * copy is made by permission of OSSS. The program is the specification's, not
 * osoe's, and so has no SPDX line.
 */
