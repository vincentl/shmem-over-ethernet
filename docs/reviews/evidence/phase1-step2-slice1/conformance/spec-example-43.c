/* OpenSHMEM 1.6 Example 43 (section 9.11.10), verbatim apart from this comment and the notice at the end. */
#include <shmem.h>
#include <stdlib.h>
int main(void) {
  shmem_init();
  int mype = shmem_my_pe();
  int npes = shmem_n_pes();
    int *flags = shmem_calloc(npes, sizeof(int));
    int *status = calloc(npes, sizeof(int));
    for (int i = 0; i < npes; i++)
      shmem_atomic_set(&flags[mype], 1, i);
    int ncompleted = 0;
    size_t completed_idx;
    while (ncompleted < npes) {
      completed_idx = shmem_test_any(flags, npes, status, SHMEM_CMP_EQ, 1);
      if (completed_idx != SIZE_MAX) {
        ncompleted++;
        status[completed_idx] = 1;
      }
      else {
        /* Overlap some computation here */
      }
    }
    free(status);
    shmem_free(flags);
    shmem_finalize();
    return 0;
}

/*
 * Notice. This program is Example 43 (section 9.11.10) of "OpenSHMEM Application
 * Programming Interface, Version 1.6" (1 November 2024), copied from it. The
 * specification belongs to Open Source Software Solutions, Inc. (OSSS), a
 * nonprofit organization, under an agreement with HPE, and permits copying all
 * or part of it without fee provided the OSSS notice and the document's title
 * appear and notice is given that the copying is by permission of OSSS. This
 * copy is made by permission of OSSS. The program is the specification's, not
 * osoe's, and so has no SPDX line.
 */
